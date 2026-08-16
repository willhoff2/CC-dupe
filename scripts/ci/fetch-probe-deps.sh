#!/usr/bin/env bash
# Provision the third-party headers the native probe needs into build/docker/_deps.
#
# The probe (scripts/native-port-probe.py) picks up dx8 / gamespy / miles / lzhl headers from
# build/docker/_deps when that directory exists, and its clean-TU count changes depending on
# whether they are there (Core/Libraries/Source/Compression needs lzhl.h). That directory is
# untracked and is normally a by-product of a Docker build, so a CI run that relies on it is
# not reproducible. This script provisions it explicitly instead, from the same pinned
# repositories and commits the CMake FetchContent calls use -- the pins are read out of
# cmake/*.cmake, so they cannot drift away from the real build.
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
deps_dir="${1:-$repo_root/build/docker/_deps}"

# Reads GIT_REPOSITORY / GIT_TAG out of a cmake/<name>.cmake file so the pins stay in one place.
# sed rather than grep -oP: -P and \K are GNU extensions, and BSD grep (macOS) rejects them.
pin() { # <cmake file> <field>
    sed -nE "s/^[[:space:]]*${2}[[:space:]]+([^[:space:]]+).*/\1/p" "$repo_root/cmake/$1"
}

clone_at() { # <url> <commit> <dest>
    local url=$1 commit=$2 dest=$3
    if [ -d "$dest/.git" ]; then
        echo "== $dest already present"
        return
    fi
    echo "== $url @ $commit -> $dest"
    mkdir -p "$dest"
    git -C "$dest" init -q
    git -C "$dest" remote add origin "$url"
    git -C "$dest" fetch -q --depth 1 origin "$commit"
    git -C "$dest" checkout -q FETCH_HEAD
}

mkdir -p "$deps_dir"
clone_at "$(pin dx8.cmake GIT_REPOSITORY)"     "$(pin dx8.cmake GIT_TAG)"     "$deps_dir/dx8-src"
clone_at "$(pin gamespy.cmake GIT_REPOSITORY)" "$(pin gamespy.cmake GIT_TAG)" "$deps_dir/gamespy-src"
clone_at "$(pin miles.cmake GIT_REPOSITORY)"   "$(pin miles.cmake GIT_TAG)"   "$deps_dir/miles-src"
# lzhl is laid out as _deps/lzhl-src/CompLibHeader by cmake/lzhl.cmake, and the engine includes
# it as <CompLibHeader/....h>.
clone_at "$(pin lzhl.cmake GIT_REPOSITORY)"    "$(pin lzhl.cmake GIT_TAG)"    "$deps_dir/lzhl-src/CompLibHeader"
# stb: <stb_image_write.h> for stb_image_write_impl.cpp and <stb_truetype.h> for the GDI
# font seam's rasteriser (docs/porting/gdi-font-seam.md), so WWLib does not compile
# natively without it.
# stb is laid out as _deps/stb-src by cmake/stb.cmake; stb_image_write_impl.cpp includes
# <stb_image_write.h> from the root of it.
clone_at "$(pin stb.cmake GIT_REPOSITORY)"     "$(pin stb.cmake GIT_TAG)"     "$deps_dir/stb-src"

# OpenAL: <AL/al.h> and <AL/alc.h> for Core/Libraries/Source/OpenALAudioDevice, the engine's own
# Miles replacement (`milesstub` off 32-bit Windows). cmake/openal.cmake prefers a system OpenAL and
# only falls back to fetching openal-soft, so these headers are what makes the native build's audio
# backend compile on a box without libopenal-dev. A blobless partial clone of the public headers is
# enough: nothing here builds openal-soft itself, and linking uses the system library when present.
openal_dir="$deps_dir/openal-src"
if [ -d "$openal_dir/.git" ]; then
    echo "== $openal_dir already present"
