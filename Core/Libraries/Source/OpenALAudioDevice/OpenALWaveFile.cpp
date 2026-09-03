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
 * so this layer owns the container and codec handling.
 *
 * The formats are the ones the retail assets actually use, measured with
 * scripts/audio-retail-survey.py over the Zero Hour audio archives: of 3523 WAV files, 2572 are
 * IMA ADPCM (block-aligned 512, 1024 or 2048) and 951 are 16-bit PCM; there is no MS ADPCM and no
 * 8-bit PCM anywhere in the retail set. 8-bit PCM is still accepted because the container allows it
 * and the cost is nil. See docs/porting/audio-retail-validation.md.
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

unsigned int imaSamplesPerBlock(unsigned int blockSize, unsigned int channels)
{
	// Each IMA ADPCM block opens with a 4-byte preamble per channel (a 16-bit predictor, a step
	// index and a pad byte) whose predictor is itself the block's first sample, then carries 4 bits
	// per sample for the rest of the block.
	if (channels == 0 || blockSize <= 4u * channels) {
		return 0;
	}
	return ((blockSize - 4u * channels) * 2u) / channels + 1u;
}

bool parseWaveInfo(const void* image, unsigned int imageSize, AILSOUNDINFO& info,
	unsigned int* dataOffset, bool requirePayload)
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
	bool haveData = false;
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
			if (!haveFormat) {
				return false;
			}
			if (requirePayload && !r.has(chunkSize)) {
				return false;
			}
			// A metadata-only parse describes a payload that is not in the buffer, so it reports the
			// payload's offset and declared length and leaves the pointers null. The stream path
			// reads the payload through the engine's file callbacks from that offset.
			info.data_ptr = r.has(chunkSize) ? r.base + r.at : nullptr;
			info.initial_ptr = info.data_ptr;
			info.data_len = chunkSize;
			if (dataOffset != nullptr) {
				*dataOffset = r.at;
			}
			haveData = true;
			break;
		} else {
			r.at += chunkSize;
		}

		r.at += (chunkSize & 1); // chunks are word aligned
	}

	if (!haveFormat || !haveData || info.channels == 0) {
		return false;
	}
	if (requirePayload && info.data_ptr == nullptr) {
		return false;
	}

	if (info.format == WAVE_FORMAT_PCM) {
		const unsigned int frameBytes = info.channels * (info.bits / 8);
		info.samples = frameBytes ? info.data_len / frameBytes : 0;
	} else if (info.format == WAVE_FORMAT_IMA_ADPCM) {
		const unsigned int blocks = info.block_size ? info.data_len / info.block_size : 0;
		info.samples = blocks * imaSamplesPerBlock(info.block_size, (unsigned int)info.channels);
	}

	return true;
}

bool parseWaveHeader(const void* image, unsigned int imageSize, AILSOUNDINFO& info,
	unsigned int* dataOffset)
{
	return parseWaveInfo(image, imageSize, info, dataOffset, true);
}

bool parseWaveMetadata(const void* image, unsigned int imageSize, AILSOUNDINFO& info,
	unsigned int* dataOffset)
{
	return parseWaveInfo(image, imageSize, info, dataOffset, false);
}

unsigned long decodeImaAdpcmBlocks(const void* payload, unsigned int blocks, unsigned int blockSize,
	unsigned int channels, int16_t* out)
{
	if (payload == nullptr || out == nullptr || channels < 1 || channels > 2
		|| blockSize <= 4u * channels) {
		return 0;
	}

	const unsigned char* src = (const unsigned char*)payload;
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

	return (unsigned long)(dst - out);
}

bool decodeImaAdpcm(const AILSOUNDINFO& info, void** outData, unsigned long* outSize)
{
	if (outData == nullptr || outSize == nullptr || info.data_ptr == nullptr) {
		return false;
	}
	if (info.format != WAVE_FORMAT_IMA_ADPCM || info.channels < 1 || info.channels > 2) {
		return false;
	}

	const unsigned int channels = (unsigned int)info.channels;
	const unsigned int blockSize = info.block_size;
	const unsigned int samplesPerBlock = imaSamplesPerBlock(blockSize, channels);
	if (samplesPerBlock == 0) {
		return false;
	}

	const unsigned int blocks = info.data_len / blockSize;
	const unsigned long total = (unsigned long)blocks * samplesPerBlock * channels;

	int16_t* out = (int16_t*)std::malloc(total * sizeof(int16_t));
	if (out == nullptr) {
		return false;
	}

	const unsigned long written = decodeImaAdpcmBlocks(info.data_ptr, blocks, blockSize, channels,
		out);
	if (written == 0 && total != 0) {
		std::free(out);
		return false;
	}

	*outData = out;
	*outSize = written * sizeof(int16_t);
	return true;
}

