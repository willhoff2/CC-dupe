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
 * @brief Streaming voices (music and long dialogue).
 *
 * Streams are opened by filename and read through the callbacks installed with
 * AIL_set_file_callbacks, because the engine's music lives inside its own .big archives and only
 * the engine can read them. Buffers are refilled by the service thread in OpenALDriver.cpp.
 *
 * WAV streams are decoded, PCM and IMA ADPCM alike: the retail survey
 * (scripts/audio-retail-survey.py) finds 2442 files under the streaming folder, 2391 IMA ADPCM and
 * 51 PCM, and the 2569 Zero Hour DialogEvents resolve onto 2429 of them, so a PCM-only stream path
 * plays almost none of the retail dialogue.
 *
 * Retail music is MP3 (7 tracks in MusicZH.big) and no MPEG decoder is linked, so those streams
 * FAIL to open with an explicit error instead of becoming zero-length silent streams.
 * See docs/porting/audio-retail-validation.md.
 */

#include "OpenALAudioInternal.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

namespace OpenALAudio
{
namespace
{

StreamVoice* streamOf(HSTREAM handle)
{
	return reinterpret_cast<StreamVoice*>(handle);
}

unsigned int frameBytes(const StreamVoice& stream)
{
	return stream.channels * (stream.bits / 8);
}

/// Largest header a stream will read looking for the `data` chunk. Retail WAV headers are 44 to 58
/// bytes; the bound exists only so a corrupt or non-WAV file cannot make this read the whole file.
constexpr unsigned int MAX_HEADER_BYTES = 64u * 1024u;

/// Byte offset in the payload where playback stops, honouring any loop block.
unsigned int playbackEnd(const StreamVoice& stream)
{
	if (stream.loopEnd != 0 && stream.loopEnd < stream.dataLength) {
		return stream.loopEnd;
	}
	return stream.dataLength;
}

/// Rounds a payload byte offset down onto a boundary the decoder can start from: a frame for PCM,
/// a whole block for ADPCM, whose predictor state lives in the block preamble.
unsigned int alignToCodecBoundary(const StreamVoice& stream, unsigned int offset)
{
	const unsigned int unit = (stream.codec == StreamCodec::ImaAdpcm)
		? stream.blockSize
		: stream.channels * (stream.bits / 8);
	if (unit == 0) {
		return 0;
	}
	if (offset > stream.dataLength) {
		offset = stream.dataLength;
	}
	return offset - (offset % unit);
}

/// True when the file's first bytes are an MPEG audio elementary stream (an ID3 tag or a frame
/// sync). Used only to name the codec in the error message when a stream cannot be decoded.
bool looksLikeMpeg(const unsigned char* front, size_t size)
{
	if (size >= 3 && std::memcmp(front, "ID3", 3) == 0) {
		return true;
	}
	return size >= 2 && front[0] == 0xFF && (front[1] & 0xE0) == 0xE0;
}

/// Reads the next chunk of the stream's file through the engine's file callbacks and decodes it to
/// PCM. Returns the number of PCM bytes produced, honouring the loop count by rewinding at the end.
unsigned int readChunk(StreamVoice& stream, std::vector<unsigned char>& into)
{
	Library& l = lib();
	if (l.fileRead == nullptr || l.fileSeek == nullptr || stream.file == nullptr) {
		return 0;
	}

	const unsigned int end = playbackEnd(stream);
	if (stream.readCursor >= end) {
		if (stream.loopCount != 1) {
			stream.readCursor = stream.loopStart;
			if (stream.loopCount > 1) {
				--stream.loopCount;
			}
		} else {
			return 0;
		}
	}

	const unsigned int remaining = end - stream.readCursor;

	// How many file bytes make about BUFFER_FRAMES of PCM. For ADPCM that is a whole number of
	// blocks, because a block is the unit that carries its own predictor state.
	unsigned int want = 0;
	if (stream.codec == StreamCodec::ImaAdpcm) {
		const unsigned int blocks = stream.samplesPerBlock
			? (StreamVoice::BUFFER_FRAMES + stream.samplesPerBlock - 1) / stream.samplesPerBlock
			: 0;
		want = blocks * stream.blockSize;
	} else {
		want = StreamVoice::BUFFER_FRAMES * frameBytes(stream);
	}
	if (want > remaining) {
		want = remaining;
	}
	if (want == 0) {
		return 0;
	}

	std::vector<unsigned char> raw(want);
	l.fileSeek(stream.file, (long)(stream.dataOffset + stream.readCursor), AIL_FILE_SEEK_BEGIN);
	const unsigned long read = l.fileRead(stream.file, raw.data(), want);
	if (read == 0) {
		return 0;
	}

	if (stream.codec == StreamCodec::Pcm) {
		stream.readCursor += (unsigned int)read;
		into.assign(raw.begin(), raw.begin() + (size_t)read);
		return (unsigned int)read;
	}

	const unsigned int blocks = (unsigned int)(read / stream.blockSize);
	if (blocks == 0) {
		// A partial trailing block cannot be decoded: its predictor preamble may be missing.
		stream.readCursor = end;
		return 0;
	}

	into.resize((size_t)blocks * stream.samplesPerBlock * stream.channels * sizeof(int16_t));
	const unsigned long samples = decodeImaAdpcmBlocks(raw.data(), blocks, stream.blockSize,
		stream.channels, (int16_t*)into.data());
	stream.readCursor += blocks * stream.blockSize;
	into.resize((size_t)samples * sizeof(int16_t));
	return (unsigned int)(samples * sizeof(int16_t));
}

/// Reads the front of the file until the WAV metadata parses, and reports the file's size.
///
/// The window is sized by where the `data` chunk *starts*, which is all that parsing needs; the
/// payload is read later through the file callbacks. The previous code required the whole payload to
/// sit inside a fixed 1024-byte read, so every stream longer than about a kilobyte -- which is every
/// retail speech file -- failed to parse and fell back to a silent zero-length stream.
bool readWaveMetadata(StreamVoice& stream, AILSOUNDINFO& info, unsigned int& dataOffset,
	unsigned int& fileSize, std::vector<unsigned char>& front)
{
	Library& l = lib();

	const long end = l.fileSeek(stream.file, 0, AIL_FILE_SEEK_END);
	fileSize = (end > 0) ? (unsigned int)end : 0;

	unsigned int window = 1024;
	for (;;) {
		const bool wholeFile = (fileSize != 0 && window >= fileSize);
		if (wholeFile) {
			window = fileSize;
		}
		front.assign(window, 0);
		l.fileSeek(stream.file, 0, AIL_FILE_SEEK_BEGIN);
		const unsigned long read = l.fileRead(stream.file, front.data(), window);
		front.resize((size_t)read);
		if (read == 0) {
			return false;
		}
		if (parseWaveMetadata(front.data(), (unsigned int)read, info, &dataOffset)) {
			return true;
		}
		// Nothing more to look at: the whole file, a short read, or the bound has been reached.
		if (wholeFile || read < window || window >= MAX_HEADER_BYTES) {
			return false;
		}
		window *= 2;
	}
}

/// Opens the file and works out the payload's extent and codec. Fails, with a reportable error, for
/// anything it cannot actually decode: a stream that opens and then plays silence is
/// indistinguishable from a working one at every call site in MilesAudioManager.
bool openStreamFile(StreamVoice& stream, const char* filename)
{
	Library& l = lib();
	if (l.fileOpen == nullptr || l.fileRead == nullptr || l.fileSeek == nullptr) {
		setLastError("AIL_set_file_callbacks has not been called");
		return false;
	}

	if (l.fileOpen(filename, &stream.file) == 0 || stream.file == nullptr) {
		setLastError("stream open failed");
		return false;
	}

	stream.fileName = filename;

	AILSOUNDINFO info;
	unsigned int dataOffset = 0;
	unsigned int fileSize = 0;
	std::vector<unsigned char> front;
	if (!readWaveMetadata(stream, info, dataOffset, fileSize, front)) {
		if (looksLikeMpeg(front.data(), front.size())) {
			// UNIMPLEMENTED, and required rather than absent: the 7 retail Zero Hour music tracks in
			// MusicZH.big are all MPEG-1 layer III, 44100 Hz stereo. Miles decoded MP3 internally;
			// core OpenAL has no decoder and none is linked yet.
			setLastError("MPEG audio streams (MP1/MP2/MP3) are not implemented: no MPEG decoder "
				"is linked (retail Zero Hour music is MP3; "
				"see docs/porting/audio-retail-validation.md)");
		} else {
			setLastError("stream is not a WAV file, or its header could not be parsed");
		}
		return false;
	}

	stream.channels = (unsigned int)info.channels;
	stream.rate = info.rate;
	stream.dataOffset = dataOffset;
	stream.dataLength = info.data_len;
	if (fileSize > dataOffset && stream.dataLength > fileSize - dataOffset) {
		// A truncated or mis-declared file must not be read past its end through the callbacks.
		stream.dataLength = fileSize - dataOffset;
	}

	if (info.format == WAVE_FORMAT_PCM) {
		stream.codec = StreamCodec::Pcm;
		stream.bits = (unsigned int)info.bits;
		const unsigned int fb = frameBytes(stream);
		if (fb == 0) {
			setLastError("PCM stream has no frame size");
			return false;
		}
		stream.dataLength -= stream.dataLength % fb;
		stream.totalFrames = stream.dataLength / fb;
	} else if (info.format == WAVE_FORMAT_IMA_ADPCM) {
		stream.codec = StreamCodec::ImaAdpcm;
		stream.bits = 16;	// what the decoder produces, not what the file holds
		stream.blockSize = info.block_size;
		stream.samplesPerBlock = imaSamplesPerBlock(stream.blockSize, stream.channels);
		if (stream.samplesPerBlock == 0) {
			setLastError("IMA ADPCM stream has an unusable block alignment");
			return false;
		}
		stream.dataLength -= stream.dataLength % stream.blockSize;
		stream.totalFrames = (stream.dataLength / stream.blockSize) * stream.samplesPerBlock;
	} else {
		char message[128];
		std::snprintf(message, sizeof(message),
			"unsupported stream codec 0x%04x (only PCM and IMA ADPCM WAV are decoded)",
			(unsigned int)info.format);
		setLastError(message);
		return false;
	}

	if (stream.channels == 0 || stream.rate == 0 || stream.totalFrames == 0) {
		setLastError("stream header describes no audio");
		return false;
	}

	stream.format = alFormatFor(stream.channels, stream.bits);
	return true;
}

} // namespace

void serviceStream(StreamVoice& stream, bool& finished)
{
	finished = false;
	if (stream.source == 0) {
		return;
	}

	ALint processed = 0;
	alGetSourcei(stream.source, AL_BUFFERS_PROCESSED, &processed);
	while (processed-- > 0) {
		ALuint buffer = 0;
		alSourceUnqueueBuffers(stream.source, 1, &buffer);

		ALint size = 0;
		alGetBufferi(buffer, AL_SIZE, &size);
		const unsigned int fb = frameBytes(stream);
		if (fb != 0) {
			stream.framesPlayed += (unsigned int)size / fb;
		}

		std::vector<unsigned char> chunk;
		const unsigned int read = readChunk(stream, chunk);
		if (read == 0) {
			stream.exhausted = true;
			continue;
		}
		alBufferData(buffer, stream.format, chunk.data(), (ALsizei)read, (ALsizei)stream.rate);
		alSourceQueueBuffers(stream.source, 1, &buffer);
	}

	ALint queued = 0;
	alGetSourcei(stream.source, AL_BUFFERS_QUEUED, &queued);
	if (queued == 0) {
		finished = true;
		return;
	}

	// A starved source stops on its own; restart it so playback continues after a slow refill.
	ALint state = 0;
	alGetSourcei(stream.source, AL_SOURCE_STATE, &state);
	if (state == AL_STOPPED) {
		alSourcePlay(stream.source);
	}
}

} // namespace OpenALAudio

