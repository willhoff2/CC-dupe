// Forwarding stand-in for scripts/native-port-probe.py. See README.md.
//
// MSVC's <mbstring.h> is the multibyte string CRT. IMEManager.cpp includes it for _mbsnccnt();
// the portable implementation lives in Dependencies/Utility, not here, because it is real code
// rather than a declaration.
#pragma once

#include <Utility/mbstring_compat.h>
