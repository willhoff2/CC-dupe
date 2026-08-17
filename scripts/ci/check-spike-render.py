#!/usr/bin/env python3
"""Run the renderer spike headless and check what it actually drew.

The spike already exits non-zero if nothing rasterised or if the validation layer complained,
but "the process exited 0" is not evidence that the right image came out, and the spike's own
validation check silently degrades to a no-op when VK_LAYER_KHRONOS_validation is not installed.
This wrapper closes both holes:

  * the validation layer must have been *active* (the spike's "layer not present" note is a
    failure here) and must have produced zero messages;
  * the read-back framebuffer is compared pixel by pixel against a committed reference image.

PNG decoding is done here rather than with Pillow so the job needs nothing but the standard
library. Only the 8-bit non-interlaced RGB/RGBA subset both the spike and optipng emit is
supported, which is checked explicitly rather than assumed.
"""
import argparse
import pathlib
import struct
import subprocess
import sys
import zlib

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import vulkan_manifests  # noqa: E402


def decode_png(path):
    """-> (width, height, bytes of RGBA8). Non-interlaced 8-bit RGB/RGBA/grey only."""
    data = pathlib.Path(path).read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"{path}: not a PNG")
    pos = 8
    idat = bytearray()
    width = height = bit_depth = colour_type = None
    palette = None
    while pos < len(data):
        (length,) = struct.unpack(">I", data[pos:pos + 4])
        ctype = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if ctype == b"IHDR":
            width, height, bit_depth, colour_type, comp, filt, interlace = \
                struct.unpack(">IIBBBBB", body)
            if bit_depth != 8:
                raise ValueError(f"{path}: bit depth {bit_depth} unsupported")
            if interlace:
                raise ValueError(f"{path}: interlaced PNGs unsupported")
            if colour_type not in (0, 2, 3, 6):
                raise ValueError(f"{path}: colour type {colour_type} unsupported")
        elif ctype == b"PLTE":
            palette = body
        elif ctype == b"IDAT":
            idat += body
        elif ctype == b"IEND":
            break

    raw = zlib.decompress(bytes(idat))
    channels = {0: 1, 2: 3, 3: 1, 6: 4}[colour_type]
    stride = width * channels
    out = bytearray(stride * height)
    prev = bytearray(stride)
    pos = 0
    for y in range(height):
        filter_type = raw[pos]
        pos += 1
        line = bytearray(raw[pos:pos + stride])
        pos += stride
        if filter_type == 1:      # Sub
            for i in range(channels, stride):
                line[i] = (line[i] + line[i - channels]) & 0xFF
        elif filter_type == 2:    # Up
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif filter_type == 3:    # Average
            for i in range(stride):
                left = line[i - channels] if i >= channels else 0
                line[i] = (line[i] + ((left + prev[i]) >> 1)) & 0xFF
        elif filter_type == 4:    # Paeth
            for i in range(stride):
                a = line[i - channels] if i >= channels else 0
                b = prev[i]
                c = prev[i - channels] if i >= channels else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 0xFF
        elif filter_type != 0:
            raise ValueError(f"{path}: unknown row filter {filter_type}")
        out[y * stride:(y + 1) * stride] = line
        prev = line

    rgba = bytearray(width * height * 4)
    for i in range(width * height):
        if colour_type == 6:
            rgba[i * 4:i * 4 + 4] = out[i * 4:i * 4 + 4]
        elif colour_type == 2:
            rgba[i * 4:i * 4 + 3] = out[i * 3:i * 3 + 3]
            rgba[i * 4 + 3] = 255
        elif colour_type == 3:
            idx = out[i]
            rgba[i * 4:i * 4 + 3] = palette[idx * 3:idx * 3 + 3]
            rgba[i * 4 + 3] = 255
        else:
            rgba[i * 4:i * 4 + 3] = bytes([out[i]]) * 3
            rgba[i * 4 + 3] = 255
    return width, height, bytes(rgba)


def compare(actual, reference, tolerance):
    aw, ah, ap = decode_png(actual)
    rw, rh, rp = decode_png(reference)
    if (aw, ah) != (rw, rh):
        return None, f"size {aw}x{ah} but the reference is {rw}x{rh}"
    worst = 0
    bad = 0
    worst_at = None
    for i in range(0, len(ap), 4):
        d = max(abs(ap[i + c] - rp[i + c]) for c in range(4))
        if d > worst:
            worst, worst_at = d, ((i // 4) % aw, (i // 4) // aw)
        if d > tolerance:
            bad += 1
    return {"pixels": aw * ah, "differing": bad, "worst": worst, "worst_at": worst_at}, None


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--binary", required=True)
    ap.add_argument("--reference", required=True)
    ap.add_argument("--out", required=True, help="where the spike writes its PNG")
    ap.add_argument("--tolerance", type=int, default=2,
                    help="permitted per-channel difference before a pixel counts as differing")
    ap.add_argument("--max-differing", type=float, default=0.0,
                    help="permitted fraction of differing pixels, 0..1")
    args = ap.parse_args()

    # DYLD_LIBRARY_PATH cannot be handed to the child on macOS -- dyld strips it when a
    # SIP-protected binary is exec'd, this script included -- so the layer and the driver are
    # reached through manifests rewritten with absolute library paths instead.
    environment = vulkan_manifests.child_environment()

    proc = subprocess.run([args.binary, "--out", args.out], capture_output=True, text=True,
                          env=environment)
    sys.stdout.write(proc.stdout)
    sys.stderr.write(proc.stderr)

    failures = []
    if proc.returncode != 0:
        failures.append(f"the spike exited {proc.returncode}")
    # The spike degrades gracefully when the layer is missing; in CI that must be an error,
    # otherwise "validation messages: 0" means "nothing was validated". Asserted on the positive
    # statement, so a spike that stops reporting its layer status fails here too rather than
    # passing by silence.
    # On stderr, because zh-staging-workload's stdout is JSON and the backend says it the same way
    # for every spike.
    if "validation layer: loaded" not in proc.stdout + proc.stderr:
        failures.append("the validation layer was not loaded, so the run proves nothing about "
                        "validation cleanliness")
    if "validation messages: 0" not in proc.stdout:
        failures.append("the spike did not report zero validation messages")

    if not pathlib.Path(args.out).is_file():
        failures.append(f"no image at {args.out}")
    else:
        stats, err = compare(args.out, args.reference, args.tolerance)
        if err:
            failures.append(err)
        else:
            fraction = stats["differing"] / stats["pixels"]
            print(f"readback vs {args.reference}: {stats['differing']}/{stats['pixels']} pixels "
                  f"({fraction * 100:.4f}%) differ by more than {args.tolerance}/255; "
                  f"worst channel delta {stats['worst']} at {stats['worst_at']}")
            if fraction > args.max_differing:
                failures.append(
                    f"the readback does not match the reference image: {fraction * 100:.4f}% of "
                    f"pixels differ by more than {args.tolerance}/255, limit is "
                    f"{args.max_differing * 100:.4f}%")

    if failures:
        print()
        print("FAIL: renderer spike check", file=sys.stderr)
        for line in failures:
            print(f"  - {line}", file=sys.stderr)
        return 1
    print("OK: spike rendered the reference image with the validation layer active and silent")
    return 0


if __name__ == "__main__":
    sys.exit(main())
