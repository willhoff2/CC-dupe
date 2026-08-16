#!/usr/bin/env python3
"""Assert the native render-backend archive was compiled with a swapchain in it.

The backend's surface and presentation paths are behind `SPIKE_WITH_PLATFORM_WINDOW`. Built without
it, `VulkanBackend` has no surface, no swapchain and nothing to present -- and the engine's
`Present()` used to return success anyway. That is the fake-success class this port exists to stop,
and it was found on an M1 Pro by running `nm -um` on the archive by hand, not by any check.

So: a swapchain-less backend must be impossible to ship silently. Two things stop it now. At run
time `VulkanBackend::Present()` refuses when `swapchain_ == VK_NULL_HANDLE` and says so. At build
time this gate reads the archive the native build produced and fails if the three swapchain entry
points are not referenced from it -- which they cannot be unless the define was set.

    python3 scripts/ci/check-swapchain-compiled.py
    python3 scripts/ci/check-swapchain-compiled.py --archive build/native/libsupport_renderbackend.a
"""

import argparse
import pathlib
import shutil
import subprocess
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
DEFAULT_ARCHIVE = REPO_ROOT / "build" / "native" / "libsupport_renderbackend.a"
# vkCreateSwapchainKHR is the creation, vkAcquireNextImageKHR and vkQueuePresentKHR are the flip.
# All three are compiled away together, so any one missing means the same thing.
SWAPCHAIN_SYMBOLS = ("vkCreateSwapchainKHR", "vkAcquireNextImageKHR", "vkQueuePresentKHR")


def nm_tool():
    """`nm` that can read this platform's archives. Apple's cannot list Mach-O undefineds the
    way llvm-nm does, and the native build already depends on LLVM's binutils on macOS."""
    for name in ("llvm-nm-14", "llvm-nm", "nm"):
        found = shutil.which(name)
        if found:
            return found
    return None


def missing_symbols(archive):
    """The swapchain symbols the archive does not reference. Raises if `nm` cannot be run."""
    tool = nm_tool()
    if tool is None:
        raise RuntimeError("no nm on PATH, so the archive cannot be read")
    listing = subprocess.run([tool, "-u", str(archive)], capture_output=True, text=True).stdout
    return [symbol for symbol in SWAPCHAIN_SYMBOLS if symbol not in listing]


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--archive", type=pathlib.Path, default=DEFAULT_ARCHIVE,
                        help="the render-backend archive scripts/native-build.py produced")
    args = parser.parse_args()

    if not args.archive.is_file():
        sys.exit(f"{args.archive} is missing: run scripts/native-build.py --level 4 first")
    try:
        missing = missing_symbols(args.archive)
    except RuntimeError as why:
        sys.exit(str(why))
    if missing:
        sys.exit(f"FAIL: {args.archive.name} does not reference {', '.join(missing)}, so it was "
                 "built without SPIKE_WITH_PLATFORM_WINDOW: it has no swapchain and could only "
                 "report a present it did not perform")
    print(f"OK: {args.archive.name} references " + ", ".join(SWAPCHAIN_SYMBOLS)
          + " -- the backend the engine links has a swapchain")


if __name__ == "__main__":
    main()
