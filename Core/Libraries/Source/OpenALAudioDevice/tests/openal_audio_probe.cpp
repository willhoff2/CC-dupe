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
 * @brief Behaviour probe for the OpenAL Miles replacement, driven only through the AIL_* API.
 *
 * Every earlier audio number in docs/porting/ was a compile or a link count. This program is an
 * observer: it drives the same AIL_* sequences MilesAudioManager.cpp and WWAudio drive, and prints
 * what the layer answered. It asserts nothing about audio the layer was never given, it does not
 * substitute for a missing decoder, and it fails loudly rather than reporting silence as success.
 *
 * It is deliberately built out of the *public* Miles surface (mss/mss.h) plus a couple of ALC
 * queries for the device name, because that is exactly what the engine can see. The one exception is
 * the `stream-drain` stage, which reaches for OpenALAudioInternal.h and says so: draining a
 * four-minute music track through the public API alone would take four minutes of real time per
 * track, and there are 56 of them, so that stage opens the stream through AIL_open_stream like
 * everything else and then pulls the same decode the service thread pulls, at full speed.
 *
 * Driven by scripts/native-audio-probe.py, which supplies the assets and captures the mixer output
 * through OpenAL Soft's wave writer. Each invocation runs one stage and prints one JSON object, so
 * that stages needing their own capture file get their own process.
 */

#include "mss/mss.h"

#include "OpenALAudioInternal.h"

#include <AL/al.h>
#include <AL/alc.h>

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

// ------------------------------------------------------------------------------ JSON emission

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

std::string quote(const char* text)
{
	std::string out = "\"";
	for (const char* p = (text != nullptr) ? text : ""; *p != '\0'; ++p) {
		if (*p == '"' || *p == '\\') {
			out += '\\';
			out += *p;
		} else if (*p == '\n') {
			out += "\\n";
		} else if ((unsigned char)*p < 0x20) {
			char buf[8];
			std::snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)*p);
			out += buf;
		} else {
			out += *p;
		}
	}
	out += '"';
	return out;
}

void emit(const char* key, long value)
{
	emitRaw(key, std::to_string(value));
}

void emit(const char* key, double value)
{
	char buf[64];
	std::snprintf(buf, sizeof(buf), "%.6f", value);
	emitRaw(key, buf);
}

void emit(const char* key, bool value)
{
	emitRaw(key, value ? "true" : "false");
}

void emit(const char* key, const char* value)
{
	emitRaw(key, (value != nullptr) ? quote(value) : std::string("null"));
}

void emitPointer(const char* key, const void* value)
{
	emitRaw(key, value != nullptr ? "true" : "false");
}

/// Records AIL_last_error() under a key, as null when the layer reported nothing.
void emitLastError(const char* key)
{
	emit(key, AIL_last_error());
}

void flushJson()
{
	std::printf("{\n%s\n}\n", g_json.c_str());
	std::fflush(stdout);
}

[[noreturn]] void die(const char* what)
{
	emit("fatal", what);
	emitLastError("fatal_last_error");
	flushJson();
	std::exit(2);
}

// ------------------------------------------------------------------------------- file helpers

std::vector<unsigned char> readFile(const char* path)
{
	std::vector<unsigned char> data;
	std::FILE* f = std::fopen(path, "rb");
	if (f == nullptr) {
		return data;
	}
	std::fseek(f, 0, SEEK_END);
	const long size = std::ftell(f);
	std::fseek(f, 0, SEEK_SET);
	if (size > 0) {
		data.resize((size_t)size);
		if (std::fread(data.data(), 1, data.size(), f) != data.size()) {
			data.clear();
		}
	}
	std::fclose(f);
	return data;
}

// The stream path only ever reaches files through the callbacks the engine installs, because music
// lives in .big archives. These are the same shape as MilesAudioManager.cpp's, over plain files.

unsigned long streamOpen(const char* name, void** handle)
{
	std::FILE* f = std::fopen(name, "rb");
	if (f == nullptr) {
		return 0;
	}
	*handle = f;
	return 1;
}

void streamClose(void* handle)
{
	std::fclose((std::FILE*)handle);
}

long streamSeek(void* handle, long offset, unsigned long type)
{
	int whence = SEEK_SET;
	if (type == AIL_FILE_SEEK_CURRENT) whence = SEEK_CUR;
	if (type == AIL_FILE_SEEK_END) whence = SEEK_END;
	std::fseek((std::FILE*)handle, offset, whence);
	return std::ftell((std::FILE*)handle);
}

