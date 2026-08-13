// Forwarding stand-in for scripts/native-port-probe.py. See README.md.
//
// "gnu_regex.h" is the header of the GNU regex library the MSVC build vendors. That library's API
// -- regex_t, regfree, re_set_syntax, re_compile_pattern, re_match and the RE_* syntax bits -- is
// what glibc ships in <regex.h> as its GNU extension set, so there is nothing to reimplement:
// regexpr.cpp compiles and works against the C library directly.
//
// Not available on the BSD C library, and so not on macOS, which has the POSIX subset
// (regcomp/regexec) but none of the re_* entry points. WWLib's CMakeLists does not compile
// regexpr.cpp in any configuration, so this is not on the path to a running game; when it becomes
// one, the answer is to use regcomp/regexec via RegularExpressionClass rather than to port GNU
// regex. Written on Linux, not compiled on macOS.
#pragma once

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <regex.h>
