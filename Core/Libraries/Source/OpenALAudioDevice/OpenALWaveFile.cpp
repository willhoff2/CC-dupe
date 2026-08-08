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
 * @brief WAV parsing and IMA ADPCM decoding for the OpenAL Miles replacement.
 *
 * Miles parsed and decoded audio internally; AIL_WAV_info, AIL_decompress_ADPCM and the
 * AIL_set_*_sample_file calls all hand the engine's raw file image straight to the sound system,
 * so this layer owns the container and codec handling. Only the formats Zero Hour's sound effects
 * actually use are supported: PCM (8/16-bit, mono/stereo) and IMA ADPCM.
 */

#include "OpenALAudioInternal.h"

#include <cstdlib>
#include <cstring>

namespace OpenALAudio
{

namespace
{

struct Reader
{
	const unsigned char* base;
	unsigned int size;
	unsigned int at = 0;

	bool has(unsigned int n) const { return at + n <= size; }
	uint16_t u16()
	{
		uint16_t v = (uint16_t)(base[at] | (base[at + 1] << 8));
		at += 2;
		return v;
	}
	uint32_t u32()
	{
		uint32_t v = (uint32_t)base[at] | ((uint32_t)base[at + 1] << 8) | ((uint32_t)base[at + 2] << 16)
			| ((uint32_t)base[at + 3] << 24);
		at += 4;
		return v;
	}
	bool tag(const char* four) const { return has(4) && std::memcmp(base + at, four, 4) == 0; }
};

/// Standard IMA ADPCM step and index tables.
const int STEP_TABLE[89] = {
	7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73,
	80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494,
	544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499,
	2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487,
	12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767 };

const int INDEX_TABLE[16] = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };

int16_t decodeNibble(int nibble, int& predictor, int& index)
{
	const int step = STEP_TABLE[index];
	int diff = step >> 3;
	if (nibble & 1) diff += step >> 2;
	if (nibble & 2) diff += step >> 1;
	if (nibble & 4) diff += step;
	if (nibble & 8) diff = -diff;

	predictor += diff;
	if (predictor > 32767) predictor = 32767;
	if (predictor < -32768) predictor = -32768;

	index += INDEX_TABLE[nibble & 0x0F];
	if (index < 0) index = 0;
	if (index > 88) index = 88;

	return (int16_t)predictor;
}

} // namespace

ALenum alFormatFor(unsigned int channels, unsigned int bits)
{
	if (channels == 1) {
		return bits == 8 ? AL_FORMAT_MONO8 : AL_FORMAT_MONO16;
	}
	return bits == 8 ? AL_FORMAT_STEREO8 : AL_FORMAT_STEREO16;
}

bool parseWaveHeader(const void* image, unsigned int imageSize, AILSOUNDINFO& info,
	unsigned int* dataOffset)
{
	if (image == nullptr || imageSize < 44) {
		return false;
	}

	Reader r{ (const unsigned char*)image, imageSize };
	if (!r.tag("RIFF")) {
		return false;
	}
	r.at += 8; // "RIFF" + size
	if (!r.tag("WAVE")) {
		return false;
	}
	r.at += 4;

	bool haveFormat = false;
	std::memset(&info, 0, sizeof(info));

	while (r.has(8)) {
		const bool isFormat = r.tag("fmt ");
		const bool isData = r.tag("data");
		r.at += 4;
		const uint32_t chunkSize = r.u32();

		if (isFormat) {
			if (!r.has(16)) {
				return false;
			}
			const unsigned int chunkStart = r.at;
			info.format = r.u16();
			info.channels = r.u16();
			info.rate = r.u32();
			r.u32(); // average bytes per second
			info.block_size = r.u16();
			info.bits = r.u16();
			r.at = chunkStart + chunkSize;
			haveFormat = true;
		} else if (isData) {
			if (!haveFormat || !r.has(chunkSize)) {
				return false;
			}
			info.data_ptr = r.base + r.at;
			info.initial_ptr = info.data_ptr;
			info.data_len = chunkSize;
			if (dataOffset != nullptr) {
				*dataOffset = r.at;
			}
			break;
		} else {
			r.at += chunkSize;
		}

		r.at += (chunkSize & 1); // chunks are word aligned
	}

	if (!haveFormat || info.data_ptr == nullptr || info.channels == 0) {
		return false;
	}

	if (info.format == WAVE_FORMAT_PCM) {
		const unsigned int frameBytes = info.channels * (info.bits / 8);
		info.samples = frameBytes ? info.data_len / frameBytes : 0;
	} else if (info.format == WAVE_FORMAT_IMA_ADPCM) {
		// Each ADPCM block holds a 4-byte per-channel preamble then 4 bits per sample.
		const unsigned int blocks = info.block_size ? info.data_len / info.block_size : 0;
		const unsigned int perBlock = info.block_size > 4u * info.channels
			? ((info.block_size - 4u * info.channels) * 2u) / info.channels + 1u
			: 0u;
		info.samples = blocks * perBlock;
	}

	return true;
}

