#!/usr/bin/env python3
"""Run the spike's HiDPI tests and check the run was worth believing.

`zh-hidpi-tests` asserts the points/pixels rule on read-back pixels at an injected backing scale
of 2.00 and 1.25 (docs/porting/hidpi-scale.md). It exits non-zero on its own when an assertion
fails, but "exited 0" is not enough for a gate:

  * a run whose validation layer never loaded proves nothing, on macOS most of all, where the
    loader silently drops the layer (docs/porting/apple-silicon-verification.md 8.1) -- so the
    binary has to have said `validation layer: loaded`;
  * a binary that quietly stopped exercising scale 2 would still exit 0, so the scales that must
    appear in the output are named here rather than left to the binary;
  * every check the binary prints is counted, and zero checks is a failure.

The child's environment comes from scripts/ci/vulkan_manifests.py, which is what makes the layer
survive the exec on macOS.
"""
import argparse
import pathlib
import re
import subprocess
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import vulkan_manifests  # noqa: E402

# The scales the binary must be seen to have run. 2.00 is a Retina panel's factor, which is the
# defect's own condition; 1.00 is the identity case Linux CI used to assert alone; 1.25 is a
# fractional scale, where a point is not a whole number of pixels.
REQUIRED_SCALES = ("2.00", "1.00", "1.25")

SUMMARY = re.compile(r"^(\d+) checks, (\d+) failed$", re.MULTILINE)
SCALE_LINE = re.compile(r"^  PASS colour target in pixels (\d+)x(\d+), expected (\d+)x(\d+)$",
                        re.MULTILINE)


def scales_seen(text, points_width, points_height):
    """-> the set of scales, formatted like REQUIRED_SCALES, the passing checks show."""
    seen = set()
    for width, height, _, _ in SCALE_LINE.findall(text):
        by_width = int(width) / points_width
        by_height = int(height) / points_height
        # A fractional scale rounds up in each axis independently, so agree to the pixel the
        # rounding can move rather than demanding the two ratios match exactly.
        seen.add(f"{max(by_width, by_height):.2f}")
    return seen


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--binary", required=True)
    parser.add_argument("--points", default="100x80",
                        help="the point-space back buffer the binary uses")
    parser.add_argument("--min-checks", type=int, default=20,
                        help="the fewest assertions a real run makes")
    args = parser.parse_args()

    points_width, points_height = (int(part) for part in args.points.split("x"))

    proc = subprocess.run([args.binary], capture_output=True, text=True,
                          env=vulkan_manifests.child_environment())
    sys.stdout.write(proc.stdout)
    sys.stderr.write(proc.stderr)

    failures = []
    if proc.returncode != 0:
        failures.append(f"the HiDPI tests exited {proc.returncode}")
    # The layer status is on stderr: zh-staging-workload's stdout is JSON, so the backend reports
    # it where every spike can say it without breaking a machine-readable stdout.
    if "validation layer: loaded" not in proc.stdout + proc.stderr:
        failures.append("the validation layer was not loaded, so this run says nothing about "
                        "validation cleanliness -- install VK_LAYER_KHRONOS_validation, or on "
                        "macOS see scripts/ci/vulkan_manifests.py")

    match = SUMMARY.search(proc.stdout)
    if match is None:
        failures.append("the binary printed no check summary")
    else:
        checks, failed = int(match.group(1)), int(match.group(2))
        print(f"{checks} checks, {failed} failed")
        if failed != 0:
            failures.append(f"{failed} of {checks} HiDPI assertions failed")
        if checks < args.min_checks:
            failures.append(f"only {checks} assertions ran, expected at least "
                            f"{args.min_checks}: the tests were cut down, not passed")

    seen = scales_seen(proc.stdout, points_width, points_height)
    missing = [scale for scale in REQUIRED_SCALES if scale not in seen]
    if missing:
        failures.append(f"no passing target-size check at backing scale(s) "
                        f"{', '.join(missing)} (saw {', '.join(sorted(seen)) or 'none'}); a run "
                        f"that never leaves scale 1.00 is the blind spot this gate exists for")

    if failures:
        print()
        print("FAIL: HiDPI scale check", file=sys.stderr)
        for line in failures:
            print(f"  - {line}", file=sys.stderr)
        return 1
    print(f"OK: the colour target, viewport, scissor and read-back followed the backing scale at "
          f"{', '.join(REQUIRED_SCALES)} with the validation layer active and silent")
    return 0


if __name__ == "__main__":
    sys.exit(main())