using namespace OpenALAudio;

// -------------------------------------------------------------------------- open and close

HSTREAM AIL_open_stream(HDIGDRIVER dig, const char* filename, int stream_mem)
{
	(void)dig;
	(void)stream_mem;

	Library& l = lib();
	std::lock_guard<std::recursive_mutex> guard(l.lock);

	if (!l.started || filename == nullptr) {
		return nullptr;
	}

	StreamVoice* stream = new StreamVoice();
	if (!openStreamFile(*stream, filename)) {
		// openStreamFile has set the error; the caller reads it back with AIL_last_error().
		if (stream->file != nullptr && l.fileClose != nullptr) {
			l.fileClose(stream->file);
		}
		delete stream;
		return nullptr;
	}

	alGenSources(1, &stream->source);
	if (stream->source == 0) {
		if (l.fileClose != nullptr) l.fileClose(stream->file);
		delete stream;
		setLastError("alGenSources failed for stream");
		return nullptr;
	}

	alGenBuffers(StreamVoice::BUFFER_COUNT, stream->buffers);
	alSourcei(stream->source, AL_SOURCE_RELATIVE, AL_TRUE);
	alSourcef(stream->source, AL_ROLLOFF_FACTOR, 0.0f);
	applyVolumePan(stream->source, stream->volume, stream->pan);

	l.streams.push_back(stream);
	return (HSTREAM)stream;
}

