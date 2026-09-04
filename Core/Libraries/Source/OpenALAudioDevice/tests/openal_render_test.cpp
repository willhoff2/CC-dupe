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
 * Render one asset through the OpenAL Miles replacement, the way the engine plays it, so the
 * mixer's output can be inspected for discontinuities.
 *
 *   openal_render_test stream <file> [seconds]   AIL_open_stream / AIL_start_stream (music, speech)
 *   openal_render_test sample <file> [seconds]   AIL_set_sample_file / AIL_start_sample (a 2D SFX)
 *   openal_render_test loop   <file> [seconds]   the same 2D voice, restarted from its EOS
 *                                                 callback the way startNextLoop does
 *
 * The caller points OpenAL Soft at the `wave` backend (ALSOFT_CONF) so the rendered PCM lands in a
 * file; scripts/audio-pcm-discontinuity.py then counts the clicks. Between AIL_* calls the harness
 * does what MilesAudioManager::update() does every frame -- listener orientation and position
 * writes -- so the library lock sees the engine's contention pattern.
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

// --------------------------------------------------------------------------------- file access

std::vector<unsigned char> g_streamImage;
size_t g_streamPos = 0;

bool loadFile(const char* path, std::vector<unsigned char>& into)
{
	FILE* f = std::fopen(path, "rb");
	if (f == nullptr) return false;
	std::fseek(f, 0, SEEK_END);
	long size = std::ftell(f);
	std::fseek(f, 0, SEEK_SET);
	if (size < 0) {
		std::fclose(f);
		return false;
	}
	into.resize((size_t)size);
	size_t got = size > 0 ? std::fread(into.data(), 1, (size_t)size, f) : 0;
	std::fclose(f);
	return got == (size_t)size;
}

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

// -------------------------------------------------------------------------------------- engine

std::atomic<long> g_completions{0};
std::atomic<long> g_loopRestarts{0};
long g_loopLimit = 0;

void streamDone(HSTREAM)
{
	g_completions.fetch_add(1);
}

void sampleDone(HSAMPLE)
{
	g_completions.fetch_add(1);
}

/// MilesAudioManager::startNextLoop's shape: the completion handler restarts the same voice.
void loopDone(HSAMPLE handle)
{
	g_completions.fetch_add(1);
	if (g_loopRestarts.load() < g_loopLimit) {
		g_loopRestarts.fetch_add(1);
		AIL_start_sample(handle);
	}
}

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

void engineFrame(H3DPOBJECT listener)
{
	AIL_set_3D_orientation(listener, 0, 1, 0, 0, 0, -1);
	AIL_set_3D_position(listener, 0, 0, 0);
}

}  // namespace

int main(int argc, char** argv)
{
	if (argc < 3) {
		std::fprintf(stderr, "usage: %s stream|sample|loop <file> [seconds]\n", argv[0]);
		return 2;
	}
	const std::string mode = argv[1];
	const char* path = argv[2];
	const double limitSeconds = argc > 3 ? std::atof(argv[3]) : 600.0;

	std::vector<unsigned char> image;
	if (!loadFile(path, image)) die("asset could not be read");

	H3DPOBJECT listener = nullptr;
	HDIGDRIVER dig = startEngine(&listener);

	HSTREAM stream = nullptr;
	HSAMPLE sample = nullptr;
	if (mode == "stream") {
		g_streamImage = image;
		stream = AIL_open_stream(dig, path, 0);
		if (stream == nullptr) die("AIL_open_stream failed");
		AIL_set_stream_loop_count(stream, 1);
		AIL_set_stream_volume_pan(stream, 1.0f, 0.5f);
		AIL_register_stream_callback(stream, streamDone);
		AIL_start_stream(stream);
	} else if (mode == "sample" || mode == "loop") {
		sample = AIL_allocate_sample_handle(dig);
		if (sample == nullptr) die("AIL_allocate_sample_handle returned null");
		AIL_init_sample(sample);
		if (AIL_set_sample_file(sample, image.data(), 0) == 0) die("AIL_set_sample_file failed");
		AIL_set_sample_volume_pan(sample, 1.0f, 0.5f);
		if (mode == "loop") {
			g_loopLimit = 8;
			AIL_register_EOS_callback(sample, loopDone);
		} else {
			AIL_register_EOS_callback(sample, sampleDone);
		}
		AIL_start_sample(sample);
	} else {
		die("mode must be stream, sample or loop");
	}

	const auto start = std::chrono::steady_clock::now();
	const long wanted = (mode == "loop") ? g_loopLimit + 1 : 1;
	long frames = 0;
	for (;;) {
		engineFrame(listener);
		++frames;
		if (g_completions.load() >= wanted) break;
		const double elapsed = std::chrono::duration<double>(
			std::chrono::steady_clock::now() - start).count();
		if (elapsed >= limitSeconds) break;
		std::this_thread::sleep_for(std::chrono::milliseconds(16));
	}
	const double elapsed = std::chrono::duration<double>(
		std::chrono::steady_clock::now() - start).count();
	// Let the mixer drain the tail before the device closes.
	std::this_thread::sleep_for(std::chrono::milliseconds(250));

	if (stream != nullptr) AIL_close_stream(stream);
	if (sample != nullptr) AIL_release_sample_handle(sample);
	AIL_close_3D_listener(listener);
	AIL_shutdown();

	emitRaw("mode", "\"" + mode + "\"");
	emit("asset_bytes", (long)image.size());
	emit("completions", g_completions.load());
	emit("loop_restarts", g_loopRestarts.load());
	emit("engine_frames", frames);
	emit("elapsed_ms", (long)(elapsed * 1000.0));
	emitRaw("completed", g_completions.load() >= wanted ? "true" : "false");
	flushJson();
	return 0;
}
