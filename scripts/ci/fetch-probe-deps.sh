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
pin() { # <cmake file> <field>
    grep -oP "^\s*${2}\s+\K\S+" "$repo_root/cmake/$1"
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

echo
echo "provisioned into $deps_dir:"
ls -1 "$deps_dir"