HSTREAM AIL_open_stream_by_sample(HDIGDRIVER driver, HSAMPLE sample, const char* file_name, int mem)
{
	// Miles could bind a stream to an already-allocated 2D voice so the caller kept its handle.
	// Here streams own their own OpenAL source, so the sample handle is not needed.
	(void)sample;
	return AIL_open_stream(driver, file_name, mem);
}

void AIL_close_stream(HSTREAM stream_handle)
{
	StreamVoice* stream = streamOf(stream_handle);
	if (stream == nullptr) {
		return;
	}

	Library& l = lib();
	std::lock_guard<std::recursive_mutex> guard(l.lock);

	if (stream->source != 0) {
		alSourceStop(stream->source);
		alSourcei(stream->source, AL_BUFFER, 0);
		alDeleteSources(1, &stream->source);
	}
	alDeleteBuffers(StreamVoice::BUFFER_COUNT, stream->buffers);
	if (stream->file != nullptr && l.fileClose != nullptr) {
		l.fileClose(stream->file);
	}

	l.streams.erase(std::remove(l.streams.begin(), l.streams.end(), stream), l.streams.end());
	delete stream;
}

// ----------------------------------------------------------------------------------- transport

void AIL_start_stream(HSTREAM stream_handle)
{
	StreamVoice* stream = streamOf(stream_handle);
	if (stream == nullptr || stream->source == 0) {
		return;
	}

	Library& l = lib();
	std::lock_guard<std::recursive_mutex> guard(l.lock);

	// Prime the queue before starting so the source does not immediately starve.
	for (unsigned int i = 0; i < StreamVoice::BUFFER_COUNT; ++i) {
		std::vector<unsigned char> chunk;
		const unsigned int read = readChunk(*stream, chunk);
		if (read == 0) {
			break;
		}
		alBufferData(stream->buffers[i], stream->format, chunk.data(), (ALsizei)read,
			(ALsizei)stream->rate);
		alSourceQueueBuffers(stream->source, 1, &stream->buffers[i]);
	}

	alSourcePlay(stream->source);
	stream->playing = true;
	stream->paused = false;
	stream->framesPlayed = 0;
}