void* buildWaveImage(const void* pcm, unsigned long pcmBytes, unsigned int channels,
	unsigned int rate, unsigned int bits, unsigned long* imageBytes)
{
	constexpr unsigned int HEADER_BYTES = 44;
	if (channels == 0 || rate == 0 || bits == 0) {
		return nullptr;
	}

	unsigned char* image = (unsigned char*)std::malloc(HEADER_BYTES + pcmBytes);
	if (image == nullptr) {
		return nullptr;
	}

	const unsigned int frameBytes = channels * (bits / 8);
	auto put32 = [](unsigned char* at, uint32_t v) {
		at[0] = (unsigned char)(v & 0xFF);
		at[1] = (unsigned char)((v >> 8) & 0xFF);
		at[2] = (unsigned char)((v >> 16) & 0xFF);
		at[3] = (unsigned char)((v >> 24) & 0xFF);
	};
	auto put16 = [](unsigned char* at, uint16_t v) {
		at[0] = (unsigned char)(v & 0xFF);
		at[1] = (unsigned char)((v >> 8) & 0xFF);
	};

	std::memcpy(image, "RIFF", 4);
	put32(image + 4, (uint32_t)(36u + pcmBytes));
	std::memcpy(image + 8, "WAVE", 4);
	std::memcpy(image + 12, "fmt ", 4);
	put32(image + 16, 16);
	put16(image + 20, WAVE_FORMAT_PCM);
	put16(image + 22, (uint16_t)channels);
	put32(image + 24, rate);
	put32(image + 28, rate * frameBytes);
	put16(image + 32, (uint16_t)frameBytes);
	put16(image + 34, (uint16_t)bits);
	std::memcpy(image + 36, "data", 4);
	put32(image + 40, (uint32_t)pcmBytes);
	if (pcmBytes != 0) {
		std::memcpy(image + HEADER_BYTES, pcm, pcmBytes);
	}

	if (imageBytes != nullptr) {
		*imageBytes = HEADER_BYTES + pcmBytes;
	}
	return image;
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
	diagnosticsBufferData(format, (unsigned int)info.channels, bits, (unsigned int)info.rate,
		pcmBytes, "sample");
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
	if (info == nullptr || outdata == nullptr) {
		return 0;
	}

	// Miles returned a complete WAV *file image*, not bare PCM: AudioFileCache::openFile() stores
	// what comes back as the cached file and later hands the same pointer to AIL_set_sample_file /
	// AIL_set_3D_sample_file, which parse a RIFF container. Returning raw samples here made every
	// IMA ADPCM asset on the one-shot path fail to play with "unsupported sample format": 181 of the
	// 1081 files under the sounds folder, which 206 AudioEvent sample references resolve onto.
	void* pcm = nullptr;
	unsigned long pcmBytes = 0;
	if (!OpenALAudio::decodeImaAdpcm(*info, &pcm, &pcmBytes)) {
		OpenALAudio::setLastError("AIL_decompress_ADPCM: not decodable IMA ADPCM");
		return 0;
	}

	unsigned long imageBytes = 0;
	void* image = OpenALAudio::buildWaveImage(pcm, pcmBytes, (unsigned int)info->channels,
		info->rate, 16, &imageBytes);
	std::free(pcm);
	if (image == nullptr) {
		OpenALAudio::setLastError("AIL_decompress_ADPCM: out of memory");
		return 0;
	}

	*outdata = image;
	if (outsize != nullptr) {
		*outsize = imageBytes;
	}
	return 1;
}

void AIL_mem_free_lock(void* ptr)
{
	// Pairs with the malloc in buildWaveImage. MilesAudioManager.cpp frees ADPCM buffers here.
	std::free(ptr);
}
