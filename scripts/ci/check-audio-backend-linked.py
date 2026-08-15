#!/usr/bin/env python3
"""Gate that the native build links the OpenAL audio backend, and that no `AIL_*` is unresolved.

`Core/Libraries/Source/OpenALAudioDevice` is the engine's audio device off 32-bit Windows:
`cmake/openal.cmake` supplies `milesstub` from it, so every audio consumer links it and there is no
parallel path. `scripts/native-build.py` did not build it, which reported 89 `AIL_*` symbols as
unresolved at levels 1-4 (60 at levels 1-3) purely because the harness excluded the layer that
defines them -- the same artefact level 4 removed for the renderer.

Two things are asserted, because the total in the baseline is only a ratchet and either failure
would look like progress:

* the backend archive is in the link, so the count cannot fall because a whole layer left the build;
* no `AIL_*` symbol is unresolved, so a Miles entry point the engine starts calling and the backend
  does not define fails here, by name, instead of hiding inside the total.

The second is the interesting one: `check-openal-symbols.py` gates *declared vs defined* inside the
backend, and `audio-surface-scan.py --check` gates *demanded vs declared* by source scan. Neither
can see a symbol the engine references but `mss.h` never declares. Only the link can.
"""
import argparse
import json
import pathlib
import sys

MILES_CATEGORY = "Miles Sound System"
BACKEND_ARCHIVE = "libsupport_openalaudiodevice"


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--results", required=True, help="JSON written by native-build.py --json")
    args = ap.parse_args()

    results = json.loads(pathlib.Path(args.results).read_text())
    linked = results.get("third_party_linked", [])
    unresolved = results.get("undefined_symbols", {}).get(MILES_CATEGORY, [])

    failures = []
    if BACKEND_ARCHIVE not in linked:
        failures.append(
            f"the OpenAL audio backend is not in the link ({BACKEND_ARCHIVE} missing from "
            f"third_party_linked: {', '.join(linked) or 'nothing'}). Without it every AIL_* symbol "
            "is unresolved for want of a build, not for want of a port: check that "
            "scripts/ci/fetch-probe-deps.sh provisioned openal-src, and that libopenal is "
            "installed.")
    print(f"linked: {', '.join(linked) or 'nothing'}")
    print(f"unresolved {MILES_CATEGORY} symbols: {len(unresolved)}")
    if unresolved:
        for name in unresolved:
            print(f"  {name}")
        failures.append(
            f"{len(unresolved)} AIL_* symbols are unresolved although the backend is linked. Each "
            "is a Miles entry point the engine calls and Core/Libraries/Source/OpenALAudioDevice "
            "does not define; implement it over OpenAL rather than stubbing it. See "
            "docs/porting/audio-device-seam.md.")

    if failures:
        for failure in failures:
            print(f"FAIL: {failure}", file=sys.stderr)
        return 1
    print("OK: the OpenAL backend is linked and defines every AIL_* symbol the engine references")
    return 0


if __name__ == "__main__":
    sys.exit(main())
