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
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>

namespace OpenALAudio
{

Library& lib()
{
	static Library instance;
	return instance;
}

// ------------------------------------------------------------------------------- diagnostics

Diagnostics& diagnostics()
{
	static Diagnostics instance;
	return instance;
}

namespace
{

unsigned long elapsedUs(std::chrono::steady_clock::time_point from,
	std::chrono::steady_clock::time_point to)
{
	return (unsigned long)std::chrono::duration_cast<std::chrono::microseconds>(to - from).count();
}

/// Seconds between periodic `counters` lines from the service thread.
constexpr unsigned long DIAG_REPORT_SECONDS = 10;

} // namespace

void diagnosticsInit()
{
	Diagnostics& d = diagnostics();
	if (d.enabled) {
		return;
	}
	const char* where = std::getenv("OPENAL_AUDIO_DIAG");
	if (where == nullptr || *where == '\0' || std::strcmp(where, "0") == 0) {
		return;
	}
	if (std::strcmp(where, "stderr") == 0 || std::strcmp(where, "1") == 0) {
		d.log = stderr;
	} else {
		d.log = std::fopen(where, "a");
		if (d.log == nullptr) {
			d.log = stderr;
		}
	}
	d.started = std::chrono::steady_clock::now();
	d.lastReport = d.started;
	d.enabled = true;
}

void diagnosticsLog(const char* format, ...)
{
	Diagnostics& d = diagnostics();
	if (!d.enabled || d.log == nullptr) {
		return;
	}
	const unsigned long ms = elapsedUs(d.started, std::chrono::steady_clock::now()) / 1000;
	std::fprintf(d.log, "[openal-diag %lu.%03lu] ", ms / 1000, ms % 1000);
	va_list args;
	va_start(args, format);
	std::vfprintf(d.log, format, args);
	va_end(args);
	std::fputc('\n', d.log);
	std::fflush(d.log);
}

void diagnosticsReport(const char* when)
{
	Diagnostics& d = diagnostics();
	if (!d.enabled) {
		return;
	}
	diagnosticsLog("counters %s"
		" stream_service_calls=%lu stream_buffers_requeued=%lu stream_queue_emptied=%lu"
		" stream_stopped_with_data=%lu stream_queued_min=%lu stream_service_gap_max_us=%lu"
		" stream_starts=%lu"
		" sample_starts=%lu sample_restarts_while_playing=%lu"
		" object_starts=%lu object_restarts_while_playing=%lu"
		" buffer_data_calls=%lu buffer_data_mismatches=%lu"
		" gain_writes=%lu position_writes=%lu"
		" service_passes=%lu service_hold_max_us=%lu service_wait_max_us=%lu"
		" api_hold_max_us=%lu api_wait_max_us=%lu al_errors=%lu",
		when,
		d.streamServiceCalls.load(), d.streamBuffersRequeued.load(), d.streamQueueEmptied.load(),
		d.streamStoppedWithData.load(), d.streamQueuedMin.load(), d.streamServiceGapMaxUs.load(),
		d.streamStarts.load(),
		d.sampleStarts.load(), d.sampleRestartsWhilePlaying.load(),
		d.objectStarts.load(), d.objectRestartsWhilePlaying.load(),
		d.bufferDataCalls.load(), d.bufferDataMismatches.load(),
		d.gainWrites.load(), d.positionWrites.load(),
		d.servicePasses.load(), d.serviceHoldMaxUs.load(), d.serviceWaitMaxUs.load(),
		d.apiHoldMaxUs.load(), d.apiWaitMaxUs.load(), d.alErrors.load());
}

void diagnosticsMax(std::atomic<unsigned long>& slot, unsigned long value)
{
	unsigned long seen = slot.load();
	while (value > seen && !slot.compare_exchange_weak(seen, value)) {
	}
}

void diagnosticsBufferData(ALenum format, unsigned int channels, unsigned int bits,
	unsigned int rate, unsigned int bytes, const char* who)
{
	Diagnostics& d = diagnostics();
	if (!d.enabled) {
		return;
	}
	d.bufferDataCalls.fetch_add(1);
	const unsigned int frameBytes = channels * (bits / 8);
	const bool formatAgrees = (format == alFormatFor(channels, bits))
		&& (channels == 1 || channels == 2) && (bits == 8 || bits == 16);
	const bool wholeFrames = frameBytes != 0 && (bytes % frameBytes) == 0;
	if (!formatAgrees || !wholeFrames || rate == 0) {
		d.bufferDataMismatches.fetch_add(1);
		diagnosticsLog("alBufferData mismatch (%s): format=0x%x channels=%u bits=%u rate=%u bytes=%u",
			who, (unsigned int)format, channels, bits, rate, bytes);
	}
}

void diagnosticsCheckAlError(const char* where)
{
	Diagnostics& d = diagnostics();
	if (!d.enabled) {
		return;
	}
	for (ALenum error = alGetError(); error != AL_NO_ERROR; error = alGetError()) {
		d.alErrors.fetch_add(1);
		diagnosticsLog("alGetError 0x%x at %s", (unsigned int)error, where);
	}
}

LibraryGuard::LibraryGuard()
{
	Library& l = lib();
	Diagnostics& d = diagnostics();
	m_timed = d.enabled;
	m_service = false;
	if (!m_timed) {
		l.lock.lock();
		return;
	}
	m_service = std::this_thread::get_id() == l.serviceThread;
	const std::chrono::steady_clock::time_point asked = std::chrono::steady_clock::now();
	l.lock.lock();
	m_acquired = std::chrono::steady_clock::now();
	diagnosticsMax(m_service ? d.serviceWaitMaxUs : d.apiWaitMaxUs, elapsedUs(asked, m_acquired));
}

LibraryGuard::~LibraryGuard()
{
	Library& l = lib();
	if (m_timed) {
		Diagnostics& d = diagnostics();
		diagnosticsMax(m_service ? d.serviceHoldMaxUs : d.apiHoldMaxUs,
			elapsedUs(m_acquired, std::chrono::steady_clock::now()));
	}
	l.lock.unlock();
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
	if (diagnostics().enabled) {
		diagnostics().gainWrites.fetch_add(1);
		diagnostics().positionWrites.fetch_add(1);
	}
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
	Diagnostics& d = diagnostics();
	while (!l.serviceQuit.load()) {
		// Poll source state and mark voices that ran out. Nothing is called back from here: the
		// engine's completion handlers (MilesAudioManager::notifyOfAudioCompletion and what it
		// reaches) read and rewrite PlayingAudio/AudioEventRTS state with no lock against the main
		// thread, exactly as they did against retail Miles, whose EOS callbacks the engine only ever
		// observed between its own AIL_* calls. deliverCompletions() keeps that contract by firing
		// the callbacks on the API thread as its AIL_* calls return.
		{
			LibraryGuard guard;
			if (d.enabled) {
				d.servicePasses.fetch_add(1);
			}

			for (SampleVoice* sample : l.samples) {
				if (!sample->started || sample->paused || sample->source == 0) {
					continue;
				}
				ALint state = 0;
				alGetSourcei(sample->source, AL_SOURCE_STATE, &state);
				if (state == AL_STOPPED) {
					sample->started = false;
					sample->completionPending = true;
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
					object->voice.completionPending = true;
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
					stream->completionPending = true;
				}
			}

			if (d.enabled) {
				diagnosticsCheckAlError("service pass");
				const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
				if (elapsedUs(d.lastReport, now) >= DIAG_REPORT_SECONDS * 1000000ul) {
					d.lastReport = now;
					diagnosticsReport("periodic");
				}
			}
		}

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
	l.serviceThread = l.service.get_id();
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

void deliverCompletions()
{
	Library& l = lib();
	std::vector<std::pair<AIL_sample_callback, HSAMPLE>> sampleDone;
	std::vector<std::pair<AIL_3dsample_callback, H3DPOBJECT>> objectDone;
	std::vector<std::pair<AIL_stream_callback, HSTREAM>> streamDone;

	{
		LibraryGuard guard;

		for (SampleVoice* sample : l.samples) {
			if (!sample->completionPending) {
				continue;
			}
			sample->completionPending = false;
			if (sample->endOfSample != nullptr && !sample->started) {
				sampleDone.emplace_back(sample->endOfSample, (HSAMPLE)sample);
			}
		}

		for (Object3D* object : l.objects) {
			if (object->isListener || !object->voice.completionPending) {
				continue;
			}
			object->voice.completionPending = false;
			if (object->endOfSample != nullptr && !object->voice.started) {
				objectDone.emplace_back(object->endOfSample, (H3DPOBJECT)object);
			}
		}

		for (StreamVoice* stream : l.streams) {
			if (!stream->completionPending) {
				continue;
			}
			stream->completionPending = false;
			if (stream->endOfStream != nullptr && !stream->playing) {
				streamDone.emplace_back(stream->endOfStream, (HSTREAM)stream);
			}
		}
	}

	// Without the library lock: the handlers call straight back into AIL_* (startNextLoop ->
	// AIL_start_3D_sample), and a handler may release the very handle it was told about.
	for (auto& done : sampleDone) done.first(done.second);
	for (auto& done : objectDone) done.first(done.second);
	for (auto& done : streamDone) done.first(done.second);
}

ApiCall::ApiCall()
{
	Library& l = lib();
	m_onApiThread = l.apiThread == std::this_thread::get_id();
	if (m_onApiThread) {
		++l.apiDepth;
	}
}

ApiCall::~ApiCall()
{
	if (!m_onApiThread) {
		return;
	}
	Library& l = lib();
	// Drain while still counted as inside the outermost call, so the AIL_* calls a handler makes
	// (nesting to depth 2) return without draining again.
	if (l.apiDepth == 1) {
		deliverCompletions();
	}
	--l.apiDepth;
}

} // namespace OpenALAudio

using namespace OpenALAudio;

// ------------------------------------------------------------------------- library lifecycle

int AIL_startup(void)
{
	Library& l = lib();
	LibraryGuard guard;

	if (l.started) {
		return AIL_NO_ERROR;
	}
	l.apiThread = std::this_thread::get_id();
	diagnosticsInit();

	l.device = alcOpenDevice(nullptr);
	if (l.device == nullptr) {
		setLastError("alcOpenDevice failed: no OpenAL output device");
		return -1;
	}

	// Miles mixed at the rate the engine asked for (AIL_quick_startup: 44,100 Hz) and every retail
	// asset is authored at or below it. Ask the implementation for that mixer rate explicitly so a
	// default derived from the output device (48 kHz on most external DACs) does not add a second
	// resampling stage, and so the effective value can be read back and compared. ALC_REFRESH is
	// deliberately not requested: where OpenAL Soft honours it, it shrinks the device period below
	// the backend's own default and only makes real-time underruns more likely.
	const ALCint attributes[] = { ALC_FREQUENCY, (ALCint)Library::MIXER_RATE, 0 };
	l.context = alcCreateContext(l.device, attributes);
	l.contextAttributesHonoured = (l.context != nullptr);
	if (l.context == nullptr) {
		// Apple's OpenAL.framework and older implementations may reject attribute lists they do not
		// understand; the attribute-less context is what every earlier build ran on.
		l.context = alcCreateContext(l.device, nullptr);
	}
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

	alcGetIntegerv(l.device, ALC_FREQUENCY, 1, &l.contextFrequency);
	alcGetIntegerv(l.device, ALC_REFRESH, 1, &l.contextRefresh);
	if (diagnostics().enabled) {
		const char* vendor = alGetString(AL_VENDOR);
		const char* renderer = alGetString(AL_RENDERER);
		const char* version = alGetString(AL_VERSION);
		const char* deviceName = alcGetString(l.device, ALC_DEVICE_SPECIFIER);
		ALCint sync = 0, monoSources = 0, stereoSources = 0;
		alcGetIntegerv(l.device, ALC_SYNC, 1, &sync);
		alcGetIntegerv(l.device, ALC_MONO_SOURCES, 1, &monoSources);
		alcGetIntegerv(l.device, ALC_STEREO_SOURCES, 1, &stereoSources);
		diagnosticsLog("implementation vendor=\"%s\" renderer=\"%s\" version=\"%s\" device=\"%s\"",
			vendor ? vendor : "", renderer ? renderer : "", version ? version : "",
			deviceName ? deviceName : "");
		diagnosticsLog("context requested_frequency=%d attributes_accepted=%d "
			"frequency=%d refresh=%d sync=%d mono_sources=%d stereo_sources=%d",
			(int)Library::MIXER_RATE, l.contextAttributesHonoured ? 1 : 0,
			(int)l.contextFrequency, (int)l.contextRefresh, (int)sync, (int)monoSources,
			(int)stereoSources);
		diagnosticsCheckAlError("AIL_startup");
	}

	l.started = true;
	setLastError(nullptr);
	startServiceThread();
	return AIL_NO_ERROR;
}

void AIL_shutdown(void)
{
	Library& l = lib();

	stopServiceThread();

	LibraryGuard guard;
	if (!l.started) {
		return;
	}
	diagnosticsReport("shutdown");

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
	LibraryGuard guard;

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
	LibraryGuard guard;
	l.fileOpen = opencb;
	l.fileClose = closecb;
	l.fileSeek = seekcb;
	l.fileRead = readcb;
}
