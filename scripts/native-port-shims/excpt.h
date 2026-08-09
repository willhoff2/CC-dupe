// Declaration-only stand-in for scripts/native-port-probe.py. See README.md.
#pragma once

#include <windows.h>

// Deliberately does NOT define `__try` / `__except` / `__finally`. libstdc++ defines `__try`
// and `__catch` as macros over real C++ `try`/`catch`, so any definition here would rewrite
// every `try` block in the standard library and bury the measurement in tens of thousands of
// spurious errors. Structured exception handling is a real port problem, not a shim problem:
// exactly three translation units use it (`WWLib/Except.cpp`, `WWLib/thread.cpp`,
// `Common/System/MiniDumper.cpp`), and the probe should report them as such.

#define EXCEPTION_MAXIMUM_PARAMETERS 15
