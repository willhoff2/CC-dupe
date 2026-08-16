#!/usr/bin/env python3
"""Prove that wide text crossing a file, a save game, a CRC or the wire is 16 bits wide.

`WideChar` is `wchar_t`: 2 bytes with MSVC, 4 bytes on macOS and Linux. Every external format the
game has stores 16 bit UTF-16 code units, because that is what the Windows build wrote. Code that
sized those units with `sizeof(WideChar)` therefore consumed twice the bytes that exist once
`wchar_t` is 4 bytes - the first native run desynchronised the `.csf` string table that way, reading
the next record id out of the middle of the next label's text.

`Common/WideCharWire.h` is the one conversion. This check COMPILES AND RUNS it twice, once with a 4
byte `wchar_t` (the native width) and once with `-fshort-wchar` (the MSVC width), and requires that

1. both runs pass every case: `.csf` round trip, save/load round trip, astral code points, malformed
   surrogates, truncation that never splits a pair;
2. the two runs produce byte-identical external data - the same `.csf` bytes, the same save blob,
   the same CRC input, the same wire bytes - which is what "the Windows build stays the oracle"
   means here;
3. with a 2 byte `wchar_t` the conversion is a plain copy of the source units, so nothing about the
   Windows bytes can change;
4. the NEGATIVE CONTROL fails: the same `.csf` reader sized the old way, with `sizeof(WideChar)`,
   must desynchronise at a 4 byte `wchar_t` and must agree at 2 bytes. A test that passes on the
   broken code is not a test.

`-fshort-wchar` is how the MSVC width is reproduced on Linux. It is not how the game is built; it is
here so the two widths can be compared.

Usage:
    python3 scripts/ci/check-widechar-wire.py [--verbose]
"""

import argparse
import pathlib
import subprocess
import sys
import tempfile

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "scripts"))

import native_probe_targets as probe  # noqa: E402  (needs REPO_ROOT on the path first)

