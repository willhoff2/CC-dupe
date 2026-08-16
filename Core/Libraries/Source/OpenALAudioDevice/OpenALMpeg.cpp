/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
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

/**
 * @file
 *
 * @brief The MPEG audio decoder behind the Miles stream path, and the frame-header parser the
 *        stream index is built from.
 *
 * Retail Zero Hour music is 56 MP3 files and Miles decoded MP3 inside mss32.dll, so off Windows
 * `AIL_open_stream` needs a decoder. minimp3 (CC0, pinned in cmake/minimp3.cmake) is compiled here,
 * in exactly one translation unit, and nothing outside this file sees it: the rest of the backend
 * uses the `MpegDecoder` handle and the frame facts declared in OpenALAudioInternal.h.
 *
 * The header parser is deliberately *not* minimp3's — minimp3's is internal — and is what lets the
 * stream index every frame at open time. That index is why duration and seeking are exact for
 * variable-bitrate files as well as constant ones: nothing here divides a byte count by a bitrate.
 * See docs/porting/audio-mpeg-decode.md.
 */

#include "OpenALAudioInternal.h"

#include <cstring>
#include <new>

// minimp3 is a vendored public-domain header, so its warnings are not this port's portability
// defects; the backend is built -Wall -Wextra and only this include is exempted.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wsign-compare"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wsign-compare"
#endif

#define MINIMP3_IMPLEMENTATION
#include <minimp3.h>