void AIL_pause_stream(HSTREAM stream_handle, int onoff)
{
	StreamVoice* stream = streamOf(stream_handle);
	if (stream == nullptr || stream->source == 0) {
		return;
	}
	std::lock_guard<std::recursive_mutex> guard(lib().lock);
	if (onoff != 0) {
		alSourcePause(stream->source);
		stream->paused = true;
	} else {
		alSourcePlay(stream->source);
		stream->paused = false;
		stream->playing = true;
	}
}

// -------------------------------------------------------------------------- volume and pan

int AIL_stream_volume(HSTREAM stream_handle)
{
	StreamVoice* stream = streamOf(stream_handle);
	return (stream != nullptr) ? (int)(stream->volume * MILES_MAX_INT_VOLUME) : 0;
}

void AIL_set_stream_volume(HSTREAM stream_handle, int volume)
{
	StreamVoice* stream = streamOf(stream_handle);
	if (stream == nullptr) {
		return;
	}
	std::lock_guard<std::recursive_mutex> guard(lib().lock);
	stream->volume = (float)volume / (float)MILES_MAX_INT_VOLUME;
	applyVolumePan(stream->source, stream->volume, stream->pan);
}

int AIL_stream_pan(HSTREAM stream_handle)
{
	StreamVoice* stream = streamOf(stream_handle);
	return (stream != nullptr) ? (int)(stream->pan * MILES_MAX_INT_VOLUME) : 0;
}