else
    openal_tag=$(pin openal.cmake GIT_TAG)
    echo "== $(pin openal.cmake GIT_REPOSITORY) @ $openal_tag -> $openal_dir"
    mkdir -p "$openal_dir"
    git -C "$openal_dir" init -q
    git -C "$openal_dir" remote add origin "$(pin openal.cmake GIT_REPOSITORY)"
    git -C "$openal_dir" config extensions.partialClone origin
    git -C "$openal_dir" sparse-checkout set --no-cone '/include/AL/*.h'
    git -C "$openal_dir" fetch -q --depth 1 --filter=blob:none origin "$openal_tag"
    git -C "$openal_dir" checkout -q FETCH_HEAD
fi

# FFmpeg headers for the RTS_BUILD_OPTION_FFMPEG video path. The real build gets the libraries
# from vcpkg, so the pin is the version in vcpkg-lock.json rather than a cmake/*.cmake GIT_TAG;
# upstream tags that release as n<version>. The probe and the native build only ever compile
# against these headers -- nothing links libavcodec here -- so a blobless partial clone of the
# public header directories is enough and keeps this cheap.
ffmpeg_version=$(sed -nE '/"name": *"ffmpeg"/,/}/ s/.*"version-string": *"([^"]+)".*/\1/p' \
    "$repo_root/vcpkg-lock.json")
ffmpeg_dir="$deps_dir/ffmpeg-src"
if [ -z "$ffmpeg_version" ]; then
    echo "could not read the ffmpeg version out of vcpkg-lock.json" >&2
    exit 1
fi
if [ -d "$ffmpeg_dir/.git" ]; then
    echo "== $ffmpeg_dir already present"
else
    echo "== FFmpeg n$ffmpeg_version -> $ffmpeg_dir"
    mkdir -p "$ffmpeg_dir"
    git -C "$ffmpeg_dir" init -q
    git -C "$ffmpeg_dir" remote add origin https://github.com/FFmpeg/FFmpeg.git
    git -C "$ffmpeg_dir" config extensions.partialClone origin
    git -C "$ffmpeg_dir" sparse-checkout set --no-cone \
        '/libavcodec/*.h' '/libavformat/*.h' '/libavutil/*.h' '/libswscale/*.h' \
        '/libswresample/*.h'
    git -C "$ffmpeg_dir" fetch -q --depth 1 --filter=blob:none origin "n$ffmpeg_version"
    git -C "$ffmpeg_dir" checkout -q FETCH_HEAD
fi

# `libavutil/avconfig.h` and `libavutil/ffversion.h` are generated by FFmpeg's configure, so they
# are absent from a source checkout, and <libavutil/common.h> includes the first of them. Writing
# them here rather than running configure keeps the provisioning to a checkout: the two macros
# avconfig.h defines are properties of the host, and the probe only ever targets little-endian
# x86_64/arm64, which have both.
if [ ! -f "$ffmpeg_dir/libavutil/avconfig.h" ]; then
    cat > "$ffmpeg_dir/libavutil/avconfig.h" <<'EOF'
/* Generated by scripts/ci/fetch-probe-deps.sh, not by FFmpeg's configure. */
#ifndef AVUTIL_AVCONFIG_H
#define AVUTIL_AVCONFIG_H
#define AV_HAVE_BIGENDIAN 0
#define AV_HAVE_FAST_UNALIGNED 1
#endif /* AVUTIL_AVCONFIG_H */
EOF
fi
if [ ! -f "$ffmpeg_dir/libavutil/ffversion.h" ]; then
    cat > "$ffmpeg_dir/libavutil/ffversion.h" <<EOF
/* Generated by scripts/ci/fetch-probe-deps.sh, not by FFmpeg's configure. */
#ifndef AVUTIL_FFVERSION_H
#define AVUTIL_FFVERSION_H
#define FFMPEG_VERSION "n$ffmpeg_version"
#endif /* AVUTIL_FFVERSION_H */
EOF
fi

