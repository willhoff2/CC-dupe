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
 * @brief Internal state shared by the OpenAL implementation of the Miles API.
 */

#pragma once

#include "mss/mss.h"

#include <AL/al.h>
#include <AL/alc.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace OpenALAudio
{

/// Number of user-data slots Miles gives every voice. The engine only ever uses slot 0 for the
/// voice index and WWAudio's INFO_OBJECT_PTR slot, but Miles exposes eight.
constexpr unsigned int USER_DATA_SLOTS = 8;

/// Miles reports integer volumes on a 0..127 scale and float volumes on a 0..1 scale.
constexpr int MILES_MAX_INT_VOLUME = 127;

/// Decoded audio, owned by a voice. Sample rate and channel count are needed for ms<->byte maths.
struct DecodedAudio
{
	ALuint buffer = 0;
	ALenum format = AL_FORMAT_MONO16;
	unsigned int rate = 0;
	unsigned int channels = 0;
	unsigned int bits = 0;
	unsigned int frames = 0;

	unsigned int lengthMs() const { return rate ? (unsigned int)((frames * 1000ull) / rate) : 0; }
};

/// Common state for anything that occupies an OpenAL source.
struct Voice
{
	ALuint source = 0;
	void* userData[USER_DATA_SLOTS] = {};
	float volume = 1.0f;			///< 0..1
	float pan = 0.5f;				///< 0..1, 0.5 == centre
	int loopCount = 1;				///< Miles convention: 0 == loop forever
	int playbackRate = 0;			///< Hz; 0 == "the file's own rate"
	bool started = false;
	bool paused = false;
	DecodedAudio audio;
};

struct SampleVoice : Voice
{
	AIL_sample_callback endOfSample = nullptr;
	HPROVIDER processor[N_SAMPLE_STAGES] = {};
	/// Filter preferences are recorded so queries stay consistent; they are not audible yet.
	std::vector<std::pair<std::string, float>> filterPreferences;
};

/// A 3D voice or the listener. Miles uses H3DPOBJECT for both, and AIL_set_3D_position /
/// AIL_set_3D_orientation are called with either, so the two must share a representation.
struct Object3D
{
	h3DPOBJECT base = {};			///< must stay first: H3DPOBJECT aliases this
	bool isListener = false;
	Voice voice;					///< unused when isListener
	AIL_3dsample_callback endOfSample = nullptr;
	float minDistance = 1.0f;
	float maxDistance = 1000.0f;
	float occlusion = 0.0f;
	float effectsLevel = 0.0f;
	void* userData[USER_DATA_SLOTS] = {};
};

/// What a stream's file holds. Retail dialogue is overwhelmingly IMA ADPCM (2391 of the 2442 files
/// under the streaming folder), so the stream path decodes it as well as PCM.
enum class StreamCodec
{
	Pcm,
	ImaAdpcm,
};

struct StreamVoice
{
	static constexpr unsigned int BUFFER_COUNT = 4;
	static constexpr unsigned int BUFFER_FRAMES = 8192;

	ALuint source = 0;
	ALuint buffers[BUFFER_COUNT] = {};
	AIL_stream_callback endOfStream = nullptr;
	void* userData[USER_DATA_SLOTS] = {};

	float volume = 1.0f;
	float pan = 0.5f;
	int loopCount = 1;
	int playbackRate = 0;
	bool playing = false;
	bool paused = false;
	bool exhausted = false;			///< no more data to queue from the source file

	/// Handle returned by the engine's file-open callback; stream I/O goes through the callbacks
	/// installed via AIL_set_file_callbacks, not through fopen, because music lives in .big
	/// archives that only the engine can read.
	void* file = nullptr;
	std::string fileName;

	/// Format of the PCM that is queued into OpenAL. For an ADPCM file that is the *decoded*
	/// format, 16 bits per sample, not the format in the file.
	ALenum format = AL_FORMAT_STEREO16;
	unsigned int rate = 0;
	unsigned int channels = 0;
	unsigned int bits = 0;
	unsigned int totalFrames = 0;
	unsigned int framesQueued = 0;
	unsigned int framesPlayed = 0;

	StreamCodec codec = StreamCodec::Pcm;
	unsigned int blockSize = 0;			///< ADPCM block alignment, in file bytes
	unsigned int samplesPerBlock = 0;	///< decoded samples per ADPCM block, per channel

	unsigned int dataOffset = 0;	///< byte offset of the payload within the file
	unsigned int dataLength = 0;	///< byte length of the payload, as it is stored in the file
	unsigned int readCursor = 0;	///< byte offset of the next read, relative to dataOffset

	/// Sub-region a looping stream repeats, as byte offsets into the payload. `loopEnd` of 0 means
	/// "to the end of the payload", which is what AIL_set_stream_loop_block(h, 0, -1) asks for.
	unsigned int loopStart = 0;
	unsigned int loopEnd = 0;
};

/// The digital driver. Must begin with DIG_DRIVER because WWAudio.cpp reads emulated_ds.
struct DigitalDriver
{
	DIG_DRIVER base = {};			///< must stay first
	bool open = false;
	unsigned int rate = 44100;
	unsigned int bits = 16;
	unsigned int channels = 2;
};

/// Process-wide state.
struct Library
{
	std::recursive_mutex lock;		///< backs AIL_lock / AIL_unlock

	ALCdevice* device = nullptr;
	ALCcontext* context = nullptr;
	bool started = false;

	DigitalDriver driver;
	Object3D* listener = nullptr;

	AIL_file_open_callback fileOpen = nullptr;
	AIL_file_close_callback fileClose = nullptr;
	AIL_file_seek_callback fileSeek = nullptr;
	AIL_file_read_callback fileRead = nullptr;

	std::string lastError = "";
	std::string redistDirectory;
	int speakerType = AIL_3D_2_SPEAKER;

	std::vector<SampleVoice*> samples;
	std::vector<Object3D*> objects;
	std::vector<StreamVoice*> streams;
	std::vector<HAUDIO> quickAudio;

	/// Miles delivered end-of-sample callbacks from its mixer thread. OpenAL has no such thread,
	/// so one is run here; the engine's callbacks already assume they may run concurrently and
	/// take AIL_lock.
	std::thread service;
	std::atomic<bool> serviceQuit{false};
};

Library& lib();

/// Starts/stops the callback service thread. Idempotent.
void startServiceThread();
void stopServiceThread();

void setLastError(const char* message);

/// Applies volume+pan to a source. OpenAL has no pan control, so a 0..1 Miles pan is expressed as
/// a position on the x axis of a source that is otherwise relative to the listener.
void applyVolumePan(ALuint source, float volume, float pan);

/// Applies a Miles playback rate (Hz) as an AL_PITCH ratio against the file's own rate. Unlike
/// Miles this changes duration as well as pitch.
void applyPlaybackRate(ALuint source, int playbackRate, unsigned int nativeRate);

/// Decodes a WAV file image (PCM or IMA ADPCM) into an OpenAL buffer. Returns false and leaves
/// `out` untouched when the image is not a WAV we can decode.
bool decodeWaveImage(const void* image, unsigned int imageSize, DecodedAudio& out);

/// Parses a WAV header of a fully resident image: the payload must be inside `image`, and
/// `info.data_ptr` points into it. Used by AIL_WAV_info and the sample paths.
bool parseWaveHeader(const void* image, unsigned int imageSize, AILSOUNDINFO& info,
	unsigned int* dataOffset);

/// Parses a WAV header when only the front of the file is available, as when a stream has read a
/// header window: `info.data_len` and `*dataOffset` describe a payload that is *not* in `image`,
/// and `info.data_ptr` is null. Streams then read the payload through the file callbacks.
bool parseWaveMetadata(const void* image, unsigned int imageSize, AILSOUNDINFO& info,
	unsigned int* dataOffset);

/// Decoded samples in one IMA ADPCM block, per channel. Zero when the block size cannot hold one.
unsigned int imaSamplesPerBlock(unsigned int blockSize, unsigned int channels);

/// Decodes whole IMA ADPCM blocks into interleaved 16-bit PCM, returning the sample count written.
/// `out` must have room for blocks * imaSamplesPerBlock() * channels samples.
unsigned long decodeImaAdpcmBlocks(const void* payload, unsigned int blocks, unsigned int blockSize,
	unsigned int channels, int16_t* out);

/// Decodes IMA ADPCM into freshly allocated 16-bit PCM. Frees with std::free.
bool decodeImaAdpcm(const AILSOUNDINFO& info, void** outData, unsigned long* outSize);

/// Wraps PCM in a freshly allocated 44-byte-header RIFF/WAVE image, as Miles' AIL_decompress_ADPCM
/// returned: the engine feeds the result straight back in through AIL_set_sample_file. Returns null
/// on failure; the caller frees with std::free (AIL_mem_free_lock).
void* buildWaveImage(const void* pcm, unsigned long pcmBytes, unsigned int channels,
	unsigned int rate, unsigned int bits, unsigned long* imageBytes);

ALenum alFormatFor(unsigned int channels, unsigned int bits);

/// Converts the third component of a Miles coordinate or vector into OpenAL's frame.
///
/// Miles' 3D frame is left-handed: +X right, +Y up, +Z *away* from the listener. OpenAL's is
/// right-handed with the same X and Y but +Z *towards* the listener, so the two differ by the sign
/// of Z alone and every position, orientation vector and velocity crossing this seam is negated
/// there. Nothing above the seam changes, so the Windows Miles path is untouched.
///
/// The sign matters because the handedness decides which side is "right": OpenAL derives its right
/// vector as at x up, Miles as up x forward. Zero Hour's listener is
/// AIL_set_3D_orientation(listener, facing.x, facing.y, facing.z, 0, 0, -1) with world coordinates
/// passed straight through (X east, Y north, Z up), so with facing = world north the un-negated
/// triples give OpenAL at x up = (0,1,0) x (0,0,-1) = (-1,0,0): a source to the world east came out
/// of the LEFT speaker, the exact inverse of Miles, which computes up x forward =
/// (0,0,-1) x (0,1,0) = (+1,0,0). Negating Z restores east = +right for both.
inline float milesToAlZ(float z)
{
	return -z;
}

/// Refills a playing stream's buffer queue. Sets `finished` when the stream has run out of data
/// and OpenAL has drained the queue. Called from the service thread with the library lock held.
void serviceStream(StreamVoice& stream, bool& finished);

} // namespace OpenALAudio