unsigned long streamRead(void* handle, void* dest, unsigned long bytes)
{
	return (unsigned long)std::fread(dest, 1, bytes, (std::FILE*)handle);
}

// ------------------------------------------------------------------------------ shared set-up

/// The AIL_* sequence MilesAudioManager::openDevice() runs, in its order.
HDIGDRIVER engineStartup()
{
	AIL_set_redist_directory("MSS\\");
	if (AIL_startup() != AIL_NO_ERROR) {
		die("AIL_startup failed");
	}
	AIL_set_preference(AIL_LOCK_PROTECTION, 1);
	if (AIL_quick_startup(1, 0, 44100, 16, 2) == 0) {
		die("AIL_quick_startup failed");
	}
	HDIGDRIVER dig = nullptr;
	AIL_quick_handles(&dig, nullptr, nullptr);
	if (dig == nullptr) {
		die("AIL_quick_handles gave no digital driver");
	}
	return dig;
}

void emitDeviceFacts()
{
	ALCcontext* context = alcGetCurrentContext();
	ALCdevice* device = (context != nullptr) ? alcGetContextsDevice(context) : nullptr;
	emit("alc_context_current", context != nullptr);
	emit("alc_device_name", (device != nullptr) ? alcGetString(device, ALC_DEVICE_SPECIFIER) : nullptr);
	emit("al_version", alGetString(AL_VERSION));
	emit("al_renderer", alGetString(AL_RENDERER));
	char version[128] = {};
	AIL_MSS_version(version, sizeof(version));
	emit("ail_mss_version", version);
	if (device != nullptr) {
		ALCint rate = 0;
		ALCint mono = 0;
		ALCint stereo = 0;
		alcGetIntegerv(device, ALC_FREQUENCY, 1, &rate);
		alcGetIntegerv(device, ALC_MONO_SOURCES, 1, &mono);
		alcGetIntegerv(device, ALC_STEREO_SOURCES, 1, &stereo);
		emit("alc_frequency", (long)rate);
		emit("alc_mono_sources", (long)mono);
		emit("alc_stereo_sources", (long)stereo);
	}
}

void emitWavInfo(const char* prefix, const std::vector<unsigned char>& image)
{
	AILSOUNDINFO info;
	std::memset(&info, 0, sizeof(info));
	const int ok = AIL_WAV_info(image.data(), &info);
	emit((std::string(prefix) + "_ok").c_str(), ok != 0);
	if (ok == 0) {
		return;
	}
	emit((std::string(prefix) + "_format").c_str(), (long)info.format);
	emit((std::string(prefix) + "_channels").c_str(), (long)info.channels);
	emit((std::string(prefix) + "_bits").c_str(), (long)info.bits);
	emit((std::string(prefix) + "_rate").c_str(), (long)info.rate);
	emit((std::string(prefix) + "_samples").c_str(), (long)info.samples);
	emit((std::string(prefix) + "_data_len").c_str(), (long)info.data_len);
	emit((std::string(prefix) + "_block_size").c_str(), (long)info.block_size);
}

