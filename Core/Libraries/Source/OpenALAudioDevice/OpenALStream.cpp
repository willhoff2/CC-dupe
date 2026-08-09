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
 * Only PCM WAV streams are decoded. Retail Zero Hour music is MP2/MP3, which Miles decoded
 * internally and core OpenAL cannot; such streams open successfully, report zero length, and play
 * silence rather than failing. See docs/porting/audio-surface.md.
 */

#include "OpenALAudioInternal.h"

#include <algorithm>
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

/// Reads the next chunk of PCM from the stream's file through the engine's file callbacks.
/// Returns the number of bytes read, honouring the loop count by rewinding at the end.
unsigned int readChunk(StreamVoice& stream, std::vector<unsigned char>& into)
{
	Library& l = lib();
	if (!stream.decodable || l.fileRead == nullptr || l.fileSeek == nullptr
		|| stream.file == nullptr) {
		return 0;
	}

	if (stream.readCursor >= stream.dataLength) {
		if (stream.loopCount != 1) {
			stream.readCursor = 0;
			if (stream.loopCount > 1) {
				--stream.loopCount;
			}
		} else {
			return 0;
		}
	}

	unsigned int want = StreamVoice::BUFFER_FRAMES * frameBytes(stream);
	const unsigned int remaining = stream.dataLength - stream.readCursor;
	if (want > remaining) {
		want = remaining;
	}
	if (want == 0) {
		return 0;
	}

	into.resize(want);
	l.fileSeek(stream.file, (long)(stream.dataOffset + stream.readCursor), AIL_FILE_SEEK_BEGIN);
	const unsigned long read = l.fileRead(stream.file, into.data(), want);
	stream.readCursor += (unsigned int)read;
	return (unsigned int)read;
}

/// Opens the file and works out the PCM payload's extent and format.
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

	// Read enough of the front of the file to cover a WAV header with a few extra chunks.
	unsigned char header[1024];
	l.fileSeek(stream.file, 0, AIL_FILE_SEEK_BEGIN);
	const unsigned long read = l.fileRead(stream.file, header, sizeof(header));

	AILSOUNDINFO info;
	unsigned int dataOffset = 0;
	if (read >= 44 && parseWaveHeader(header, (unsigned int)read, info, &dataOffset)
		&& info.format == WAVE_FORMAT_PCM) {
		stream.channels = (unsigned int)info.channels;
		stream.bits = (unsigned int)info.bits;
		stream.rate = info.rate;
		stream.format = alFormatFor(stream.channels, stream.bits);
		stream.dataOffset = dataOffset;
		stream.dataLength = info.data_len;
		const unsigned int fb = frameBytes(stream);
		stream.totalFrames = fb ? info.data_len / fb : 0;
		stream.decodable = true;
	} else {
		// Not a PCM WAV: most likely compressed music. Keep the handle valid and silent.
		stream.decodable = false;
		stream.channels = 2;
		stream.bits = 16;
		stream.rate = 44100;
		stream.format = AL_FORMAT_STEREO16;
		stream.totalFrames = 0;
	}

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
	// Sub-region looping. Not implemented; no engine call site relies on it taking effect.
	(void)stream_handle;
	(void)loop_start;
	(void)loop_end;
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
	if (stream == nullptr || !stream->decodable) {
		return;
	}

	std::lock_guard<std::recursive_mutex> guard(lib().lock);

	const unsigned int fb = frameBytes(*stream);
	const unsigned int byteOffset = (unsigned int)((unsigned long long)pos * stream->rate / 1000ull) * fb;
	stream->readCursor = (byteOffset < stream->dataLength) ? byteOffset : stream->dataLength;
	stream->framesPlayed = fb ? stream->readCursor / fb : 0;

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
