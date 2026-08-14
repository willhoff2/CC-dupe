/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 TheSuperHackers
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

//////////////////////////////////////////////////////////////////////////////
// Native stack walk test.
//
// Verifies the claim the port makes about the debug library off Windows: that DebugStackwalk
// really walks the stack and really symbolises it, rather than compiling and returning nothing.
// It is deliberately an executable rather than a compile-time check, because "the stack walker
// links" and "the stack walker produces frames that name my functions" are different claims and
// only the second one is useful when a native build crashes.
//
// Driven by scripts/native-stackwalk-test.py, which also links it. See
// docs/porting/debug-and-profile-libs.md.
//////////////////////////////////////////////////////////////////////////////

// relative, like the sample programs in this tree: the probe compiles this file with only its own
// directory on the include path
#include "../debug.h"
#include <stdio.h>
#include <string.h>

static int g_failures;

static void Check(bool condition, const char *what)
{
  printf("%-6s %s\n", condition ? "ok" : "FAIL", what);
  if (!condition)
    ++g_failures;
}

// The frames we expect to find. Not static: dladdr() and backtrace_symbols() can only name a
// function the linker put in a symbol table, which is also true of every engine function whose
// name we want out of a native crash.
extern void StackwalkTestInnermost(DebugStackwalk::Signature &sig);
extern void StackwalkTestMiddle(DebugStackwalk::Signature &sig);
extern void StackwalkTestOutermost(DebugStackwalk::Signature &sig);

__attribute__((noinline)) void StackwalkTestInnermost(DebugStackwalk::Signature &sig)
{
  const int found = DebugStackwalk::StackWalk(sig);
  Check(found > 0, "StackWalk() returned at least one frame");
  Check(sig.Size() == (unsigned)found, "Signature::Size() agrees with the return value");
}

__attribute__((noinline)) void StackwalkTestMiddle(DebugStackwalk::Signature &sig)
{
  StackwalkTestInnermost(sig);
  // keeps the call from being turned into a tail call, which would drop this frame
  asm volatile("" ::: "memory");
}

__attribute__((noinline)) void StackwalkTestOutermost(DebugStackwalk::Signature &sig)
{
  StackwalkTestMiddle(sig);
  asm volatile("" ::: "memory");
}

int main()
{
  DebugStackwalk::Signature sig;
  StackwalkTestOutermost(sig);

  // Every frame must be a plausible code address, and one whole-signature dump must contain the
  // names of all three test functions: that is the part a stub cannot fake.
  bool allAddressesLookLikeCode = sig.Size() > 0;
  char joined[256 * 300];
  *joined = 0;
  for (unsigned k = 0; k < sig.Size(); ++k)
  {
    if (sig.GetAddress(k) == 0)
      allAddressesLookLikeCode = false;

    char buf[256];
    DebugStackwalk::Signature::GetSymbol(sig.GetAddress(k), buf, sizeof(buf));
    printf("  #%02u %s\n", k, buf);
    if (strlen(joined) + strlen(buf) + 2 < sizeof(joined))
    {
      strcat(joined, buf);
      strcat(joined, "\n");
    }
  }

  Check(allAddressesLookLikeCode, "every frame has a non-zero address");
  Check(strstr(joined, "StackwalkTestInnermost") != nullptr,
        "the innermost test function is named in the symbolised stack");
  Check(strstr(joined, "StackwalkTestMiddle") != nullptr,
        "its caller is named too, so this is a walk and not a single frame");
  Check(strstr(joined, "StackwalkTestOutermost") != nullptr,
        "and its caller as well");
  Check(strstr(joined, "main") != nullptr, "main() is in there, so the walk reaches the bottom");

  // The split overload is what debug_debug.cpp's frame reporting and the crash log use, so check
  // it resolves a module and a symbol with sane relative offsets rather than just a string.
  char mod[256], sym[256], file[256];
  unsigned relMod = 0xffffffff, relSym = 0xffffffff, line = 0xffffffff, relLine = 0xffffffff;
  DebugStackwalk::Signature::GetSymbol(sig.GetAddress(0),
                                       mod, sizeof(mod), &relMod,
                                       sym, sizeof(sym), &relSym,
                                       file, sizeof(file), &line, &relLine);
  printf("  module=%s+%u symbol=%s+%u\n", mod, relMod, sym, relSym);
  Check(*mod != 0, "the frame resolves to a module");
  Check(strstr(mod, "stackwalk") != nullptr || strstr(mod, "test") != nullptr,
        "and that module is this test binary");
  // demangled, so the C++ signature is part of the name: "StackwalkTestInnermost(...)"
  Check(strncmp(sym, "StackwalkTestInnermost", strlen("StackwalkTestInnermost")) == 0,
        "the innermost frame's symbol is the function it is in");
  Check(relMod != 0xffffffff && relSym != 0xffffffff,
        "both relative offsets were written");
  Check(relSym < 0x10000, "the offset within the symbol is small, i.e. it is the right symbol");

  // A Signature must survive being copied, since that is how the frame hash stores them.
  DebugStackwalk::Signature copy(sig);
  bool copiedFaithfully = copy.Size() == sig.Size();
  for (unsigned k = 0; copiedFaithfully && k < sig.Size(); ++k)
    copiedFaithfully = copy.GetAddress(k) == sig.GetAddress(k);
  Check(copiedFaithfully, "Signature copies preserve every address");

  printf(g_failures ? "\nFAILED (%d)\n" : "\nPASSED\n", g_failures);
  return g_failures ? 1 : 0;
}