void sleepMs(long ms)
{
	std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// Completion is only observable through the end-of-sample callbacks: mss.h declares no
// AIL_sample_status, because MilesAudioManager.cpp detects the end of a voice entirely through
// AIL_register_EOS_callback and its stream equivalent. The probe therefore observes what the
// engine observes.
std::atomic<int> g_sampleEos{0};
std::atomic<int> g_streamEos{0};
std::atomic<int> g_object3dEos{0};

void sampleFinished(HSAMPLE) { ++g_sampleEos; }
void streamFinished(HSTREAM) { ++g_streamEos; }
void object3dFinished(H3DPOBJECT) { ++g_object3dEos; }

// ------------------------------------------------------------------------------------- stages

/// 1. Does the audio subsystem initialise off Windows: device, driver, listener, voices?
int stageInit()
{
	HDIGDRIVER dig = engineStartup();
	emitDeviceFacts();
	emit("dig_driver_opened", dig != nullptr);
	emit("dig_emulated_ds", (long)((DIG_DRIVER*)dig)->emulated_ds);
	emitLastError("after_startup_last_error");

	// WWAudio opens its own driver through AIL_waveOutOpen with a PCMWAVEFORMAT, rather than
	// through AIL_quick_startup. Both paths have to yield a driver.
	PCMWAVEFORMAT format;
	std::memset(&format, 0, sizeof(format));
	format.wf.wFormatTag = WAVE_FORMAT_PCM;
	format.wf.nChannels = 2;
	format.wf.nSamplesPerSec = 44100;
	format.wf.wBitsPerSample = 16;
	format.wf.nBlockAlign = 4;
	format.wf.nAvgBytesPerSec = 44100 * 4;
	format.wBitsPerSample = 16;
	HDIGDRIVER waveOut = nullptr;
	const int waveOutResult = AIL_waveOutOpen(&waveOut, nullptr, 0, (LPWAVEFORMAT)&format);
	emit("waveoutopen_result", (long)waveOutResult);
	emit("waveoutopen_driver", waveOut != nullptr);
	emit("waveoutopen_emulated_ds", waveOut != nullptr ? (long)((DIG_DRIVER*)waveOut)->emulated_ds : -1);

	// How many 2D voices can actually be allocated? Miles had a fixed voice count; here it is
	// however many OpenAL sources the device grants.
	const int wanted = 512;
	std::vector<HSAMPLE> samples;
	for (int i = 0; i < wanted; ++i) {
		HSAMPLE sample = AIL_allocate_sample_handle(dig);
		if (sample == nullptr) {
			break;
		}
		samples.push_back(sample);
	}
	emit("sample_handles_requested", (long)wanted);
	emit("sample_handles_granted", (long)samples.size());
	emitLastError("sample_handle_exhaustion_error");
	for (HSAMPLE sample : samples) {
		AIL_release_sample_handle(sample);
	}

	// The 3D provider and the listener, as MilesAudioManager::openDevice() enumerates them.
	HPROENUM next = HPROENUM_FIRST;
	HPROVIDER provider = nullptr;
	char* name = nullptr;
	int providers = 0;
	std::string names;
	while (AIL_enumerate_3D_providers(&next, &provider, &name) != 0) {
		++providers;
		if (!names.empty()) names += ",";
		names += (name != nullptr) ? name : "(unnamed)";
	}
	emit("providers_enumerated", (long)providers);
	emit("provider_names", names.c_str());

	next = HPROENUM_FIRST;
	provider = nullptr;
	AIL_enumerate_3D_providers(&next, &provider, &name);
	const M3DRESULT opened = AIL_open_3D_provider(provider);
	emit("open_3d_provider_result", (long)opened);
	H3DPOBJECT listener = AIL_open_3D_listener(provider);
	emit("listener_opened", listener != nullptr);
	if (listener != nullptr) {
		AIL_set_3D_position(listener, 0.0f, 0.0f, 0.0f);
		AIL_set_3D_orientation(listener, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, -1.0f);
		AIL_set_3D_velocity_vector(listener, 0.0f, 0.0f, 0.0f);
		ALfloat pos[3] = { -1.0f, -1.0f, -1.0f };
		ALfloat orient[6] = {};
		alGetListenerfv(AL_POSITION, pos);
		alGetListenerfv(AL_ORIENTATION, orient);
		emit("listener_al_position_x", pos[0]);
		emit("listener_al_position_y", pos[1]);
		emit("listener_al_position_z", pos[2]);
		emit("listener_al_at_z", orient[2]);
		emit("listener_al_up_y", orient[4]);
		emit("listener_al_error", (long)alGetError());
	}

	int sample3d = 0;
	std::vector<H3DSAMPLE> samples3d;
	for (int i = 0; i < 128; ++i) {
		H3DSAMPLE handle = AIL_allocate_3D_sample_handle(provider);
		if (handle == nullptr) {
			break;
		}
		++sample3d;
		samples3d.push_back(handle);
	}
	emit("sample_3d_handles_granted", (long)sample3d);
	for (H3DSAMPLE handle : samples3d) {
		AIL_release_3D_sample_handle(handle);
	}

	AIL_set_3D_speaker_type(provider, AIL_3D_SURROUND);
	emit("speaker_type_requested", (long)AIL_3D_SURROUND);

	// The Bink handoff: Miles was asked for the raw DirectSound object.
	AILLPDIRECTSOUND ds = (AILLPDIRECTSOUND)-1;
	AILLPDIRECTSOUNDBUFFER dsb = (AILLPDIRECTSOUNDBUFFER)-1;
	AIL_get_DirectSound_info(nullptr, &ds, &dsb);
	emitPointer("directsound_object", ds);
	emitPointer("directsound_buffer", dsb);

	AIL_close_3D_listener(listener);
	AIL_shutdown();
	emit("shutdown_returned", true);
	return 0;
}

/// 2/3. Load one file image as a 2D sample and play it to completion. The capture file is the
/// assertion for "did anything play"; this stage reports what the layer said about the asset.
int stageSample(const char* path, float pan)
{
	std::vector<unsigned char> image = readFile(path);
	emit("asset", path);
	emit("asset_bytes", (long)image.size());
	if (image.empty()) {
		die("asset could not be read");
	}

	HDIGDRIVER dig = engineStartup();
	emitDeviceFacts();
	emitWavInfo("wav", image);

	HSAMPLE sample = AIL_allocate_sample_handle(dig);
	if (sample == nullptr) {
		die("AIL_allocate_sample_handle returned null");
	}
	AIL_init_sample(sample);
	const int setResult = AIL_set_sample_file(sample, image.data(), 0);
	emit("set_sample_file_result", (long)setResult);
	emitLastError("set_sample_file_last_error");

	if (setResult != 0) {
		long total = -1;
		long current = -1;
		AIL_sample_ms_position(sample, &total, &current);
		emit("sample_length_ms", total);
		emit("sample_playback_rate", (long)AIL_sample_playback_rate(sample));
		AIL_set_sample_volume_pan(sample, 1.0f, pan);
		emit("pan_requested", pan);
		AIL_set_sample_loop_count(sample, 1);
		AIL_register_EOS_callback(sample, sampleFinished);
		AIL_start_sample(sample);

		// Wait for the end-of-sample callback, with a bound; report the position the layer reports.
		long waited = 0;
		long highWater = -1;
		while (waited < 4000) {
			sleepMs(50);
			waited += 50;
			AIL_sample_ms_position(sample, &total, &current);
			if (current > highWater) {
				highWater = current;
			}
			if (g_sampleEos.load() > 0) {
				break;
			}
		}
		emit("waited_ms", waited);
		emit("high_water_position_ms", highWater);
		emit("eos_callbacks", (long)g_sampleEos.load());
		emit("position_advanced", highWater > 0);
	}

	AIL_release_sample_handle(sample);
	AIL_shutdown();
	return 0;
}

/// 2. IMA ADPCM through the public decompressor, compared against an independently decoded
/// reference (ffmpeg's own decode of the same file) by the driving script.
int stageAdpcm(const char* path, const char* outRaw)
{
	std::vector<unsigned char> image = readFile(path);
	emit("asset", path);
	emit("asset_bytes", (long)image.size());
	if (image.empty()) {
		die("asset could not be read");
	}

	AILSOUNDINFO info;
	std::memset(&info, 0, sizeof(info));
	const int infoOk = AIL_WAV_info(image.data(), &info);
	emitWavInfo("wav", image);
	if (infoOk == 0) {
		die("AIL_WAV_info rejected the asset");
	}

	void* pcm = nullptr;
	unsigned long bytes = 0;
	const int decoded = AIL_decompress_ADPCM(&info, &pcm, &bytes);
	emit("decompress_adpcm_result", (long)decoded);
	emit("decompressed_bytes", (long)bytes);
	if (decoded != 0 && pcm != nullptr) {
		std::FILE* f = std::fopen(outRaw, "wb");
		if (f == nullptr) {
			die("could not write the decoded PCM");
		}
		std::fwrite(pcm, 1, bytes, f);
		std::fclose(f);
		emit("decoded_pcm_path", outRaw);
		AIL_mem_free_lock(pcm);
	}
	return 0;
}

/// 2/3. AudioFileCache::openFile() followed by MilesAudioManager::playSample(), verbatim: an
/// ADPCM asset is decompressed with AIL_decompress_ADPCM and *the decompressed buffer* is what
/// AIL_set_sample_file is given. On Windows that buffer was itself a WAV image. Whether it is one
/// here decides whether any retail ADPCM sound is audible, and the engine never checks the return
/// value, so the only way to see the answer is to look.
int stageEngineAdpcm(const char* path)
{
	std::vector<unsigned char> image = readFile(path);
	emit("asset", path);
	emit("asset_bytes", (long)image.size());
	if (image.empty()) {
		die("asset could not be read");
	}

	HDIGDRIVER dig = engineStartup();
	emitDeviceFacts();

	AILSOUNDINFO info;
	std::memset(&info, 0, sizeof(info));
	AIL_WAV_info(image.data(), &info);
	emit("wav_format", (long)info.format);
	if (info.format != WAVE_FORMAT_IMA_ADPCM) {
		die("asset is not IMA ADPCM; this stage models the engine's ADPCM branch");
	}

	void* decompressed = nullptr;
	unsigned long decompressedBytes = 0;
	const int decompressed_ok = AIL_decompress_ADPCM(&info, &decompressed, &decompressedBytes);
	emit("decompress_adpcm_result", (long)decompressed_ok);
	emit("decompressed_bytes", (long)decompressedBytes);
	if (decompressed_ok == 0 || decompressed == nullptr) {
		die("AIL_decompress_ADPCM failed");
	}

	// Is the decompressed buffer a WAV image, as Miles' was? Reported as a fact, from the bytes.
	const unsigned char* head = (const unsigned char*)decompressed;
	const bool looksLikeRiff = decompressedBytes >= 12 && std::memcmp(head, "RIFF", 4) == 0
		&& std::memcmp(head + 8, "WAVE", 4) == 0;
	emit("decompressed_is_riff_wave", looksLikeRiff);

	AILSOUNDINFO roundTrip;
	std::memset(&roundTrip, 0, sizeof(roundTrip));
	emit("decompressed_wav_info_ok", AIL_WAV_info(decompressed, &roundTrip) != 0);

	HSAMPLE sample = AIL_allocate_sample_handle(dig);
	if (sample == nullptr) {
		die("AIL_allocate_sample_handle returned null");
	}
	AIL_init_sample(sample);
	AIL_register_EOS_callback(sample, sampleFinished);

	// The engine ignores this return value.
	const int setResult = AIL_set_sample_file(sample, decompressed, 0);
	emit("set_sample_file_result", (long)setResult);
	emitLastError("set_sample_file_last_error");

	long total = -1;
	long current = -1;
	AIL_sample_ms_position(sample, &total, &current);
	emit("sample_length_ms", total);

	AIL_set_sample_volume_pan(sample, 1.0f, 0.5f);
	AIL_set_sample_loop_count(sample, 1);
	AIL_start_sample(sample);

	long waited = 0;
	while (waited < 1500 && g_sampleEos.load() == 0) {
		sleepMs(50);
		waited += 50;
	}
	emit("waited_ms", waited);
	emit("eos_callbacks", (long)g_sampleEos.load());

	AIL_release_sample_handle(sample);
	AIL_mem_free_lock(decompressed);
	AIL_shutdown();
	return 0;
}

/// 2/3. The streaming path, which is what music and long dialogue use.
int stageStream(const char* path, bool installCallbacks)
{
	HDIGDRIVER dig = engineStartup();
	emitDeviceFacts();
	emit("asset", path);
	emit("file_callbacks_installed", installCallbacks);

	if (installCallbacks) {
		AIL_set_file_callbacks(streamOpen, streamClose, streamSeek, streamRead);
	}

	HSTREAM stream = AIL_open_stream(dig, path, 0);
	emit("open_stream_handle", stream != nullptr);
	emitLastError("open_stream_last_error");
	if (stream == nullptr) {
		AIL_shutdown();
		return 0;
	}

	S32 total = -1;
	S32 current = -1;
	AIL_stream_ms_position(stream, &total, &current);
	emit("stream_length_ms", (long)total);
	emit("stream_initial_position_ms", (long)current);
	emit("stream_volume", (long)AIL_stream_volume(stream));
	emit("stream_playback_rate", (long)AIL_stream_playback_rate(stream));

	AIL_set_stream_loop_count(stream, 1);
	AIL_register_stream_callback(stream, streamFinished);
	AIL_start_stream(stream);

	long waited = 0;
	long highWater = -1;
	while (waited < 3000) {
		sleepMs(100);
		waited += 100;
		AIL_stream_ms_position(stream, &total, &current);
		if (current > highWater) {
			highWater = current;
		}
		if (g_streamEos.load() > 0) {
			break;
		}
	}
	emit("waited_ms", waited);
	emit("stream_high_water_position_ms", highWater);
	emit("stream_final_length_ms", (long)total);
	emit("stream_eos_callbacks", (long)g_streamEos.load());
	emit("stream_position_advanced", highWater > 0);
	emitLastError("stream_last_error");

	AIL_close_stream(stream);
	AIL_shutdown();
	return 0;
}

/// 2/3. Decodes a whole stream to PCM as fast as the decoder will go, and writes the PCM out.
///
/// Everything up to the decode is the engine's own path: the file callbacks are installed, the
/// stream is opened with AIL_open_stream, and the length is the number AIL_stream_ms_position
/// reports -- which is what MilesAudioManager::getFileLengthMS returns. Only the pump differs: the
/// service thread queues one chunk per buffer as OpenAL drains them, in real time, and this calls
/// the same decodeStreamChunk in a loop instead. The stream is never started, so the service thread
/// does not touch it and there is no race.
///
/// Draining rather than playing is what makes measuring all 56 retail tracks possible at all: they
/// are 2h 18m of audio, so a realtime capture of the set would take that long. The realtime path is
/// covered by the `stream` stage, whose mix is captured and measured.
int stageStreamDrain(const char* path, const char* outRaw, long seekMs)
{
	HDIGDRIVER dig = engineStartup();
	emit("asset", path);
	AIL_set_file_callbacks(streamOpen, streamClose, streamSeek, streamRead);

	HSTREAM stream = AIL_open_stream(dig, path, 0);
	emit("open_stream_handle", stream != nullptr);
	emitLastError("open_stream_last_error");
	if (stream == nullptr) {
		AIL_shutdown();
		return 0;
	}

	S32 total = -1;
	S32 current = -1;
	AIL_stream_ms_position(stream, &total, &current);
	emit("stream_length_ms", (long)total);
	emit("stream_playback_rate", (long)AIL_stream_playback_rate(stream));

	OpenALAudio::StreamVoice* voice = (OpenALAudio::StreamVoice*)stream;
	emit("stream_channels", (long)voice->channels);
	emit("stream_bits", (long)voice->bits);
	emit("stream_codec_is_mpeg", voice->codec == OpenALAudio::StreamCodec::Mpeg);
	emit("stream_mpeg_frames_indexed", (long)voice->mpegFrames.size());
	emit("stream_total_frames", (long)voice->totalFrames);
	emit("stream_payload_bytes", (long)voice->dataLength);
	emit("stream_payload_offset", (long)voice->dataOffset);

	std::FILE* out = (outRaw != nullptr) ? std::fopen(outRaw, "wb") : nullptr;
	if (outRaw != nullptr && out == nullptr) {
		die("could not open the PCM output path");
	}

	std::vector<unsigned char> chunk;
	unsigned long long bytes = 0;
	long chunks = 0;
	for (;;) {
		chunk.clear();
		const unsigned int produced = OpenALAudio::decodeStreamChunk(*voice, chunk);
		if (produced == 0) {
			break;
		}
		++chunks;
		bytes += produced;
		if (out != nullptr) {
			std::fwrite(chunk.data(), 1, produced, out);
		}
	}
	emit("decoded_chunks", chunks);
	emit("decoded_pcm_bytes", (long)bytes);
	emit("decoded_pcm_path", outRaw);
	// The read cursor having reached the end of the payload is what says the whole stream was
	// decoded rather than abandoned part way: a decode that stops early stops here too.
	emit("drained_to_end", voice->readCursor >= voice->dataLength);
	emit("read_cursor", (long)voice->readCursor);
	emitLastError("drain_last_error");

	if (seekMs > 0) {
		// Seeking is the other thing the index is for. Ask for a position, then decode from there
		// and report what came back, so a seek that lands in the wrong place is visible as PCM.
		AIL_set_stream_ms_position(stream, (S32)seekMs);
		AIL_stream_ms_position(stream, &total, &current);
		emit("seek_requested_ms", seekMs);
		emit("seek_reported_ms", (long)current);
		// The exact PCM frame the seek landed on, which is what the reported milliseconds are
		// computed from. The driving script compares the samples after the seek against the oracle's
		// samples at this offset, and a millisecond is 44 samples of slack too many for that.
		emit("seek_frames_played", (long)voice->framesPlayed);
		chunk.clear();
		const unsigned int produced = OpenALAudio::decodeStreamChunk(*voice, chunk);
		emit("seek_decoded_bytes", (long)produced);
		if (out != nullptr && produced != 0) {
			// Written after the full decode, and reported separately, so the driving script can
			// measure it without mistaking it for part of the stream.
			std::fwrite(chunk.data(), 1, produced, out);
			emit("seek_pcm_offset_bytes", (long)bytes);
		}
		emitLastError("seek_last_error");
	}

	if (out != nullptr) {
		std::fclose(out);
	}
	AIL_close_stream(stream);
	AIL_shutdown();
	return 0;
}

/// 4. A 3D voice at a position, the way MilesAudioManager plays a world sound. The capture is the
/// assertion: a sound to the listener's left must come out louder on the left.
int stage3D(const char* path, float x, float occlusion)
{
	std::vector<unsigned char> image = readFile(path);
	emit("asset", path);
	if (image.empty()) {
		die("asset could not be read");
	}

	engineStartup();
	emitDeviceFacts();

	HPROENUM next = HPROENUM_FIRST;
	HPROVIDER provider = nullptr;
	char* name = nullptr;
	if (AIL_enumerate_3D_providers(&next, &provider, &name) == 0) {
		die("no 3D provider enumerated");
	}
	if (AIL_open_3D_provider(provider) != M3D_NOERR) {
		die("AIL_open_3D_provider failed");
	}
	H3DPOBJECT listener = AIL_open_3D_listener(provider);
	if (listener == nullptr) {
		die("AIL_open_3D_listener returned null");
	}
	// The engine's listener convention, from MilesAudioManager::setListenerPosition().
	AIL_set_3D_position(listener, 0.0f, 0.0f, 0.0f);
	AIL_set_3D_orientation(listener, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, -1.0f);

	H3DSAMPLE sample = AIL_allocate_3D_sample_handle(provider);
	if (sample == nullptr) {
		die("AIL_allocate_3D_sample_handle returned null");
	}
	const int setResult = AIL_set_3D_sample_file(sample, image.data());
	emit("set_3d_sample_file_result", (long)setResult);
	emitLastError("set_3d_sample_file_last_error");
	if (setResult == 0) {
		AIL_shutdown();
		return 0;
	}

	emit("sample_3d_length_ms", (long)AIL_3D_sample_length(sample));
	AIL_set_3D_sample_volume(sample, 1.0f);
	AIL_set_3D_sample_distances(sample, 1000.0f, 1.0f);
	AIL_set_3D_sample_occlusion(sample, occlusion);
	AIL_set_3D_position(sample, x, 0.0f, 0.0f);
	emit("position_x_requested", x);
	emit("occlusion_requested", occlusion);

	AIL_register_3D_EOS_callback(sample, object3dFinished);
	AIL_start_3D_sample(sample);
	long waited = 0;
	while (waited < 4000) {
		sleepMs(50);
		waited += 50;
		if (g_object3dEos.load() > 0) {
			break;
		}
	}
	emit("waited_ms", waited);
	emit("eos_callbacks", (long)g_object3dEos.load());
	emit("final_offset", (long)AIL_3D_sample_offset(sample));

	AIL_release_3D_sample_handle(sample);
	AIL_close_3D_listener(listener);
	AIL_shutdown();
	return 0;
}

} // namespace