TEST_TU = r"""
// Runs the real Common/WideCharWire.h against the shapes the external formats actually have.
#include "Common/WideCharWire.h"

#include <stdio.h>
#include <string.h>

static int g_failures = 0;

static void check(bool ok, const char *what)
{
    if (!ok) {
        printf("    [FAIL] %s\n", what);
        ++g_failures;
    }
}

// ---------------------------------------------------------------------------------------------
// A miniature .csf string table: a count of 16 bit units, then the units, bit-inverted the way
// the real format stores them. Two records, so a mis-sized read of the first desynchronises the
// second - which is exactly what happened on the first native run.
// ---------------------------------------------------------------------------------------------

struct Csf
{
    unsigned char bytes[512];
    int size;

    Csf() : size(0) {}

    void putUnit(unsigned short unit)
    {
        bytes[size++] = (unsigned char)(unit & 0xFF);
        bytes[size++] = (unsigned char)(unit >> 8);
    }
    void putRecord(const WideWireChar *units, int count)
    {
        putUnit((unsigned short)count);
        for (int i = 0; i < count; ++i)
            putUnit((unsigned short)~units[i]);
    }
    unsigned short getUnit(int at) const
    {
        return (unsigned short)(bytes[at] | (bytes[at + 1] << 8));
    }
};

/// Reads one record the way GameText.cpp does now: units, not sizeof(WideChar).
static int csfReadRecord(const Csf &csf, int &at, WideChar *dest, int destCount)
{
    const int len = csf.getUnit(at);
    at += 2;

    WideWireChar wire[256];
    for (int i = 0; i < len; ++i)
        wire[i] = (WideWireChar)~csf.getUnit(at + i * 2);
    at += wideCharWireBytes(len);

    const int chars = wireToWideChar(dest, destCount - 1, wire, len);
    dest[chars] = 0;
    return chars;
}

/// The pre-port reader: the byte count came from sizeof(WideChar). THE NEGATIVE CONTROL.
static int csfReadRecordOldWay(const Csf &csf, int &at, WideChar *dest, int destCount)
{
    const int len = csf.getUnit(at);
    at += 2;

    int chars = 0;
    for (int i = 0; i < len && chars < destCount - 1; ++i)
        dest[chars++] = (WideChar)~csf.getUnit(at + i * 2);
    at += len * (int)sizeof(WideChar);   // the bug

    dest[chars] = 0;
    return chars;
}

static void printBytes(const char *label, const unsigned char *bytes, int size)
{
    printf("    %s %d bytes:", label, size);
    for (int i = 0; i < size; ++i)
        printf(" %02X", bytes[i]);
    printf("\n");
}

static int wireLen(const WideWireChar *units)
{
    int n = 0;
    while (units[n] != 0)
        ++n;
    return n;
}

int main(void)
{
    printf("  sizeof(wchar_t) = %d\n", (int)sizeof(WideChar));

    // ---- the .csf crossing, and the negative control -----------------------------------------
    // "GUI:SinglePlayer" is the label the first native run walked into; 12 units is the length it
    // over-read.
    const WideWireChar first[]  = { 'S','i','n','g','l','e',' ','P','l','a','y','e', 0 };
    const WideWireChar second[] = { 'S','k','i','r','m','i','s','h', 0 };

    Csf csf;
    csf.putRecord(first, wireLen(first));
    const int secondRecordAt = csf.size;
    csf.putRecord(second, wireLen(second));

    {
        int at = 0;
        WideChar text[256];
        const int chars = csfReadRecord(csf, at, text, 256);
        check(chars == wireLen(first), ".csf first record length");
        for (int i = 0; i < chars; ++i)
            check(text[i] == (WideChar)first[i], ".csf first record text");
        check(at == secondRecordAt, ".csf reader left the file positioned at the next record");

        const int chars2 = csfReadRecord(csf, at, text, 256);
        check(chars2 == wireLen(second), ".csf second record length");
        for (int i = 0; i < chars2; ++i)
            check(text[i] == (WideChar)second[i], ".csf second record text");
        check(at == csf.size, ".csf reader consumed the whole table");
    }

    // The old sizing. It must be wrong at a 4 byte wchar_t and right at 2 bytes, otherwise the
    // test above proves nothing.
    {
        int at = 0;
        WideChar text[256];
        csfReadRecordOldWay(csf, at, text, 256);
        const bool desynced = (at != secondRecordAt);
        if (sizeof(WideChar) == 2)
            check(!desynced, "negative control: the old sizing is correct at a 2 byte wchar_t");
        else
            check(desynced, "negative control: the old sizing must desynchronise at 4 bytes");
        if (desynced)
            printf("    (negative control desynchronised by %d bytes, as expected)\n",
                   at - secondRecordAt);
    }

    // ---- a save game / CRC blob: a unit count, then the units --------------------------------
    // XferSave, XferLoad and XferCRC all serialise a UnicodeString this shape.
    {
        const WideChar text[] = { 'p','l','a','y','e','r',' ','1', 0 };
        unsigned char blob[512];
        int size = 0;

        const int units = wideCharWireUnitCount(text, WIDECHAR_WIRE_NUL_TERMINATED);
        blob[size++] = (unsigned char)units;
        WideWireChar wire[WIDECHAR_WIRE_MAX_UNITS];
        const int written = wideCharToWire(wire, WIDECHAR_WIRE_MAX_UNITS, text,
                                           WIDECHAR_WIRE_NUL_TERMINATED);
        check(written == units, "save: the unit count matches what was encoded");
        memcpy(blob + size, wire, wideCharWireBytes(written));
        size += wideCharWireBytes(written);

        printBytes("save blob", blob, size);

        // Read it back, as XferLoad does.
        const int len = blob[0];
        WideChar back[256];
        const int chars = wireToWideChar(back, 255, (const WideWireChar *)(blob + 1), len);
        back[chars] = 0;
        check(chars == 8, "save: round trip length");
        for (int i = 0; i < chars; ++i)
            check(back[i] == text[i], "save: round trip text");
    }

    // ---- a 2 byte wchar_t must be a plain copy, i.e. the Windows bytes cannot move ------------
    if (sizeof(WideChar) == 2) {
        const WideChar text[] = { 'a', 0x00E9, 0x4E2D, 0xD83D, 0xDE00, 0 };
        WideWireChar wire[16];
        const int units = wideCharToWire(wire, 16, text, WIDECHAR_WIRE_NUL_TERMINATED);
        check(units == 5, "MSVC width: the unit count is the WideChar count");
        check(memcmp(wire, text, wideCharWireBytes(units)) == 0,
              "MSVC width: the conversion is a byte for byte copy");

        WideChar back[16];
        const int chars = wireToWideChar(back, 15, wire, units);
        check(chars == units && memcmp(back, text, wideCharWireBytes(units)) == 0,
              "MSVC width: reading back is a byte for byte copy");
    }

    // ---- astral code points, malformed surrogates, truncation --------------------------------
    {
        // U+1F600. One WideChar natively, one surrogate pair on the wire, and either way two units.
        WideChar grin[4];
        if (sizeof(WideChar) == 2) {
            grin[0] = (WideChar)0xD83D;
            grin[1] = (WideChar)0xDE00;
            grin[2] = 0;
        } else {
            grin[0] = (WideChar)0x1F600;
            grin[1] = 0;
        }

        WideWireChar wire[8];
        const int units = wideCharToWire(wire, 8, grin, WIDECHAR_WIRE_NUL_TERMINATED);
        check(units == 2, "astral: two code units on the wire");
        check(wire[0] == 0xD83D && wire[1] == 0xDE00, "astral: the expected surrogate pair");
        check(wideCharWireUnitCount(grin, WIDECHAR_WIRE_NUL_TERMINATED) == 2,
              "astral: the unit count agrees with the encoder");

        WideChar back[8];
        const int chars = wireToWideChar(back, 7, wire, units);
        check(chars == (sizeof(WideChar) == 2 ? 2 : 1), "astral: round trip length");
        check(back[0] == grin[0], "astral: round trip value");

        // Only room for one unit: rather than half a pair, nothing is written.
        WideWireChar tight[2] = { 0xFFFF, 0xFFFF };
        const int cut = wideCharToWire(tight, 1, grin, WIDECHAR_WIRE_NUL_TERMINATED);
        check(cut == 0, "truncation: a pair that does not fit is dropped whole");

        // A lone surrogate has no UTF-16 representation. It becomes U+FFFD, natively; with a 2 byte
        // wchar_t a lone low surrogate does too, and a lone high surrogate ends the text.
        WideChar lone[3] = { (WideChar)0xDE00, 'x', 0 };
        WideWireChar out[8];
        const int n = wideCharToWire(out, 8, lone, WIDECHAR_WIRE_NUL_TERMINATED);
        check(n == 2 && out[0] == 0xFFFD && out[1] == 'x',
              "malformed: a lone low surrogate becomes U+FFFD");

        // The same on the way in.
        const WideWireChar half[2] = { 0xD83D, 'x' };
        WideChar decoded[8];
        const int m = wireToWideChar(decoded, 7, half, 2);
        check(m == 2, "malformed: a lone high surrogate on the wire still yields two characters");
        if (sizeof(WideChar) != 2)
            check(decoded[0] == (WideChar)0xFFFD,
                  "malformed: a lone high surrogate decodes to U+FFFD");
    }

    // ---- the bytes any other build must see --------------------------------------------------
    // Printed, not asserted: the harness compares the two widths' output.
    {
        const WideChar mixed[] = { 'a', 0x00E9, 0x4E2D, 0 };
        WideWireChar wire[16];
        const int units = wideCharToWire(wire, 16, mixed, WIDECHAR_WIRE_NUL_TERMINATED);
        printBytes("wire bytes", (const unsigned char *)wire, wideCharWireBytes(units));
        printBytes("csf bytes", csf.bytes, csf.size);
    }

    if (g_failures != 0) {
        printf("  %d case(s) failed\n", g_failures);
        return 1;
    }
    printf("  all cases passed\n");
    return 0;
}
"""

