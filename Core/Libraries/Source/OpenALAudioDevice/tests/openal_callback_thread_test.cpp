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

/*
 * Which thread does the OpenAL Miles replacement deliver end-of-sample callbacks on?
 *
 * MilesAudioManager::notifyOfAudioCompletion (reached from every EOS and stream callback the
 * engine registers) rewrites PlayingAudio and AudioEventRTS state
 * with no lock against the main thread: it never calls AIL_lock. It is only safe if the callback
 * runs on the thread that drives the AIL_* API, and only while that thread is inside the library.
 * The Apple Silicon skirmish crash in docs/porting/playability-probe.md 1.3 is what happens when it
 * does not: the shim's service thread called it while the main thread was tearing the same
 * PlayingAudio down.
 *
 * This harness drives the public AIL_* API from one thread, the way the engine does, and records
 * for every callback which thread it arrived on and whether the API thread was inside an AIL_*
 * call at the time. It is run twice by scripts/native-audio-callback-test.py: it must report the
 * defect against the pre-fix shim and its absence against the fixed one, so it is the reproduction
 * as well as the regression test.
 *
 * Output is one JSON object on stdout, the format openal_audio_probe.cpp uses.
 */

#include "mss/mss.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace
{

// ------------------------------------------------------------------------------------- report

std::string g_json;

void emitRaw(const char* key, const std::string& value)
{
	if (!g_json.empty()) {
		g_json += ",\n";
	}
	g_json += "  \"";
	g_json += key;
	g_json += "\": ";
	g_json += value;
}

void emit(const char* key, long value)
{
	emitRaw(key, std::to_string(value));
}

void emit(const char* key, bool value)
{
	emitRaw(key, value ? "true" : "false");
}

void flushJson()
{
	std::printf("{\n%s\n}\n", g_json.c_str());
	std::fflush(stdout);
}

[[noreturn]] void die(const char* what)
{
	std::string quoted = "\"";
	quoted += what;
	quoted += "\"";
	emitRaw("fatal", quoted);
	const char* error = AIL_last_error();
	quoted = "\"";
	quoted += (error != nullptr) ? error : "";
	quoted += "\"";
	emitRaw("fatal_last_error", quoted);
	flushJson();
	std::exit(2);
}

void sleepMs(int ms)
{
	std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// ------------------------------------------------------------------------------- observations

/// What every callback records. Reads on the API thread happen only when the callback cannot be
/// running (after a join-free quiescent sleep or from inside the API thread itself), and the
/// counters are atomics so a service-thread delivery cannot tear them.
struct Observed
{
	std::atomic<long> callbacks{0};
	std::atomic<long> onApiThread{0};	///< delivered on the thread driving AIL_*
	std::atomic<long> insideApiCall{0};	///< delivered while that thread was inside an AIL_* call
	std::atomic<long> afterUnregister{0};	///< delivered after the engine had unregistered it
};

std::thread::id g_apiThread;
std::atomic<bool> g_insideApiCall{false};
std::atomic<bool> g_unregistered{false};

Observed g_sample;
Observed g_object;
Observed g_stream;
Observed g_loop;
std::atomic<long> g_loopRestarts{0};
H3DSAMPLE g_loopHandle = nullptr;

void record(Observed& o)
{
	o.callbacks.fetch_add(1);
	if (std::this_thread::get_id() == g_apiThread) o.onApiThread.fetch_add(1);
	if (g_insideApiCall.load()) o.insideApiCall.fetch_add(1);
	if (g_unregistered.load()) o.afterUnregister.fetch_add(1);
}

void sampleDone(HSAMPLE)
{
	record(g_sample);
}

void objectDone(H3DSAMPLE)
{
	record(g_object);
}

void streamDone(HSTREAM)
{
	record(g_stream);
}

/// MilesAudioManager::startNextLoop's shape: the completion handler restarts the same voice.
void loopDone(H3DSAMPLE handle)
{
	record(g_loop);
	if (g_loopRestarts.load() < 3) {
		g_loopRestarts.fetch_add(1);
		AIL_start_3D_sample(handle);
	}
}

/// Every AIL_* call the "engine" makes goes through this so a callback can tell whether the API
/// thread was inside the library when it ran.
template <typename Fn>
auto apiCall(Fn fn) -> decltype(fn())
{
	g_insideApiCall.store(true);
	struct Reset
	{
		~Reset() { g_insideApiCall.store(false); }
	} reset;
	return fn();
}

// ------------------------------------------------------------------------------------ assets

/// A RIFF/WAVE image of `ms` milliseconds of 22050 Hz 16-bit mono silence: what
/// AIL_set_sample_file and AIL_set_3D_sample_file are handed by the engine.
std::vector<unsigned char> makeWave(int ms)
{
	const unsigned int rate = 22050;
	const unsigned int frames = rate * (unsigned int)ms / 1000;
	const unsigned int dataBytes = frames * 2;
	std::vector<unsigned char> image(44 + dataBytes, 0);
	auto put32 = [&](size_t at, unsigned int v) {
		image[at] = (unsigned char)(v & 0xff);
		image[at + 1] = (unsigned char)((v >> 8) & 0xff);
		image[at + 2] = (unsigned char)((v >> 16) & 0xff);
		image[at + 3] = (unsigned char)((v >> 24) & 0xff);
	};
	auto put16 = [&](size_t at, unsigned int v) {
		image[at] = (unsigned char)(v & 0xff);
		image[at + 1] = (unsigned char)((v >> 8) & 0xff);
	};
	std::memcpy(&image[0], "RIFF", 4);
	put32(4, 36 + dataBytes);
	std::memcpy(&image[8], "WAVE", 4);
	std::memcpy(&image[12], "fmt ", 4);
	put32(16, 16);
	put16(20, 1);
	put16(22, 1);
	put32(24, rate);
	put32(28, rate * 2);
	put16(32, 2);
	put16(34, 16);
	std::memcpy(&image[36], "data", 4);
	put32(40, dataBytes);
	return image;
}

// The stream path reaches its file only through AIL_set_file_callbacks. These serve the one
// in-memory image, the way MilesAudioManager.cpp's serve .big archive members.

std::vector<unsigned char> g_streamImage;
size_t g_streamPos = 0;

unsigned long streamOpen(const char*, void** handle)
{
	g_streamPos = 0;
	*handle = &g_streamImage;
	return 1;
}

void streamClose(void*)
{
}

long streamSeek(void*, long offset, unsigned long type)
{
	long base = 0;
	if (type == AIL_FILE_SEEK_CURRENT) base = (long)g_streamPos;
	if (type == AIL_FILE_SEEK_END) base = (long)g_streamImage.size();
	long target = base + offset;
	if (target < 0) target = 0;
	if (target > (long)g_streamImage.size()) target = (long)g_streamImage.size();
	g_streamPos = (size_t)target;
	return target;
}

unsigned long streamRead(void*, void* dest, unsigned long bytes)
{
	size_t left = g_streamImage.size() - g_streamPos;
	if (bytes > left) bytes = (unsigned long)left;
	std::memcpy(dest, g_streamImage.data() + g_streamPos, bytes);
	g_streamPos += bytes;
	return bytes;
}

// ------------------------------------------------------------------------------------- stages

const int SHORT_MS = 40;	///< voice length; every wait below is far longer
const int IDLE_MS = 400;	///< the API thread out of the library: nothing may arrive
const int SETTLE_MS = 100;	///< the service thread's poll period is 10 ms

/// MilesAudioManager::openDevice()'s AIL_* order, then the 3D provider and listener.
HDIGDRIVER startEngine(H3DPOBJECT* listener)
{
	AIL_set_redist_directory("MSS\\");
	if (AIL_startup() != AIL_NO_ERROR) die("AIL_startup failed");
	AIL_set_preference(AIL_LOCK_PROTECTION, 1);
	if (AIL_quick_startup(1, 0, 44100, 16, 2) == 0) die("AIL_quick_startup failed");
	HDIGDRIVER dig = nullptr;
	AIL_quick_handles(&dig, nullptr, nullptr);
	if (dig == nullptr) die("AIL_quick_handles gave no digital driver");

	HPROENUM next = HPROENUM_FIRST;
	HPROVIDER provider = 0;
	char* name = nullptr;
	if (AIL_enumerate_3D_providers(&next, &provider, &name) == 0) die("no 3D provider");
	if (AIL_open_3D_provider(provider) != M3D_NOERR) die("AIL_open_3D_provider failed");
	*listener = AIL_open_3D_listener(provider);
	if (*listener == nullptr) die("AIL_open_3D_listener returned null");
	AIL_set_file_callbacks(streamOpen, streamClose, streamSeek, streamRead);
	return dig;
}

/// What MilesAudioManager::update() does to the library every frame whether or not anything is
/// playing: setDeviceListenerPosition() -> AIL_set_3D_orientation / AIL_set_3D_position.
void engineFrame(H3DPOBJECT listener)
{
	apiCall([&] { AIL_set_3D_orientation(listener, 0, 1, 0, 0, 0, -1); return 0; });
	apiCall([&] { AIL_set_3D_position(listener, 0, 0, 0); return 0; });
}

}  // namespace

int main()
{
	g_apiThread = std::this_thread::get_id();
	H3DPOBJECT listener = nullptr;
	HDIGDRIVER dig = startEngine(&listener);
	HPROVIDER provider = 0;
	{
		HPROENUM next = HPROENUM_FIRST;
		char* name = nullptr;
		AIL_enumerate_3D_providers(&next, &provider, &name);
	}
	const std::vector<unsigned char> image = makeWave(SHORT_MS);

	// 1. A 2D voice, a 3D voice and a stream are started, and the API thread then stays out of the
	//    library for far longer than they last. Whatever arrives in that window was delivered by
	//    another thread while the engine's state was unguarded: the defect.
	HSAMPLE sample = apiCall([&] { return AIL_allocate_sample_handle(dig); });
	if (sample == nullptr) die("AIL_allocate_sample_handle returned null");
	apiCall([&] { AIL_init_sample(sample); return 0; });
	if (apiCall([&] { return AIL_set_sample_file(sample, image.data(), 0); }) == 0) {
		die("AIL_set_sample_file failed");
	}
	apiCall([&] { AIL_register_EOS_callback(sample, sampleDone); return 0; });

	H3DSAMPLE object = apiCall([&] { return AIL_allocate_3D_sample_handle(provider); });
	if (object == nullptr) die("AIL_allocate_3D_sample_handle returned null");
	if (apiCall([&] { return AIL_set_3D_sample_file(object, image.data()); }) == 0) {
		die("AIL_set_3D_sample_file failed");
	}
	apiCall([&] { AIL_register_3D_EOS_callback(object, objectDone); return 0; });

	g_streamImage = makeWave(SHORT_MS);
	HSTREAM stream = apiCall([&] { return AIL_open_stream(dig, "memory.wav", 0); });
	if (stream == nullptr) die("AIL_open_stream failed");
	apiCall([&] { AIL_set_stream_loop_count(stream, 1); return 0; });
	apiCall([&] { AIL_register_stream_callback(stream, streamDone); return 0; });

	apiCall([&] { AIL_start_sample(sample); return 0; });
	apiCall([&] { AIL_start_3D_sample(object); return 0; });
	apiCall([&] { AIL_start_stream(stream); return 0; });

	sleepMs(IDLE_MS);
	emit("idle_sample_callbacks", g_sample.callbacks.load());
	emit("idle_object_callbacks", g_object.callbacks.load());
	emit("idle_stream_callbacks", g_stream.callbacks.load());

	// 2. The engine's next frame. The completions must arrive now, on this thread, inside the call.
	engineFrame(listener);
	engineFrame(listener);

	emit("sample_callbacks", g_sample.callbacks.load());
	emit("sample_on_api_thread", g_sample.onApiThread.load());
	emit("sample_inside_api_call", g_sample.insideApiCall.load());
	emit("object_callbacks", g_object.callbacks.load());
	emit("object_on_api_thread", g_object.onApiThread.load());
	emit("object_inside_api_call", g_object.insideApiCall.load());
	emit("stream_callbacks", g_stream.callbacks.load());
	emit("stream_on_api_thread", g_stream.onApiThread.load());
	emit("stream_inside_api_call", g_stream.insideApiCall.load());

	// 3. The order MilesAudioManager::releaseMilesHandles runs when the main thread retires a
	//    voice whose source has already run dry but whose completion has not been seen yet:
	//    unregister, stop. After the unregister nothing may be delivered for that voice, whichever
	//    thread noticed the end. Before the fix the service thread had already fired it during the
	//    idle window above, so the stale count is visible here as callbacks that were not asked for.
	apiCall([&] { AIL_start_3D_sample(object); return 0; });
	const long beforeStale = g_object.callbacks.load();
	sleepMs(IDLE_MS);
	g_unregistered.store(true);
	apiCall([&] { AIL_register_3D_EOS_callback(object, nullptr); return 0; });
	apiCall([&] { AIL_stop_3D_sample(object); return 0; });
	engineFrame(listener);
	sleepMs(SETTLE_MS);
	engineFrame(listener);
	g_unregistered.store(false);
	emit("retired_voice_callbacks", g_object.callbacks.load() - beforeStale);
	emit("retired_voice_callbacks_after_unregister", g_object.afterUnregister.load());

	// 4. startNextLoop's shape: the handler restarts the voice from inside the callback. The restart
	//    is a nested AIL_* call on the API thread; each following completion must still arrive, on
	//    the API thread, one per frame at most, and never during the idle window.
	g_loopHandle = apiCall([&] { return AIL_allocate_3D_sample_handle(provider); });
	if (g_loopHandle == nullptr) die("AIL_allocate_3D_sample_handle (loop) returned null");
	if (apiCall([&] { return AIL_set_3D_sample_file(g_loopHandle, image.data()); }) == 0) {
		die("AIL_set_3D_sample_file (loop) failed");
	}
	apiCall([&] { AIL_register_3D_EOS_callback(g_loopHandle, loopDone); return 0; });
	apiCall([&] { AIL_start_3D_sample(g_loopHandle); return 0; });
	long loopIdleCallbacks = 0;
	for (int frame = 0; frame < 8; ++frame) {
		const long before = g_loop.callbacks.load();
		sleepMs(SHORT_MS * 3);
		loopIdleCallbacks += g_loop.callbacks.load() - before;
		engineFrame(listener);
	}
	emit("loop_callbacks", g_loop.callbacks.load());
	emit("loop_restarts", g_loopRestarts.load());
	emit("loop_on_api_thread", g_loop.onApiThread.load());
	emit("loop_inside_api_call", g_loop.insideApiCall.load());
	emit("loop_idle_callbacks", loopIdleCallbacks);

	apiCall([&] { AIL_close_stream(stream); return 0; });
	apiCall([&] { AIL_release_3D_sample_handle(object); return 0; });
	apiCall([&] { AIL_release_3D_sample_handle(g_loopHandle); return 0; });
	apiCall([&] { AIL_release_sample_handle(sample); return 0; });
	apiCall([&] { AIL_close_3D_listener(listener); return 0; });
	AIL_shutdown();

	emit("completed", true);
	flushJson();
	return 0;
}