#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace OpenALAudio
{
namespace
{

/// Sample rates, indexed by the frame header's version and rate index. MPEG-2 is half MPEG-1 and
/// MPEG-2.5 a quarter of it; retail uses two of these (44100 and, for Silence60.mp3, 22050).
const unsigned int MPEG_RATES[4][3] = {
	{11025, 12000, 8000},	///< version bits 00: MPEG-2.5
	{0, 0, 0},				///< 01: reserved
	{22050, 24000, 16000},	///< 10: MPEG-2
	{44100, 48000, 32000},	///< 11: MPEG-1
};

/// Bitrates in kbps by layer and bitrate index. Index 0 is "free format" and 15 is invalid; both
/// are rejected by the parser, so the zeroes at either end are never read as a rate.
const unsigned int MPEG1_BITRATES[3][16] = {
	{0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448, 0},	///< layer I
	{0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 0},		///< layer II
	{0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0},		///< layer III
};
const unsigned int MPEG2_BITRATES[3][16] = {
	{0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256, 0},		///< layer I
	{0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0},			///< layer II
	{0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0},			///< layer III
};

} // namespace

struct MpegDecoder
{
	mp3dec_t state;
};

bool parseMpegFrameHeader(const unsigned char* at, size_t available, MpegFrameHeader& out)
{
	if (at == nullptr || available < MPEG_FRAME_HEADER_BYTES) {
		return false;
	}
	if (at[0] != 0xFF || (at[1] & 0xE0) != 0xE0) {
		return false;
	}

	const unsigned int versionBits = (unsigned int)(at[1] >> 3) & 0x03u;
	const unsigned int layerBits = (unsigned int)(at[1] >> 1) & 0x03u;
	const unsigned int bitrateIndex = (unsigned int)(at[2] >> 4) & 0x0Fu;
	const unsigned int rateIndex = (unsigned int)(at[2] >> 2) & 0x03u;
	const unsigned int padding = (unsigned int)(at[2] >> 1) & 0x01u;
	const unsigned int mode = (unsigned int)(at[3] >> 6) & 0x03u;

	// Reserved version, reserved layer, reserved rate index, free-format and invalid bitrates: a
	// sync pattern with any of these is not a frame header, it is a false positive inside tag or
	// payload bytes, and the scan must keep looking.
	if (versionBits == 1 || layerBits == 0 || rateIndex == 3
		|| bitrateIndex == 0 || bitrateIndex == 15) {
		return false;
	}

	const unsigned int layer = 4u - layerBits;			///< bits 11/10/01 are layers I/II/III
	const unsigned int rate = MPEG_RATES[versionBits][rateIndex];
	const bool mpeg1 = (versionBits == 3);
	const unsigned int bitrate = mpeg1
		? MPEG1_BITRATES[layer - 1][bitrateIndex]
		: MPEG2_BITRATES[layer - 1][bitrateIndex];
	if (rate == 0 || bitrate == 0) {
		return false;
	}

	// Samples per frame: 384 for layer I, 1152 for layer II, and for layer III 1152 on MPEG-1 but
	// 576 on MPEG-2/2.5, which is why Silence60.mp3 (MPEG-2) cannot share MPEG-1's maths.
	unsigned int samples = 1152;
	if (layer == 1) {
		samples = 384;
	} else if (layer == 3 && !mpeg1) {
		samples = 576;
	}

	// Frame length in bytes. Layer I is counted in 4-byte slots, the others in bytes.
	unsigned int bytes = 0;
	if (layer == 1) {
		bytes = (12000u * bitrate / rate + padding) * 4u;
	} else {
		bytes = (samples / 8u) * 1000u * bitrate / rate + padding;
	}
	if (bytes <= MPEG_FRAME_HEADER_BYTES) {
		return false;
	}

	out.bytes = bytes;
	out.samples = samples;
	out.rate = rate;
	out.channels = (mode == 3) ? 1u : 2u;			///< mode 11 is single channel
	out.bitrateKbps = bitrate;
	out.layer = layer;
	out.version = mpeg1 ? 1u : (versionBits == 2 ? 2u : 25u);
	return true;
}

unsigned int id3v2TagLength(const unsigned char* front, size_t size)
{
	// ID3v2: "ID3", two version bytes, flags, then a 28-bit synchsafe size that excludes the
	// 10-byte header. 54 of the 56 retail tracks carry one, all 1024 bytes, so the first frame
	// header is not at offset 0 and a decoder starting there finds tag bytes.
	if (front == nullptr || size < 10 || std::memcmp(front, "ID3", 3) != 0) {
		return 0;
	}
	unsigned int length = 0;
	for (int i = 6; i < 10; ++i) {
		if ((front[i] & 0x80u) != 0) {
			return 0;					///< not synchsafe: not a tag this can trust
		}
		length = (length << 7) | (unsigned int)(front[i] & 0x7Fu);
	}
	const bool footer = (front[5] & 0x10u) != 0;
	return 10u + length + (footer ? 10u : 0u);
}

MpegDecoder* createMpegDecoder()
{
	MpegDecoder* decoder = new (std::nothrow) MpegDecoder();
	if (decoder == nullptr) {
		return nullptr;
	}
	mp3dec_init(&decoder->state);
	return decoder;
}

void destroyMpegDecoder(MpegDecoder* decoder)
{
	delete decoder;
}

void resetMpegDecoder(MpegDecoder* decoder)
{
	if (decoder != nullptr) {
		// Layer III frames may reference up to 511 bytes of the previous frame's bit reservoir, and
		// the synthesis filterbank carries overlap state, so a decoder that jumps must forget both.
		// The first frame after a seek or a loop restart is therefore approximate, as it is in every
		// MP3 decoder; the frames after it are exact.
		mp3dec_init(&decoder->state);
	}
}

unsigned int decodeMpegFrame(MpegDecoder& decoder, const unsigned char* data, unsigned int size,
	int16_t* out, MpegFrameHeader& header)
{
	mp3dec_frame_info_t info;
	std::memset(&info, 0, sizeof(info));
	const int samples = mp3dec_decode_frame(&decoder.state, data, (int)size, out, &info);
	if (samples <= 0 || info.channels <= 0 || info.hz <= 0) {
		return 0;
	}
	header.bytes = (info.frame_bytes > 0) ? (unsigned int)info.frame_bytes : 0u;
	header.samples = (unsigned int)samples;
	header.rate = (unsigned int)info.hz;
	header.channels = (unsigned int)info.channels;
	header.bitrateKbps = (unsigned int)info.bitrate_kbps;
	header.layer = (unsigned int)info.layer;
	header.version = 0;					///< minimp3 does not report it; the parser above does
	return (unsigned int)samples;
}

} // namespace OpenALAudio
