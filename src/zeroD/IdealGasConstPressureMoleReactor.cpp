//! @file IdealGasConstPressureMoleReactor.cpp A constant pressure
//! zero-dimensional reactor with moles as the state

// This file is part of Cantera. See License.txt in the top-level directory or
// at https://cantera.org/license.txt for license and copyright information.

#include "cantera/zeroD/IdealGasConstPressureMoleReactor.h"
#include "cantera/zeroD/FlowDevice.h"
#include "cantera/zeroD/ReactorNet.h"
#include "cantera/zeroD/ReactorSurface.h"
#include "cantera/kinetics/Kinetics.h"
#include "cantera/thermo/ThermoPhase.h"
#include "cantera/thermo/SurfPhase.h"
#include "cantera/base/utilities.h"
#include <limits>

namespace Cantera
{

void IdealGasConstPressureMoleReactor::setThermoMgr(ThermoPhase& thermo)
{
    if (thermo.type() != "ideal-gas") {
        throw CanteraError("IdealGasConstPressureMoleReactor::setThermoMgr",
                           "Incompatible phase type provided");
    }
    ConstPressureMoleReactor::setThermoMgr(thermo);
}

void IdealGasConstPressureMoleReactor::getState(double* y)
{
    if (m_thermo == 0) {
        throw CanteraError("IdealGasConstPressureMoleReactor::getState",
                           "Error: reactor is empty.");
    }
    m_thermo->restoreState(m_state);
    // get mass for calculations
    m_mass = m_thermo->density() * m_vol;
    // set the first component to the temperature
    y[0] = m_thermo->temperature();
    // get moles of species in remaining state
    getMoles(y + m_sidx);
    // set the remaining components to the surface species moles on the walls
    getSurfaceInitialConditions(y + m_nsp + m_sidx);
}

void IdealGasConstPressureMoleReactor::initialize(double t0)
{
    ConstPressureMoleReactor::initialize(t0);
    m_hk.resize(m_nsp, 0.0);
}

void IdealGasConstPressureMoleReactor::updateState(double* y)
{
    // the components of y are: [0] the temperature, [1...K+1) are the
    // moles of each species, and [K+1...] are the moles of surface
    // species on each wall.
    setMassFromMoles(y + m_sidx);
    m_thermo->setMolesNoTruncate(y + m_sidx);
    m_thermo->setState_TP(y[0], m_pressure);
    m_vol = m_mass / m_thermo->density();
    updateConnected(false);
    updateSurfaceState(y + m_nsp + m_sidx);
}

void IdealGasConstPressureMoleReactor::eval(double time, double* LHS, double* RHS)
{
    double& mcpdTdt = RHS[0]; // m * c_p * dT/dt
    double* dndt = RHS + m_sidx; // kmol per s

    evalWalls(time);

    m_thermo->restoreState(m_state);

    m_thermo->getPartialMolarEnthalpies(&m_hk[0]);
    const vector_fp& imw = m_thermo->inverseMolecularWeights();

    if (m_chem) {
        m_kin->getNetProductionRates(&m_wdot[0]); // "omega dot"
    }

    // evaluate reactor surfaces
    evalSurfaces(LHS + m_nsp + m_sidx, RHS + m_nsp + m_sidx, m_sdot.data());

    // external heat transfer
    mcpdTdt += m_Qdot;

    for (size_t n = 0; n < m_nsp; n++) {
        // heat release from gas phase and surface reactions
        mcpdTdt -= m_wdot[n] * m_hk[n] * m_vol;
        mcpdTdt -= m_sdot[n] * m_hk[n];
        // production in gas phase and from surfaces
        dndt[n] = m_wdot[n] * m_vol + m_sdot[n];
    }

    // add terms for outlets
    for (auto outlet : m_outlet) {
        for (size_t n = 0; n < m_nsp; n++) {
            // flow of species into system and dilution by other species
            dndt[n] -= outlet->outletSpeciesMassFlowRate(n) * imw[n];
        }
    }

    // add terms for inlets
    for (auto inlet : m_inlet) {
        double mdot = inlet->massFlowRate();
        mcpdTdt += inlet->enthalpy_mass() * mdot;
        for (size_t n = 0; n < m_nsp; n++) {
            double mdot_spec = inlet->outletSpeciesMassFlowRate(n);
            // flow of species into system and dilution by other species
            dndt[n] += inlet->outletSpeciesMassFlowRate(n) * imw[n];
            mcpdTdt -= m_hk[n] * imw[n] * mdot_spec;
        }
    }

    if (m_energy) {
        LHS[0] = m_mass * m_thermo->cp_mass();
    } else {
        RHS[0] = 0.0;
    }
}

Eigen::SparseMatrix<double> IdealGasConstPressureMoleReactor::jacobian()
{
    if (m_nv == 0) {
        throw CanteraError("IdealGasConstPressureMoleReactor::jacobian",
                           "Reactor must be initialized first.");
    }
    // clear former jacobian elements
    m_jac_trips.clear();
    // Determine Species Derivatives
    // volume / moles * rates portion of equation
    Eigen::VectorXd netProductionRates(m_nsp);
    m_kin->getNetProductionRates(netProductionRates.data()); // "omega dot"
    Eigen::SparseMatrix<double> dnk_dnj = m_kin->netProductionRates_ddN();
    double molarVolume = m_thermo->molarVolume();
    // Calculate ROP derivatives, excluding the terms where dnk/dnj is zero but
    // molarVolume * wdot is not, as it reduces matrix sparsity and diminishes
    // performance.
    for (int k = 0; k < dnk_dnj.outerSize(); k++) {
        for (Eigen::SparseMatrix<double>::InnerIterator it(dnk_dnj, k); it; ++it) {
            it.valueRef() = it.value() + netProductionRates[it.row()] * molarVolume;
            m_jac_trips.emplace_back(static_cast<int>(it.row() + m_sidx),
                static_cast<int>(it.col() + m_sidx), it.value());
        }
    }

    // Temperature Derivatives
    double NCp = 0.0;
    if (m_energy) {
        // getting perturbed state for finite difference
        double deltaTemp = m_thermo->temperature()
            * std::sqrt(std::numeric_limits<double>::epsilon());
        // finite difference temperature derivatives
        vector_fp lhsPerturbed(m_nv, 1.0), lhsCurrent(m_nv, 1.0);
        vector_fp rhsPerturbed(m_nv, 0.0), rhsCurrent(m_nv, 0.0);
        vector_fp yCurrent(m_nv);
        getState(yCurrent.data());
        vector_fp yPerturbed = yCurrent;
        // perturb temperature
        yPerturbed[0] += deltaTemp;
        // getting perturbed state
        updateState(yPerturbed.data());
        double time = (m_net != nullptr) ? m_net->time() : 0.0;
        eval(time, lhsPerturbed.data(), rhsPerturbed.data());
        // reset and get original state
        updateState(yCurrent.data());
        eval(time, lhsCurrent.data(), rhsCurrent.data());
        // d ydot_j/dT
        for (size_t j = 0; j < m_nv; j++) {
            double ydotPerturbed = rhsPerturbed[j] / lhsPerturbed[j];
            double ydotCurrent = rhsCurrent[j] / lhsCurrent[j];
            m_jac_trips.emplace_back(static_cast<int>(j), 0,
                                     (ydotPerturbed - ydotCurrent) / deltaTemp);
        }
        // d T_dot/dnj
        Eigen::VectorXd enthalpy(m_nsp);
        m_thermo->getPartialMolarEnthalpies(enthalpy.data());
        // get specific heat for whole system
        Eigen::VectorXd specificHeat(m_nv - m_sidx);
        // gas phase
        m_thermo->getPartialMolarCp(specificHeat.data());
        // surface phases
        size_t shift = m_nsp;
        for (auto S : m_surfaces) {
            S->thermo()->getPartialMolarCp(specificHeat.data() + shift);
            shift += S->thermo()->nSpecies();
        }
        double qdot = m_vol * enthalpy.dot(netProductionRates);
        // find denominator ahead of time
        for (size_t i = 0; i < m_nv - m_sidx; i++) {
            NCp += yCurrent[i + m_sidx] * specificHeat[i];
        }
        double denom = 1 / (NCp * NCp);
        Eigen::VectorXd hk_dnkdnj_sum = dnk_dnj.transpose() * enthalpy;
        // Add derivatives to jac by spanning columns
        for (size_t j = 0; j < m_nsp; j++) {
            m_jac_trips.emplace_back(0, static_cast<int>(j + m_sidx),
                (specificHeat[j] * qdot - NCp * hk_dnkdnj_sum[j]) * denom);
        }
    }
    // add surface jacobian to system
    addSurfJacobian(NCp, true);
    // convert triplets to sparse matrix
    Eigen::SparseMatrix<double> jac(m_nv, m_nv);
    jac.setFromTriplets(m_jac_trips.begin(), m_jac_trips.end());
    return jac;
}

size_t IdealGasConstPressureMoleReactor::componentIndex(const string& nm) const
{
    size_t k = speciesIndex(nm);
    if (k != npos) {
        return k + m_sidx;
    } else if (nm == "temperature") {
        return 0;
    } else {
        return npos;
    }
}

std::string IdealGasConstPressureMoleReactor::componentName(size_t k) {
    if (k == 0) {
        return "temperature";
    } else if (k >= m_sidx && k < neq()) {
        k -= m_sidx;
        if (k < m_thermo->nSpecies()) {
            return m_thermo->speciesName(k);
        } else {
            k -= m_thermo->nSpecies();
        }
        for (auto& S : m_surfaces) {
            ThermoPhase* th = S->thermo();
            if (k < th->nSpecies()) {
                return th->speciesName(k);
            } else {
                k -= th->nSpecies();
            }
        }
    }
    throw CanteraError("IdealGasConstPressureMoleReactor::componentName",
                       "Index is out of bounds.");
}

}