# (label, extra flags). 4 byte wchar_t is the native width; -fshort-wchar is the MSVC width.
CONFIGS = [
    ("64-bit, 4 byte wchar_t (native macOS/Linux)", []),
    ("64-bit, 2 byte wchar_t (the MSVC width)", ["-fshort-wchar"]),
]


def compile_and_run(clangxx, tu, out, extra_flags, verbose=False):
    target = [t for t in probe.TARGETS if t.name == "Core/GameEngine"][0]
    includes = [str(REPO_ROOT / "scripts" / "native-port-shims")]
    includes += probe.target_includes(target, probe.DEFAULT_DEPS_DIR)
    cmd = [
        clangxx, "-std=c++20", "-fms-extensions", "-w",
        "-include", "Utility/CppMacros.h",
        "-DWIN32_LEAN_AND_MEAN", "-D_REENTRANT", "-DRTS_ZEROHOUR=1",
    ] + list(extra_flags)
    for inc in includes:
        cmd += ["-I", str(inc)]
    cmd += [str(tu), "-o", str(out)]
    if verbose:
        print("  $", " ".join(cmd))
    proc = subprocess.run(cmd, cwd=REPO_ROOT, capture_output=True, text=True)
    if proc.returncode != 0:
        return None, proc.stdout + proc.stderr
    run = subprocess.run([str(out)], cwd=REPO_ROOT, capture_output=True, text=True)
    return run.returncode, run.stdout + run.stderr


def external_bytes(output):
    """The `wire bytes`/`csf bytes`/`save blob` lines, i.e. everything that leaves the process."""
    return [line.strip() for line in output.splitlines()
            if line.strip().startswith(("wire bytes", "csf bytes", "save blob"))]


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--clangxx", default="clang++-14", help="compiler to measure with")
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()

    failures = []
    outputs = {}

    with tempfile.TemporaryDirectory() as tmp:
        tmp = pathlib.Path(tmp)
        tu = tmp / "widechar_wire_test.cpp"
        tu.write_text(TEST_TU)

        for index, (label, flags) in enumerate(CONFIGS):
            print(f"[....] {label}")
            rc, output = compile_and_run(args.clangxx, tu, tmp / f"wcwire{index}", flags,
                                         verbose=args.verbose)
            if rc is None:
                failures.append(f"{label}: did not compile")
                print("\n".join(line for line in output.splitlines() if "error:" in line)[:4000])
                continue
            print(output.rstrip())
            if rc != 0:
                failures.append(f"{label}: a case failed")
                continue
            outputs[label] = output
            print(f"[ ok ] {label}")

        if len(outputs) == len(CONFIGS):
            reference_label = CONFIGS[0][0]
            reference = external_bytes(outputs[reference_label])
            for label, output in outputs.items():
                if external_bytes(output) != reference:
                    failures.append(f"{label}: the external bytes differ from the "
                                    f"{reference_label} build")
            if not failures:
                print(f"[ ok ] both widths produce the same {len(reference)} external byte "
                      f"sequences")

    if failures:
        print("\nFAILED:\n  " + "\n  ".join(failures))
        return 1
    print("\nWide text is 16 bits wide outside the process, at either wchar_t width.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
