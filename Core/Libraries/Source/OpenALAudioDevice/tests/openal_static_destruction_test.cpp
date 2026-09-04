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
 * Does the OpenAL Miles replacement survive process exit without AIL_shutdown?
 *
 * The shim's process-wide state is a function-local static (`lib()`) that owns the service
 * thread. A `std::thread` that is still joinable when its destructor runs is `std::terminate`, so a
 * quit path that reaches static destruction without the engine's AIL_shutdown aborted the process
 * (`terminate called without an active exception`, stack `std::thread::~thread` <-
 * `OpenALAudio::Library::~Library` <- `exit`). The fixed shim joins the thread in an explicit
 * `~Library() noexcept`, keeps its diagnostics in a leaked singleton the thread can read at any
 * point of static destruction, and closes the diagnostics log without throwing.
 *
 * This harness starts the library the way MilesAudioManager::openDevice does, plays a 2D voice, a
 * 3D voice and a looping stream so that the service thread has voices to poll and a queue to
 * refill, then returns from main with everything still playing and AIL_shutdown never called.
 * Run by scripts/native-audio-static-destruction-test.py with diagnostics off, to stderr and to a
 * file; the only assertion is the exit status: 0 after the fix, SIGABRT before it.
 *
 * Output is one JSON object on stdout, the format the other harnesses under tests/ use.
 */

#include "mss/mss.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace
{

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

// ------------------------------------------------------------------------------------ assets

/// A RIFF/WAVE image of `ms` milliseconds of 22050 Hz 16-bit mono silence.
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

/// Long enough that every voice is still playing when main returns.
const int VOICE_MS = 5000;
/// Long enough for the service thread to have polled the voices and refilled the stream queue.
const int RUN_MS = 300;

}  // namespace

int main()
{
	// MilesAudioManager::openDevice()'s AIL_* order, then the 3D provider and listener.
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
	H3DPOBJECT listener = AIL_open_3D_listener(provider);
	if (listener == nullptr) die("AIL_open_3D_listener returned null");
	AIL_set_file_callbacks(streamOpen, streamClose, streamSeek, streamRead);

	const std::vector<unsigned char> image = makeWave(VOICE_MS);

	HSAMPLE sample = AIL_allocate_sample_handle(dig);
	if (sample == nullptr) die("AIL_allocate_sample_handle returned null");
	AIL_init_sample(sample);
	if (AIL_set_sample_file(sample, image.data(), 0) == 0) die("AIL_set_sample_file failed");

	H3DSAMPLE object = AIL_allocate_3D_sample_handle(provider);
	if (object == nullptr) die("AIL_allocate_3D_sample_handle returned null");
	if (AIL_set_3D_sample_file(object, image.data()) == 0) die("AIL_set_3D_sample_file failed");

	g_streamImage = makeWave(VOICE_MS);
	HSTREAM stream = AIL_open_stream(dig, "memory.wav", 0);
	if (stream == nullptr) die("AIL_open_stream failed");
	AIL_set_stream_loop_count(stream, 0);

	AIL_start_sample(sample);
	AIL_start_3D_sample(object);
	AIL_start_stream(stream);

	std::this_thread::sleep_for(std::chrono::milliseconds(RUN_MS));
	AIL_set_3D_position(listener, 0, 0, 0);

	emit("voice_ms", (long)VOICE_MS);
	emit("ran_ms", (long)RUN_MS);
	emit("shutdown_called", false);
	emit("completed", true);
	flushJson();

	// No AIL_shutdown: the service thread is still running and the voices still playing when the
	// process's static destructors run. Returning from main is what a clean engine quit does.
	return 0;
}