# The FFmpeg *libraries*, so the native build can link the video path instead of reporting its 29
# symbols as unresolved forever. Built from the same tag the headers above are checked out at,
# because a link against a different major (Ubuntu 22.04 ships libavcodec 58; the pin is 7.1.1,
# libavcodec 61) resolves every symbol and then disagrees with the headers the engine compiled
# against on every struct layout -- an executable produced that way would be a measurement lie.
#
# The configuration is deliberately narrow and named here rather than "whatever configure
# defaults to": the shared libraries only have to export the API `FFmpegFile.cpp` calls and decode
# what the game ships, which is Bink (`Data/Movies/*.bik`, plus `.binka` audio) -- the format
# upstream FFmpeg decodes natively and the reason the FFmpeg route replaced the Bink SDK here at
# all. Two consequences worth stating rather than discovering: the real build gets vcpkg's
# full-featured FFmpeg, so this subset is a property of the harness and not of the port; and
# `--disable-x86asm` (used when no assembler is installed) costs decode speed and changes no API.
# h264/aac were in this list briefly and are not, because `--disable-everything --enable-decoder=h264`
# leaves libavcodec itself with an undefined `ff_aom_uninit_film_grain_params` -- an upstream
# configure dependency gap that would have surfaced as an unresolved symbol attributed to the engine.
#
# Set SKIP_FFMPEG_LIBS=1 to skip it (the link then reports the 29 symbols as unresolved again).
ffmpeg_lib_dir="$deps_dir/ffmpeg-lib"
ffmpeg_build_dir="$deps_dir/ffmpeg-build"
if [ "${SKIP_FFMPEG_LIBS:-0}" = "1" ]; then
    echo "== skipping the FFmpeg library build (SKIP_FFMPEG_LIBS=1)"
elif [ -f "$ffmpeg_lib_dir/.built-n$ffmpeg_version" ]; then
    echo "== $ffmpeg_lib_dir already built (n$ffmpeg_version)"
else
    echo "== building FFmpeg n$ffmpeg_version -> $ffmpeg_lib_dir"
    clone_at https://github.com/FFmpeg/FFmpeg.git "n$ffmpeg_version" "$ffmpeg_build_dir"
    asm_flag=
    if ! command -v nasm >/dev/null 2>&1 && ! command -v yasm >/dev/null 2>&1; then
        echo "   no nasm/yasm: configuring --disable-x86asm (slower decode, same API)"
        asm_flag=--disable-x86asm
    fi
    # FFmpeg's configure defaults to `gcc`, and the CI container installs clang only -- the whole
    # point of measuring with clang 14 -- so a missing gcc fails the C compiler test five seconds
    # in. Name a compiler that exists instead of assuming one, and if configure still fails, print
    # the tail of its own log: the failure was invisible once and cost a CI round trip.
    ffmpeg_cc=
    for candidate in "${CC:-}" cc gcc clang clang-14; do
        if [ -n "$candidate" ] && command -v "$candidate" >/dev/null 2>&1; then
            ffmpeg_cc="$candidate"
            break
        fi
    done
    if [ -z "$ffmpeg_cc" ]; then
        echo "   no C compiler found for FFmpeg's configure: skipping the library build" >&2
        echo "   (the link will report the FFmpeg symbols as unresolved)" >&2
        exit 1
    fi
    echo "   configuring with --cc=$ffmpeg_cc"
    rm -rf "$ffmpeg_lib_dir"
    (
        cd "$ffmpeg_build_dir"
        # shellcheck disable=SC2086  # asm_flag is one optional word, deliberately unquoted.
        if ! ./configure --prefix="$ffmpeg_lib_dir" --cc="$ffmpeg_cc" $asm_flag \
            --disable-programs --disable-doc --disable-network --disable-autodetect \
            --disable-everything --enable-shared --disable-static \
            --enable-decoder=bink,binkaudio_dct,binkaudio_rdft,pcm_s16le \
            --enable-demuxer=bink,binka,avi,wav \
            --enable-protocol=file --enable-swscale --enable-swresample >/dev/null; then
            echo "   FFmpeg configure failed; the tail of ffbuild/config.log:" >&2
            tail -n 40 ffbuild/config.log >&2 || true
            exit 1
        fi
        make -j"$(getconf _NPROCESSORS_ONLN)" >/dev/null
        make install >/dev/null
    )
    touch "$ffmpeg_lib_dir/.built-n$ffmpeg_version"
fi

echo
echo "provisioned into $deps_dir:"
ls -1 "$deps_dir"
