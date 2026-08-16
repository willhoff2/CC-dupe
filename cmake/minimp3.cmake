# minimp3: the MPEG audio decoder the OpenAL backend uses for retail music.
#
# WHY A DEDICATED DECODER RATHER THAN FFMPEG. Retail Zero Hour music is 56 MP3 files
# (docs/porting/audio-retail-validation.md), and Miles decoded MP3 inside mss32.dll, so off Windows
# the `AIL_open_stream` path needs a decoder of its own. FFmpeg is already in the tree, but it is
# reached through `RTS_BUILD_OPTION_FFMPEG`, which is OFF by default and attaches libav* to
# `corei_gameenginedevice` for the *video* path; routing music through it would make the music
# conditional on a video option and give the audio backend -- which `check-audio-backend-linked.py`
# gates as linking standalone -- a four-library runtime dependency for one codec. Measured, the
# FFmpeg this repo actually provisions cannot decode MP3 at all: `fetch-probe-deps.sh` configures it
# `--disable-everything` for Bink, and `avcodec_find_decoder(AV_CODEC_ID_MP3)` returns null against
# it. See docs/porting/audio-mpeg-decode.md for the full comparison.
#
# minimp3 is a single public-domain (CC0) header, decodes MPEG-1/2/2.5 layer I/II/III to interleaved
# 16-bit PCM a frame at a time -- the shape `readChunk` already has -- and adds no runtime library.
# It is pinned to a commit, like every other dependency here, and provisioned for the native probe
# by scripts/ci/fetch-probe-deps.sh at the same commit.
find_package(minimp3 QUIET)

if(NOT minimp3_FOUND AND NOT DEFINED minimp3_INCLUDE_DIR)
	include(FetchContent)
	FetchContent_Declare(
		minimp3
		GIT_REPOSITORY https://github.com/lieff/minimp3.git
		# master as of 2026-03-11; the two commits at that tip harden VBR-tag parsing and fix an
		# out-of-bounds read on seek, so this is deliberately newer than the 2021 tag most
		# packagers ship.
		GIT_TAG        7b590fdcfa5a79c033e76eacc05d0c3e4c79f536
	)

	FetchContent_MakeAvailable(minimp3)

	set(minimp3_INCLUDE_DIR ${minimp3_SOURCE_DIR})
endif()

add_library(minimp3 INTERFACE)
target_include_directories(minimp3 INTERFACE ${minimp3_INCLUDE_DIR})