int main(int argc, char** argv)
{
	if (argc < 2) {
		std::fprintf(stderr, "usage: %s <stage> [args]\n", argv[0]);
		return 64;
	}

	const std::string stage = argv[1];
	emit("stage", stage.c_str());

	int result = 0;
	if (stage == "init") {
		result = stageInit();
	} else if (stage == "sample" && argc >= 3) {
		result = stageSample(argv[2], (argc >= 4) ? (float)std::atof(argv[3]) : 0.5f);
	} else if (stage == "adpcm" && argc >= 4) {
		result = stageAdpcm(argv[2], argv[3]);
	} else if (stage == "engine-adpcm" && argc >= 3) {
		result = stageEngineAdpcm(argv[2]);
	} else if (stage == "stream" && argc >= 3) {
		result = stageStream(argv[2], (argc < 4) || std::strcmp(argv[3], "no-callbacks") != 0);
	} else if (stage == "stream-drain" && argc >= 3) {
		result = stageStreamDrain(argv[2], (argc >= 4) ? argv[3] : nullptr,
			(argc >= 5) ? std::atol(argv[4]) : 0);
	} else if (stage == "sample3d" && argc >= 4) {
		result = stage3D(argv[2], (float)std::atof(argv[3]),
			(argc >= 5) ? (float)std::atof(argv[4]) : 0.0f);
	} else {
		std::fprintf(stderr, "unknown stage or missing arguments: %s\n", stage.c_str());
		return 64;
	}

	flushJson();
	return result;
}
