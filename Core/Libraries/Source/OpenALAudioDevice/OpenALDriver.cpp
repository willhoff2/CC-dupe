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
 * @brief Library lifecycle, digital driver, providers and filters.
 */

#include "OpenALAudioInternal.h"

#include <chrono>
#include <cstdio>
#include <cstring>

namespace OpenALAudio
{

Library& lib()
{
	static Library instance;
	return instance;
}

void setLastError(const char* message)
{
	lib().lastError = (message != nullptr) ? message : "";
}

void applyVolumePan(ALuint source, float volume, float pan)
{
	if (source == 0) {
		return;
	}
	if (volume < 0.0f) volume = 0.0f;
	if (volume > 1.0f) volume = 1.0f;
	if (pan < 0.0f) pan = 0.0f;
	if (pan > 1.0f) pan = 1.0f;

	alSourcef(source, AL_GAIN, volume);
	// OpenAL has no pan control. For a listener-relative source with no attenuation, offsetting
	// along x reproduces a constant-power pan closely enough for 2D voices.
	alSource3f(source, AL_POSITION, (pan * 2.0f) - 1.0f, 0.0f, 0.0f);
}

void applyPlaybackRate(ALuint source, int playbackRate, unsigned int nativeRate)
{
	if (source == 0 || nativeRate == 0 || playbackRate <= 0) {
		return;
	}
	alSourcef(source, AL_PITCH, (float)playbackRate / (float)nativeRate);
}

namespace
{

/// The one synthetic 3D provider we advertise. The engine enumerates providers by name and picks
/// one; there is only ever one OpenAL output, so it is reported as "OpenAL".
char PROVIDER_NAME[] = "OpenAL";
char FILTER_NAME[] = "OpenAL Filter";
h3DPOBJECT THE_3D_PROVIDER = {};
h3DPOBJECT THE_FILTER_PROVIDER = {};

void serviceLoop()
{
	Library& l = lib();
	while (!l.serviceQuit.load()) {
		// Miles dispatched end-of-sample callbacks from its mixer thread. Poll source state and do
		// the same. Callbacks are invoked without the library lock held: the engine's handlers take
		// AIL_lock themselves, and holding it here would deadlock against them.
		std::vector<std::pair<AIL_sample_callback, HSAMPLE>> sampleDone;
		std::vector<std::pair<AIL_3dsample_callback, H3DPOBJECT>> objectDone;
		std::vector<std::pair<AIL_stream_callback, HSTREAM>> streamDone;

		{
			std::lock_guard<std::recursive_mutex> guard(l.lock);

			for (SampleVoice* sample : l.samples) {
				if (!sample->started || sample->paused || sample->source == 0) {
					continue;
				}
				ALint state = 0;
				alGetSourcei(sample->source, AL_SOURCE_STATE, &state);
				if (state == AL_STOPPED) {
					sample->started = false;
					if (sample->endOfSample != nullptr) {
						sampleDone.emplace_back(sample->endOfSample, (HSAMPLE)sample);
					}
				}
			}

			for (Object3D* object : l.objects) {
				if (object->isListener || !object->voice.started || object->voice.paused
					|| object->voice.source == 0) {
					continue;
				}
				ALint state = 0;
				alGetSourcei(object->voice.source, AL_SOURCE_STATE, &state);
				if (state == AL_STOPPED) {
					object->voice.started = false;
					if (object->endOfSample != nullptr) {
						objectDone.emplace_back(object->endOfSample, (H3DPOBJECT)object);
					}
				}
			}

			for (StreamVoice* stream : l.streams) {
				if (!stream->playing || stream->paused) {
					continue;
				}
				bool finished = false;
				serviceStream(*stream, finished);
				if (finished) {
					stream->playing = false;
					if (stream->endOfStream != nullptr) {
						streamDone.emplace_back(stream->endOfStream, (HSTREAM)stream);
					}
				}
			}
		}

		for (auto& done : sampleDone) done.first(done.second);
		for (auto& done : objectDone) done.first(done.second);
		for (auto& done : streamDone) done.first(done.second);

		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
}

} // namespace

void startServiceThread()
{
	Library& l = lib();
	if (l.service.joinable()) {
		return;
	}
	l.serviceQuit.store(false);
	l.service = std::thread(serviceLoop);
}

void stopServiceThread()
{
	Library& l = lib();
	if (!l.service.joinable()) {
		return;
	}
	l.serviceQuit.store(true);
	l.service.join();
}

} // namespace OpenALAudio

using namespace OpenALAudio;

// ------------------------------------------------------------------------- library lifecycle

int AIL_startup(void)
{
	Library& l = lib();
	std::lock_guard<std::recursive_mutex> guard(l.lock);

	if (l.started) {
		return AIL_NO_ERROR;
	}

	l.device = alcOpenDevice(nullptr);
	if (l.device == nullptr) {
		setLastError("alcOpenDevice failed: no OpenAL output device");
		return -1;
	}

	l.context = alcCreateContext(l.device, nullptr);
	if (l.context == nullptr || alcMakeContextCurrent(l.context) == ALC_FALSE) {
		if (l.context != nullptr) {
			alcDestroyContext(l.context);
			l.context = nullptr;
		}
		alcCloseDevice(l.device);
		l.device = nullptr;
		setLastError("alcCreateContext failed");
		return -1;
	}

	// The engine works in a left-handed world with a listener orientation it sets explicitly, and
	// its distance model is driven by per-sample min/max distances.
	alDistanceModel(AL_LINEAR_DISTANCE_CLAMPED);

	l.started = true;
	setLastError(nullptr);
	startServiceThread();
	return AIL_NO_ERROR;
}

void AIL_shutdown(void)
{
	Library& l = lib();

	stopServiceThread();

	std::lock_guard<std::recursive_mutex> guard(l.lock);
	if (!l.started) {
		return;
	}

	for (SampleVoice* sample : l.samples) {
		if (sample->source != 0) alDeleteSources(1, &sample->source);
		if (sample->audio.buffer != 0) alDeleteBuffers(1, &sample->audio.buffer);
		delete sample;
	}
	l.samples.clear();

	for (Object3D* object : l.objects) {
		if (object->voice.source != 0) alDeleteSources(1, &object->voice.source);
		if (object->voice.audio.buffer != 0) alDeleteBuffers(1, &object->voice.audio.buffer);
		delete object;
	}
	l.objects.clear();
	l.listener = nullptr;

	for (StreamVoice* stream : l.streams) {
		if (stream->source != 0) alDeleteSources(1, &stream->source);
		alDeleteBuffers(StreamVoice::BUFFER_COUNT, stream->buffers);
		if (stream->file != nullptr && l.fileClose != nullptr) l.fileClose(stream->file);
		delete stream;
	}
	l.streams.clear();

	alcMakeContextCurrent(nullptr);
	if (l.context != nullptr) alcDestroyContext(l.context);
	if (l.device != nullptr) alcCloseDevice(l.device);
	l.context = nullptr;
	l.device = nullptr;
	l.started = false;
	l.driver.open = false;
}

int AIL_set_preference(unsigned int number, int value)
{
	// The engine sets exactly two preferences: AIL_LOCK_PROTECTION and DIG_USE_WAVEOUT. Neither
	// has an OpenAL analogue — locking is always on here, and there is no waveOut path — so both
	// are accepted and reported as successful rather than failing the caller's assertions.
	(void)number;
	(void)value;
	return AIL_NO_ERROR;
}

char* AIL_set_redist_directory(const char* dir)
{
	Library& l = lib();
	l.redistDirectory = (dir != nullptr) ? dir : "";
	return const_cast<char*>(l.redistDirectory.c_str());
}

void AIL_MSS_version(char* buffer, unsigned int size)
{
	if (buffer == nullptr || size == 0) {
		return;
	}
	const char* version = alGetString(AL_VERSION);
	std::snprintf(buffer, size, "OpenAL %s", (version != nullptr) ? version : "(not started)");
}

char* AIL_last_error(void)
{
	Library& l = lib();
	return l.lastError.empty() ? nullptr : const_cast<char*>(l.lastError.c_str());
}

void AIL_lock(void)
{
	lib().lock.lock();
}

void AIL_unlock(void)
{
	lib().lock.unlock();
}

void AIL_stop_timer(HTIMER timer)
{
	// The engine's only timer drove WWAudio's periodic update, which it now drives itself; there
	// is no Miles timer service to stop.
	(void)timer;
}

void AIL_release_timer_handle(HTIMER timer)
{
	(void)timer;
}

unsigned long AIL_get_timer_highest_delay(void)
{
	// Reported latency. The service thread polls at 10 ms, which bounds callback delivery.
	return 10;
}

// ---------------------------------------------------------------------------- digital driver

int AIL_waveOutOpen(HDIGDRIVER* driver, LPHWAVEOUT* waveout, int id, LPWAVEFORMAT format)
{
	(void)waveout;
	(void)id;

	Library& l = lib();
	std::lock_guard<std::recursive_mutex> guard(l.lock);

	if (!l.started && AIL_startup() != AIL_NO_ERROR) {
		return -1;
	}

	if (format != nullptr) {
		l.driver.rate = format->nSamplesPerSec ? format->nSamplesPerSec : 44100;
		l.driver.channels = format->nChannels ? format->nChannels : 2;
		l.driver.bits = format->wBitsPerSample ? format->wBitsPerSample : 16;
	}

	// WWAudio.cpp opens the driver once to test whether it is a DirectSound emulation, then
	// reopens accordingly. OpenAL is never a DirectSound emulation, so the first probe succeeds
	// and the fallback path is never taken.
	l.driver.base.emulated_ds = 0;
	l.driver.open = true;

	if (driver != nullptr) {
		*driver = (HDIGDRIVER)&l.driver;
	}
	return AIL_NO_ERROR;
}

void AIL_waveOutClose(HDIGDRIVER driver)
{
	(void)driver;
	lib().driver.open = false;
}

void AIL_get_DirectSound_info(HSAMPLE sample, AILLPDIRECTSOUND* lplpDS, AILLPDIRECTSOUNDBUFFER* lplpDSB)
{
	// There is no DirectSound. The only caller is the Bink video handoff, which is Windows-only.
	(void)sample;
	if (lplpDS != nullptr) *lplpDS = nullptr;
	if (lplpDSB != nullptr) *lplpDSB = nullptr;
}

// -------------------------------------------------------------------- providers and filters

int AIL_enumerate_3D_providers(HPROENUM* next, HPROVIDER* dest, char** name)
{
	if (next == nullptr || dest == nullptr) {
		return 0;
	}
	if (*next != HPROENUM_FIRST) {
		return 0;
	}
	*next = 1;
	*dest = (HPROVIDER)&THE_3D_PROVIDER;
	if (name != nullptr) {
		*name = PROVIDER_NAME;
	}
	return 1;
}

M3DRESULT AIL_open_3D_provider(HPROVIDER lib_handle)
{
	if (lib_handle != (HPROVIDER)&THE_3D_PROVIDER) {
		setLastError("unknown 3D provider");
		return -1;
	}
	return (AIL_startup() == AIL_NO_ERROR) ? M3D_NOERR : -1;
}

void AIL_close_3D_provider(HPROVIDER lib_handle)
{
	(void)lib_handle;
}

void AIL_set_3D_speaker_type(HPROVIDER lib_handle, int speaker_type)
{
	// OpenAL Soft selects its own output channel configuration from the system. The request is
	// recorded so that the engine's own queries stay self-consistent, but it is not honoured.
	(void)lib_handle;
	lib().speakerType = speaker_type;
}

int AIL_enumerate_filters(HPROENUM* next, HPROVIDER* dest, char** name)
{
	if (next == nullptr || dest == nullptr) {
		return 0;
	}
	if (*next != HPROENUM_FIRST) {
		return 0;
	}
	*next = 1;
	*dest = (HPROVIDER)&THE_FILTER_PROVIDER;
	if (name != nullptr) {
		*name = FILTER_NAME;
	}
	return 1;
}

void AIL_set_file_callbacks(AIL_file_open_callback opencb, AIL_file_close_callback closecb,
	AIL_file_seek_callback seekcb, AIL_file_read_callback readcb)
{
	Library& l = lib();
	std::lock_guard<std::recursive_mutex> guard(l.lock);
	l.fileOpen = opencb;
	l.fileClose = closecb;
	l.fileSeek = seekcb;
	l.fileRead = readcb;
}