void AIL_set_stream_pan(HSTREAM stream_handle, int pan)
{
	StreamVoice* stream = streamOf(stream_handle);
	if (stream == nullptr) {
		return;
	}
	std::lock_guard<std::recursive_mutex> guard(lib().lock);
	stream->pan = (float)pan / (float)MILES_MAX_INT_VOLUME;
	applyVolumePan(stream->source, stream->volume, stream->pan);
}

void AIL_stream_volume_pan(HSTREAM stream_handle, float* volume, float* pan)
{
	StreamVoice* stream = streamOf(stream_handle);
	if (stream == nullptr) {
		return;
	}
	if (volume != nullptr) *volume = stream->volume;
	if (pan != nullptr) *pan = stream->pan;
}

void AIL_set_stream_volume_pan(HSTREAM stream_handle, float volume, float pan)
{
	StreamVoice* stream = streamOf(stream_handle);
	if (stream == nullptr) {
		return;
	}
	std::lock_guard<std::recursive_mutex> guard(lib().lock);
	stream->volume = volume;
	stream->pan = pan;
	applyVolumePan(stream->source, volume, pan);
}

// ------------------------------------------------------------------- looping and position

int AIL_stream_loop_count(HSTREAM stream_handle)
{
	StreamVoice* stream = streamOf(stream_handle);
	return (stream != nullptr) ? stream->loopCount : 0;
}

void AIL_set_stream_loop_count(HSTREAM stream_handle, int count)
{
	StreamVoice* stream = streamOf(stream_handle);
	if (stream != nullptr) {
		stream->loopCount = count;
	}
}

void AIL_set_stream_loop_block(HSTREAM stream_handle, int loop_start, int loop_end)
{
	// Byte offsets into the stream's payload; a negative end means "to the end of the file", which
	// is what SoundStreamHandleClass::Set_Sample_Loop_Count passes. Offsets are pulled back onto a
	// decodable boundary, so a caller cannot ask playback to resume mid-ADPCM-block.
	StreamVoice* stream = streamOf(stream_handle);
	if (stream == nullptr) {
		return;
	}
	std::lock_guard<std::recursive_mutex> guard(lib().lock);

	stream->loopStart = alignToCodecBoundary(*stream,
		(loop_start > 0) ? (unsigned int)loop_start : 0u);
	stream->loopEnd = (loop_end > 0)
		? alignToCodecBoundary(*stream, (unsigned int)loop_end)
		: 0u;
	if (stream->loopEnd != 0 && stream->loopEnd <= stream->loopStart) {
		stream->loopEnd = 0;
	}
}

