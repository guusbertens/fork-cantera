# This file is part of Cantera. See License.txt in the top-level directory or
# at https://cantera.org/license.txt for license and copyright information.

import inspect
from pathlib import Path
import textwrap

def normalize_indent(code: str) -> str:
    code = textwrap.dedent(code).strip()

    indent = 0

    call_line = inspect.stack()[1].code_context[0]

    indent = len(call_line) - len(call_line.lstrip())

    # If called inside a string interpolation, indent to the rest of the block.
    # Look for the opening brace for the interpolation, which isn't perfect, but works.
    # This will fire for lines such as “        {normalize(my_str)}”
    if call_line[indent] == '{':
        code = textwrap.indent(code, ' ' * indent)

        code = code[indent:]

    return code

def get_preamble() -> str:
    return Path(__file__).parent.joinpath("preamble.txt").read_text()
