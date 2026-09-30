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
 * Does a 3D voice's playback rate start from the file's own rate every time a file is set?
 *
 * MilesAudioManager::initFilters3D applies an event's pitch shift as
 * AIL_set_3D_sample_playback_rate(s, REAL_TO_INT(AIL_3D_sample_playback_rate(s) * pitchShift)),
 * after AIL_set_3D_sample_file, on every 3D play: every loop of a looping sound (startNextLoop)
 * and every event that reuses a pooled voice. That is one pitch shift only if setting a file
 * resets the rate to the file's; otherwise each play multiplies the previous one's.
 *
 * Driven by scripts/native-audio-3d-rate-test.py, which judges the facts. Output is one JSON
 * object on stdout, the format openal_audio_probe.cpp uses.
 */

#include "OpenALAudioInternal.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
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

void emit(const char* key, double value)
{
	char text[32];
	std::snprintf(text, sizeof(text), "%.6f", value);
	emitRaw(key, text);
}

void emit(const char* key, const std::vector<long>& values)
{
	std::string list = "[";
	for (size_t i = 0; i < values.size(); ++i) {
		list += (i ? ", " : "") + std::to_string(values[i]);
	}
	emitRaw(key, list + "]");
}

void flushJson()
{
	std::printf("{\n%s\n}\n", g_json.c_str());
	std::fflush(stdout);
}

[[noreturn]] void die(const char* what)
{
	emitRaw("fatal", std::string("\"") + what + "\"");
	flushJson();
	std::exit(2);
}

/// A RIFF/WAVE image of 100 ms of 16-bit mono silence at `rate` Hz.
std::vector<unsigned char> makeWave(unsigned int rate)
{
	const unsigned int dataBytes = (rate / 10) * 2;
	std::vector<unsigned char> image(44 + dataBytes, 0);
	auto put32 = [&](size_t at, unsigned int v) {
		for (int i = 0; i < 4; ++i) image[at + i] = (unsigned char)((v >> (8 * i)) & 0xff);
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

/// The AL_PITCH OpenAL is actually mixing the voice at: what the listener hears.
float alPitchOf(H3DSAMPLE sample)
{
	const OpenALAudio::Object3D* object = reinterpret_cast<OpenALAudio::Object3D*>(sample);
	ALfloat pitch = 0.0f;
	alGetSourcef(object->voice.source, AL_PITCH, &pitch);
	return pitch;
}

/// MilesAudioManager::playSample3D's AIL_* order, initFilters3D's pitch line included.
void enginePlay3D(H3DSAMPLE sample, const std::vector<unsigned char>& image, float pitchShift)
{
	if (AIL_set_3D_sample_file(sample, image.data()) == 0) die("AIL_set_3D_sample_file failed");
	AIL_set_3D_sample_distances(sample, 1000.0f, 10.0f);
	AIL_set_3D_position(sample, 5.0f, 0.0f, 0.0f);
	AIL_set_3D_sample_volume(sample, 1.0f);
	AIL_set_3D_sample_playback_rate(sample, (int)(AIL_3D_sample_playback_rate(sample) * pitchShift));
	AIL_start_3D_sample(sample);
}

}  // namespace

int main()
{
	if (AIL_startup() != AIL_NO_ERROR) die("AIL_startup failed");
	if (AIL_quick_startup(1, 0, 44100, 16, 2) == 0) die("AIL_quick_startup failed");
	HPROENUM next = HPROENUM_FIRST;
	HPROVIDER provider = 0;
	char* name = nullptr;
	if (AIL_enumerate_3D_providers(&next, &provider, &name) == 0) die("no 3D provider");
	if (AIL_open_3D_provider(provider) != M3D_NOERR) die("AIL_open_3D_provider failed");
	H3DPOBJECT listener = AIL_open_3D_listener(provider);
	if (listener == nullptr) die("AIL_open_3D_listener returned null");
	H3DSAMPLE sample = AIL_allocate_3D_sample_handle(provider);
	if (sample == nullptr) die("AIL_allocate_3D_sample_handle returned null");

	const std::vector<unsigned char> wave22k = makeWave(22050);
	const std::vector<unsigned char> wave44k = makeWave(44100);
	const float pitchShift = 1.1f;

	// (a) A rate set on one file does not survive setting a file again.
	if (AIL_set_3D_sample_file(sample, wave22k.data()) == 0) die("AIL_set_3D_sample_file failed");
	emit("refile_rate_before", (long)AIL_3D_sample_playback_rate(sample));
	AIL_set_3D_sample_playback_rate(sample, (int)(AIL_3D_sample_playback_rate(sample) * pitchShift));
	emit("refile_rate_shifted", (long)AIL_3D_sample_playback_rate(sample));
	AIL_set_3D_sample_file(sample, wave22k.data());
	emit("refile_rate_after", (long)AIL_3D_sample_playback_rate(sample));
	emit("refile_al_pitch_after", (double)alPitchOf(sample));

	// (b) The engine's per-loop sequence ten times on one voice: every loop at the same rate.
	std::vector<long> loopRates;
	for (int loop = 0; loop < 10; ++loop) {
		enginePlay3D(sample, wave22k, pitchShift);
		loopRates.push_back(AIL_3D_sample_playback_rate(sample));
	}
	AIL_end_3D_sample(sample);
	emit("loop_rates", loopRates);
	emit("loop_expected_rate", (long)(int)(22050 * pitchShift));
	emit("loop_al_pitch_last", (double)alPitchOf(sample));

	// (c) A pooled voice moving from a 22050 Hz file to a 44100 Hz one plays the new file at its rate.
	AIL_set_3D_sample_file(sample, wave22k.data());
	AIL_set_3D_sample_playback_rate(sample, (int)(AIL_3D_sample_playback_rate(sample) * pitchShift));
	AIL_set_3D_sample_file(sample, wave44k.data());
	emit("pooled_rate_after", (long)AIL_3D_sample_playback_rate(sample));
	emit("pooled_al_pitch_after", (double)alPitchOf(sample));

	AIL_release_3D_sample_handle(sample);
	AIL_close_3D_listener(listener);
	AIL_shutdown();
	emitRaw("completed", "true");
	flushJson();
	return 0;
}
