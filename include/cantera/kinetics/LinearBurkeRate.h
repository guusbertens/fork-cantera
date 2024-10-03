//! @file LinearBurkeRate.h
// This file is part of Cantera. See License.txt in the top-level directory or
// at https://cantera.org/license.txt for license and copyright information.

#ifndef CT_LINEARBURKERATE_H
#define CT_LINEARBURKERATE_H
#include "cantera/kinetics/Arrhenius.h"
#include <boost/variant.hpp>
#include "cantera/kinetics/Falloff.h"
#include "cantera/kinetics/ChebyshevRate.h"
#include "cantera/kinetics/PlogRate.h"

namespace Cantera
{

//! Data container holding shared data specific to LinearBurkeRate
/**
 * The data container `LinearBurkeData` holds precalculated data common to
 * all `LinearBurkeRate` objects.
 */
struct LinearBurkeData : public ReactionData
{
    LinearBurkeData();

    void update(double T, double P) override
    {
        ReactionData::update(T);
        pressure = P;
        logP = std::log(P);
    }

    bool update(const ThermoPhase& phase, const Kinetics& kin) override;

    using ReactionData::update;

    //! Perturb pressure of data container
    /**
     * The method is used for the evaluation of numerical derivatives.
     * @param  deltaP  relative pressure perturbation
     */
    void perturbPressure(double deltaP);

    void restore() override;

    virtual void resize(size_t nSpecies, size_t nReactions, size_t nPhases) override
    {
        moleFractions.resize(nSpecies, NAN);
        ready = true;
    }

    void invalidateCache() override
    {
        ReactionData::invalidateCache();
        pressure = NAN;
    }

    double pressure = NAN; //!< Pressure
    double logP = 0.0; //!< Logarithm of pressure
    bool ready = false; //!< Boolean indicating whether vectors are accessible
    vector<double> moleFractions;
    int mf_number;

protected:
    double m_pressure_buf = -1.0;
};


//! Pressure-dependent and composition-dependent reaction rate calculated
//! according to the reduced-pressure linear mixture rule (LMR-R) developed
//! at Columbia University. @cite singal2025
class LinearBurkeRate final : public ReactionRate
{
public:
    //! Default constructor.
    LinearBurkeRate() = default;

    LinearBurkeRate(const AnyMap& node, const UnitStack& rate_units={});

    unique_ptr<MultiRateBase> newMultiRate() const override {
        return make_unique<MultiRate<LinearBurkeRate, LinearBurkeData>>();
    }

    //! Identifier of reaction rate type
    const string type() const override { return "linear-burke"; }

    //! Perform object setup based on AnyMap node information
    /*!
     *  @param node  AnyMap containing rate information
     *  @param rate_units  Unit definitions specific to rate information
     */
    void setParameters(const AnyMap& node, const UnitStack& rate_units) override;

    void getParameters(AnyMap& rateNode) const override;

    //! Create type aliases that refer to Plog, Troe, and Chebyshev
    using RateTypes = boost::variant<PlogRate, TroeRate, ChebyshevRate>;
    using DataTypes = boost::variant<PlogData, FalloffData, ChebyshevData>;

    //! Evaluate overall reaction rate, using Troe/PLOG/Chebyshev to evaluate
    //! pressure-dependent aspect of the reaction
    /*!
     *  @param shared_data  data shared by all reactions of a given type
     */
    double evalPlogRate(const LinearBurkeData& shared_data, DataTypes& dataObj, RateTypes& rateObj);
    double evalTroeRate(const LinearBurkeData& shared_data, DataTypes& dataObj, RateTypes& rateObj);
    double evalChebyshevRate(const LinearBurkeData& shared_data, DataTypes& dataObj, RateTypes& rateObj);
    double evalFromStruct(const LinearBurkeData& shared_data);

    void setContext(const Reaction& rxn, const Kinetics& kin) override;

    void validate(const string& equation, const Kinetics& kin) override;

    //! String name of each collider, appearing in the same order as that of the
    //! original reaction input.
    vector<string> colliderNames;

    //! Index of each collider in the kinetics object species list where the vector
    //! elements appear in the same order as that of the original reaction input.
    vector<size_t> colliderIndices;

    //! Allows data from setParameters() to be later accessed by getParameters()
    map<string, AnyMap> colliderInfo;

    //! Third-body collision efficiency objects (eps = eig0_i/eig0_M)
    vector<ArrheniusRate> epsObjs1; //!< used for k(T,P,X) and eig0_mix calculation
    vector<ArrheniusRate> epsObjs2; //!< used for logPeff calculation
    ArrheniusRate epsObj_M; //!< used just for M (eig0_M/eig0_M = 1 always)

    //! Stores rate objects corresponding to each collider, which can be either
    //! PlogRate, TroeRate, or ChebyshevRate
    vector<RateTypes> rateObjs; //!< list for non-M colliders
    RateTypes rateObj_M; //!< collider M

    //! Stores data objects corresponding to each collider, which can be either
    //! PlogData, TroeData, or ChebyshevData
    vector<DataTypes> dataObjs; //!< list for non-M colliders
    DataTypes dataObj_M; //!< collider M

    size_t nSpecies; //!< total number of species in the kinetics object
    double logPeff_; //! effective pressure as a function of eps
    double eps_mix; //! mole-fraction-weighted overall eps value of the mixtures
};

}
#endif