bool decodeImaAdpcm(const AILSOUNDINFO& info, void** outData, unsigned long* outSize)
{
	if (outData == nullptr || outSize == nullptr || info.data_ptr == nullptr) {
		return false;
	}
	if (info.format != WAVE_FORMAT_IMA_ADPCM || info.channels < 1 || info.channels > 2) {
		return false;
	}
	if (info.block_size <= 4u * (unsigned int)info.channels) {
		return false;
	}

	const unsigned int channels = (unsigned int)info.channels;
	const unsigned int blockSize = info.block_size;
	const unsigned int blocks = info.data_len / blockSize;
	const unsigned int samplesPerBlock = ((blockSize - 4u * channels) * 2u) / channels + 1u;
	const unsigned long total = (unsigned long)blocks * samplesPerBlock * channels;

	int16_t* out = (int16_t*)std::malloc(total * sizeof(int16_t));
	if (out == nullptr) {
		return false;
	}

	const unsigned char* src = (const unsigned char*)info.data_ptr;
	int16_t* dst = out;

	for (unsigned int b = 0; b < blocks; ++b) {
		const unsigned char* block = src + (size_t)b * blockSize;

		int predictor[2] = { 0, 0 };
		int index[2] = { 0, 0 };
		for (unsigned int c = 0; c < channels; ++c) {
			predictor[c] = (int16_t)(block[c * 4] | (block[c * 4 + 1] << 8));
			index[c] = block[c * 4 + 2];
			if (index[c] > 88) index[c] = 88;
			*dst++ = (int16_t)predictor[c];
		}

		const unsigned char* data = block + 4u * channels;
		const unsigned int dataBytes = blockSize - 4u * channels;

		if (channels == 1) {
			for (unsigned int i = 0; i < dataBytes; ++i) {
				*dst++ = decodeNibble(data[i] & 0x0F, predictor[0], index[0]);
				*dst++ = decodeNibble(data[i] >> 4, predictor[0], index[0]);
			}
		} else {
			// Stereo IMA ADPCM interleaves 4-byte (8-sample) groups per channel.
			for (unsigned int i = 0; i + 8 <= dataBytes; i += 8) {
				int16_t left[8];
				int16_t right[8];
				for (unsigned int n = 0; n < 4; ++n) {
					left[n * 2] = decodeNibble(data[i + n] & 0x0F, predictor[0], index[0]);
					left[n * 2 + 1] = decodeNibble(data[i + n] >> 4, predictor[0], index[0]);
					right[n * 2] = decodeNibble(data[i + 4 + n] & 0x0F, predictor[1], index[1]);
					right[n * 2 + 1] = decodeNibble(data[i + 4 + n] >> 4, predictor[1], index[1]);
				}
				for (unsigned int n = 0; n < 8; ++n) {
					*dst++ = left[n];
					*dst++ = right[n];
				}
			}
		}
	}

	*outData = out;
	*outSize = (unsigned long)((dst - out) * sizeof(int16_t));
	return true;
}

bool decodeWaveImage(const void* image, unsigned int imageSize, DecodedAudio& out)
{
	AILSOUNDINFO info;
	if (!parseWaveHeader(image, imageSize, info, nullptr)) {
		return false;
	}

	const void* pcm = info.data_ptr;
	unsigned int pcmBytes = info.data_len;
	unsigned int bits = (unsigned int)info.bits;
	void* decoded = nullptr;

	if (info.format == WAVE_FORMAT_IMA_ADPCM) {
		unsigned long size = 0;
		if (!decodeImaAdpcm(info, &decoded, &size)) {
			return false;
		}
		pcm = decoded;
		pcmBytes = (unsigned int)size;
		bits = 16;
	} else if (info.format != WAVE_FORMAT_PCM) {
		return false;
	}

	ALuint buffer = 0;
	alGenBuffers(1, &buffer);
	if (buffer == 0) {
		std::free(decoded);
		return false;
	}

	const ALenum format = alFormatFor((unsigned int)info.channels, bits);
	alBufferData(buffer, format, pcm, (ALsizei)pcmBytes, (ALsizei)info.rate);
	std::free(decoded);

	if (alGetError() != AL_NO_ERROR) {
		alDeleteBuffers(1, &buffer);
		return false;
	}

	const unsigned int frameBytes = (unsigned int)info.channels * (bits / 8);

	out.buffer = buffer;
	out.format = format;
	out.rate = info.rate;
	out.channels = (unsigned int)info.channels;
	out.bits = bits;
	out.frames = frameBytes ? pcmBytes / frameBytes : 0;
	return true;
}

} // namespace OpenALAudio

// ---------------------------------------------------------------------------------- public API

int AIL_WAV_info(const void* data, AILSOUNDINFO* info)
{
	if (info == nullptr) {
		return 0;
	}
	// Miles took no size, so the header has to be trusted; bound the read by the RIFF size field.
	unsigned int size = 0xFFFFFFFFu;
	if (data != nullptr) {
		const unsigned char* p = (const unsigned char*)data;
		const unsigned int riff = (unsigned int)p[4] | ((unsigned int)p[5] << 8)
			| ((unsigned int)p[6] << 16) | ((unsigned int)p[7] << 24);
		if (riff < 0xFFFFFFFFu - 8u) {
			size = riff + 8u;
		}
	}
	return OpenALAudio::parseWaveHeader(data, size, *info, nullptr) ? 1 : 0;
}

int AIL_decompress_ADPCM(const AILSOUNDINFO* info, void** outdata, unsigned long* outsize)
{
	if (info == nullptr) {
		return 0;
	}
	return OpenALAudio::decodeImaAdpcm(*info, outdata, outsize) ? 1 : 0;
}

void AIL_mem_free_lock(void* ptr)
{
	// Pairs with the malloc in decodeImaAdpcm. MilesAudioManager.cpp frees ADPCM buffers here.
	std::free(ptr);
}