void AIL_stream_ms_position(HSTREAM stream_handle, S32* total_milliseconds, S32* current_milliseconds)
{
	StreamVoice* stream = streamOf(stream_handle);
	if (stream == nullptr) {
		if (total_milliseconds != nullptr) *total_milliseconds = 0;
		if (current_milliseconds != nullptr) *current_milliseconds = 0;
		return;
	}

	std::lock_guard<std::recursive_mutex> guard(lib().lock);

	// MilesAudioManager opens a stream purely to read its length before playing anything, so the
	// total must be known from the header rather than from playback progress.
	if (total_milliseconds != nullptr) {
		*total_milliseconds = stream->rate
			? (S32)((unsigned long long)stream->totalFrames * 1000ull / stream->rate)
			: 0;
	}

	if (current_milliseconds != nullptr) {
		unsigned int frames = stream->framesPlayed;
		if (stream->source != 0) {
			ALint offset = 0;
			alGetSourcei(stream->source, AL_SAMPLE_OFFSET, &offset);
			frames += (unsigned int)offset;
		}
		*current_milliseconds = stream->rate
			? (S32)((unsigned long long)frames * 1000ull / stream->rate)
			: 0;
	}
}

void AIL_set_stream_ms_position(HSTREAM stream_handle, int pos)
{
	StreamVoice* stream = streamOf(stream_handle);
	if (stream == nullptr || stream->rate == 0) {
		return;
	}

	std::lock_guard<std::recursive_mutex> guard(lib().lock);

	const unsigned long long frame = (unsigned long long)(pos > 0 ? pos : 0) * stream->rate / 1000ull;
	unsigned int byteOffset = 0;
	if (stream->codec == StreamCodec::ImaAdpcm) {
		// Seeking is only exact to a block: a block's samples cannot be decoded without its
		// predictor preamble.
		byteOffset = (unsigned int)(frame / stream->samplesPerBlock) * stream->blockSize;
	} else {
		byteOffset = (unsigned int)frame * frameBytes(*stream);
	}
	stream->readCursor = alignToCodecBoundary(*stream, byteOffset);

	if (stream->codec == StreamCodec::ImaAdpcm) {
		stream->framesPlayed = (stream->readCursor / stream->blockSize) * stream->samplesPerBlock;
	} else {
		const unsigned int fb = frameBytes(*stream);
		stream->framesPlayed = fb ? stream->readCursor / fb : 0;
	}

	// Requeue from the new position.
	if (stream->source != 0) {
		alSourceStop(stream->source);
		ALint queued = 0;
		alGetSourcei(stream->source, AL_BUFFERS_QUEUED, &queued);
		while (queued-- > 0) {
			ALuint buffer = 0;
			alSourceUnqueueBuffers(stream->source, 1, &buffer);
		}
		if (stream->playing) {
			AIL_start_stream(stream_handle);
		}
	}
}

int AIL_stream_playback_rate(HSTREAM stream_handle)
{
	StreamVoice* stream = streamOf(stream_handle);
	if (stream == nullptr) {
		return 0;
	}
	return (stream->playbackRate != 0) ? stream->playbackRate : (int)stream->rate;
}

void AIL_set_stream_playback_rate(HSTREAM stream_handle, int rate)
{
	StreamVoice* stream = streamOf(stream_handle);
	if (stream == nullptr) {
		return;
	}
	std::lock_guard<std::recursive_mutex> guard(lib().lock);
	stream->playbackRate = rate;
	applyPlaybackRate(stream->source, rate, stream->rate);
}

AIL_stream_callback AIL_register_stream_callback(HSTREAM stream_handle, AIL_stream_callback callback)
{
	StreamVoice* stream = streamOf(stream_handle);
	if (stream == nullptr) {
		return nullptr;
	}
	std::lock_guard<std::recursive_mutex> guard(lib().lock);
	AIL_stream_callback previous = stream->endOfStream;
	stream->endOfStream = callback;
	return previous;
}
