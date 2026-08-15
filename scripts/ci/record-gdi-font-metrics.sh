#!/usr/bin/env bash
# Record the reference glyph metrics scripts/ci/check-font-metrics.py compares the port's
# rasteriser against, by running the Win32 dumper against a GDI implementation.
#
#   scripts/ci/record-gdi-font-metrics.sh [font-file] [face-name] [output]
#
# The dumper is Core/Libraries/Source/WWVegas/WWLib/platform/tests/gdi_font_metrics_dump.cpp, the
# same source the native side of the comparison is built from. It is compiled with VC6 and run
# under Wine, in the container scripts/docker-build.sh builds the Windows executables with -- so
# the numbers are GDI's *as Wine implements it*, not retail Windows GDI's. That is stated in
# docs/porting/gdi-font-seam.md and in the recorded file itself; re-record on Windows by building
# the same source there and running it with the same arguments.
#
# The font file is an argument on both sides of the comparison and both sides get the same one:
# DejaVu Sans is what the build container has installed, and the check requires the same file on
# the machine it runs on.
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
font_file=${1:-/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf}
face_name=${2:-DejaVu Sans}
output=${3:-$repo_root/scripts/ci/font-metrics-reference.json}
image=${IMAGE:-zerohour-build}

if ! docker image inspect "$image" >/dev/null 2>&1; then
    echo "$image is not built. Run scripts/docker-build.sh first: this needs its VC6 and Wine." >&2
    exit 1
fi

font_name=$(basename "$font_file")
recorded=$(mktemp)
trap 'rm -f "$recorded"' EXIT

docker run --rm -v "$repo_root":/cnc:ro -v "$font_file":/font/"$font_name":ro -w /tmp \
    "$image" bash -lc '
set -euo pipefail
mkdir -p "$WINEPREFIX/drive_c/windows/Fonts"
cp "/font/'"$font_name"'" "$WINEPREFIX/drive_c/windows/Fonts/"
cp /cnc/Core/Libraries/Source/WWVegas/WWLib/platform/tests/gdi_font_metrics_dump.cpp dump.cpp
wine "$CXX" /nologo /MT /W3 dump.cpp /link gdi32.lib user32.lib /out:dump.exe >/dev/null 2>&1
wine ./dump.exe "C:\\windows\\Fonts\\'"$font_name"'" "'"$face_name"'" 2>/dev/null
' > "$recorded"

python3 - "$recorded" "$output" "$font_name" <<'PYTHON'
import datetime
import json
import sys

recorded, output, font_name = sys.argv[1], sys.argv[2], sys.argv[3]
with open(recorded) as handle:
    data = json.load(handle)

# The provenance travels with the numbers: a reader of this file has to be able to tell recorded
# Wine-GDI data from data taken on retail Windows, because the two are not the same oracle.
data = {
    "provenance": {
        "implementation": "Wine's gdi32 under the VC6 build container (scripts/docker-build.sh)",
        "is_retail_windows": False,
        "recorded_by": "scripts/ci/record-gdi-font-metrics.sh",
        "recorded_on": datetime.date.today().isoformat(),
        "note": "Recorded, not live: the comparison in scripts/ci/check-font-metrics.py runs "
                "against these numbers. Re-record by building "
                "platform/tests/gdi_font_metrics_dump.cpp on Windows and running it with the "
                "same font file and face name.",
    },
    "font_name": font_name,
    **data,
}

with open(output, "w") as handle:
    json.dump(data, handle, indent=2)
    handle.write("\n")
print(f"wrote {output}: {len(data['cases'])} font cases")
PYTHON
