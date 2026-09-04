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
#include <chrono>
#include <cstdio>
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
	/// Set by the service thread when the source ran out; consumed by deliverCompletions().
	bool completionPending = false;
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
/// under the streaming folder), so the stream path decodes it as well as PCM; all 56 retail music
/// tracks are MPEG audio (layer III), which is the third.
enum class StreamCodec
{
	Pcm,
	ImaAdpcm,
	Mpeg,
};

/// Bytes of an MPEG audio frame header, which is all the parser needs to describe a whole frame.
constexpr unsigned int MPEG_FRAME_HEADER_BYTES = 4;

/// Largest PCM a single MPEG frame can decode to: 1152 samples of stereo.
constexpr unsigned int MPEG_MAX_SAMPLES_PER_FRAME = 1152 * 2;

/// Frames decoded and thrown away before the first frame wanted after a seek or a loop rewind.
///
/// Layer III frames are not independent: the bit reservoir lets a frame's main data live up to 511
/// bytes back in the frames before it, and the synthesis filterbank carries overlap across frames.
/// A decoder started cold on a mid-stream frame therefore produces no samples at all for it --
/// minimp3 returns zero, which this stream reports as a frame that did not decode as its header
/// described -- and only approximate ones for the next. Four frames is past the largest reservoir at
/// every retail bitrate and past the filterbank overlap.
constexpr unsigned int MPEG_PRIME_FRAMES = 4;

/// What an MPEG audio frame header describes. `bytes` includes the header and the padding byte, so
/// consecutive frames are `offset + bytes` apart.
struct MpegFrameHeader
{
	unsigned int bytes = 0;
	unsigned int samples = 0;		///< PCM frames this MPEG frame decodes to, per channel
	unsigned int rate = 0;
	unsigned int channels = 0;
	unsigned int bitrateKbps = 0;
	unsigned int layer = 0;			///< 1, 2 or 3
	unsigned int version = 0;		///< 1, 2, or 25 for MPEG-2.5
};

/// One indexed MPEG frame within a stream's payload.
///
/// An MPEG elementary stream has no seek table -- 0 of the 56 retail tracks carry a Xing/VBRI tag --
/// so the stream indexes every frame when it opens. That index is what makes duration and seeking
/// exact for variable-bitrate files as well as constant-bitrate ones: no byte count is ever divided
/// by a bitrate, which is the maths that goes silently wrong on VBR.
struct MpegFrame
{
	unsigned int offset = 0;		///< byte offset of the frame header within the payload
	unsigned int bytes = 0;			///< frame length in file bytes
	unsigned int samples = 0;		///< PCM frames it decodes to, per channel
};

/// The MPEG decoder's state. Opaque: minimp3 is compiled in OpenALMpeg.cpp alone.
struct MpegDecoder;

struct StreamVoice
{
	~StreamVoice();

	/// Queue depth. Refill only happens on the service thread's 10 ms poll, so the queue is the
	/// whole tolerance for that thread not running: 8 x 8192 frames is 1.49 s at 44.1 kHz, more than
	/// the ~1.2 s scheduling stalls measured during a lavapipe map load (4 buffers, 0.74 s, ran dry).
	static constexpr unsigned int BUFFER_COUNT = 8;
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
	bool completionPending = false;
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

	/// MPEG only: the decoder, and every frame of the payload in file order.
	MpegDecoder* mpeg = nullptr;
	std::vector<MpegFrame> mpegFrames;

	/// Set when the decoder was rewound away from the frame the read cursor points at, so that the
	/// next decode primes it on the frames before that one instead of starting cold.
	bool mpegNeedsPriming = false;

	unsigned int dataOffset = 0;	///< byte offset of the payload within the file
	unsigned int dataLength = 0;	///< byte length of the payload, as it is stored in the file
	unsigned int readCursor = 0;	///< byte offset of the next read, relative to dataOffset

	/// Sub-region a looping stream repeats, as byte offsets into the payload. `loopEnd` of 0 means
	/// "to the end of the payload", which is what AIL_set_stream_loop_block(h, 0, -1) asks for.
	unsigned int loopStart = 0;
	unsigned int loopEnd = 0;

	/// Diagnostics only: when serviceStream last ran for this stream.
	std::chrono::steady_clock::time_point lastServiced;
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
	Library() = default;

	/// Runs at static destruction when the engine did not (or could not) call AIL_shutdown. A
	/// joinable std::thread's destructor is std::terminate, so the service thread is stopped and
	/// joined here, then the device is closed; nothing in the body can throw.
	~Library() noexcept;

	Library(const Library&) = delete;
	Library& operator=(const Library&) = delete;

	std::recursive_mutex lock;		///< backs AIL_lock / AIL_unlock

	ALCdevice* device = nullptr;
	ALCcontext* context = nullptr;
	bool started = false;

	/// The mixer rate the engine asks Miles for (AIL_quick_startup's 44,100), requested from the
	/// implementation at context creation; what it actually gave back is read after creation.
	static constexpr unsigned int MIXER_RATE = 44100;
	bool contextAttributesHonoured = false;
	ALCint contextFrequency = 0;
	ALCint contextRefresh = 0;

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

	/// Polls OpenAL for finished voices and refills streams. It never calls back into the engine:
	/// it only marks a voice completionPending, and the callback is delivered from the API thread.
	std::thread service;
	std::atomic<bool> serviceQuit{false};

	/// The thread that called AIL_startup. End-of-sample callbacks are delivered on this thread
	/// only, when an AIL_* entry point returns and no other entry point is active on it.
	std::thread::id apiThread;
	std::thread::id serviceThread;
	int apiDepth = 0;				///< nesting of AIL_* entry points on apiThread; touched by it alone
};

Library& lib();

/// Opt-in counters and log for the audio path, below the AIL_* surface. Enabled by the environment
/// variable OPENAL_AUDIO_DIAG: "stderr", or a path the log is appended to. Off, nothing here costs
/// more than a load of `enabled`. The counters are what an audible defect has to show up in: a
/// stream that ran dry, a source restarted mid-waveform, a buffer whose declared format does not
/// match its bytes, or a library lock held long enough to starve the other thread.
struct Diagnostics
{
	bool enabled = false;
	std::FILE* log = nullptr;
	std::chrono::steady_clock::time_point started;
	std::chrono::steady_clock::time_point lastReport;

	// Stream queue, counted in serviceStream.
	std::atomic<unsigned long> streamServiceCalls{0};
	std::atomic<unsigned long> streamBuffersRequeued{0};
	std::atomic<unsigned long> streamQueueEmptied{0};		///< every buffer processed: the mixer ran dry
	std::atomic<unsigned long> streamStoppedWithData{0};	///< AL_STOPPED with buffers queued: forced restart
	std::atomic<unsigned long> streamQueuedMin{StreamVoice::BUFFER_COUNT};	///< fewest buffers queued after a refill
	std::atomic<unsigned long> streamServiceGapMaxUs{0};	///< longest gap between two service passes
	std::atomic<unsigned long> streamStarts{0};

	// One-shot voices.
	std::atomic<unsigned long> sampleStarts{0};
	std::atomic<unsigned long> sampleRestartsWhilePlaying{0};	///< AIL_start_sample on an AL_PLAYING source
	std::atomic<unsigned long> objectStarts{0};
	std::atomic<unsigned long> objectRestartsWhilePlaying{0};	///< AIL_start_3D_sample on an AL_PLAYING source

	// Every alBufferData the shim makes, checked against the decoded data.
	std::atomic<unsigned long> bufferDataCalls{0};
	std::atomic<unsigned long> bufferDataMismatches{0};	///< format/channels/bits disagree or a partial frame

	// Parameter writes the mixer sees as steps.
	std::atomic<unsigned long> gainWrites{0};
	std::atomic<unsigned long> positionWrites{0};

	// Library lock, per thread class.
	std::atomic<unsigned long> serviceHoldMaxUs{0};
	std::atomic<unsigned long> serviceWaitMaxUs{0};
	std::atomic<unsigned long> apiHoldMaxUs{0};
	std::atomic<unsigned long> apiWaitMaxUs{0};
	std::atomic<unsigned long> servicePasses{0};
	std::atomic<unsigned long> servicePassGapMaxUs{0};	///< longest time between two service passes
	std::chrono::steady_clock::time_point lastPassEnd;

	// Fault injection, OPENAL_AUDIO_DIAG_STALL="<at_ms>:<for_ms>": once, the first time the service
	// thread wakes at or past at_ms, it sleeps for_ms before its pass, imitating a scheduling stall.
	unsigned long stallAtUs = 0;
	unsigned long stallForUs = 0;
	bool stallDone = false;

	std::atomic<unsigned long> alErrors{0};
};

/// Heap-allocated once and never freed, so it outlives every static and the service thread:
/// serviceLoop and LibraryGuard read it without regard to static destruction order.
Diagnostics& diagnostics();

/// Reads OPENAL_AUDIO_DIAG and opens the log. Called once by AIL_startup.
void diagnosticsInit();

/// Flushes and closes a file log (never stderr) and turns diagnostics off. Does not throw.
void diagnosticsClose() noexcept;

/// Appends one line to the log when diagnostics are on. printf-style.
void diagnosticsLog(const char* format, ...);

/// Writes every counter as one `counters` line; `when` names the moment (periodic, shutdown).
void diagnosticsReport(const char* when);

/// Raises `slot` to `value` if larger.
void diagnosticsMax(std::atomic<unsigned long>& slot, unsigned long value);

/// Counts an alBufferData and checks the declared format against the bytes being queued.
void diagnosticsBufferData(ALenum format, unsigned int channels, unsigned int bits,
	unsigned int rate, unsigned int bytes, const char* who);

/// Drains alGetError into the counters and log. No-op when diagnostics are off.
void diagnosticsCheckAlError(const char* where);

/// The library lock, timed when diagnostics are on: how long the caller waited for it and how long
/// it held it, per thread class (the service thread against everyone else).
class LibraryGuard
{
public:
	LibraryGuard();
	~LibraryGuard();
	LibraryGuard(const LibraryGuard&) = delete;
	LibraryGuard& operator=(const LibraryGuard&) = delete;

private:
	std::chrono::steady_clock::time_point m_acquired;
	bool m_timed;
	bool m_service;
};

/// Delivers every pending end-of-sample/stream callback on the calling thread. Callbacks are read
/// at delivery time, so one unregistered since the voice stopped is dropped, and a voice restarted
/// since is left alone. Called by ApiCall on the API thread; the tests may call it directly.
void deliverCompletions();

/// Scope of one AIL_* entry point. On the API thread the outermost scope delivers pending
/// completions as it ends, after the entry point's own effect has been applied, so the engine sees
/// its callbacks only while it is inside the library and never while the service thread is
/// concurrently reading engine state. Nested entry points (the engine calls AIL_* from inside its
/// callbacks) and other threads deliver nothing.
class ApiCall
{
public:
	ApiCall();
	~ApiCall();
	ApiCall(const ApiCall&) = delete;
	ApiCall& operator=(const ApiCall&) = delete;

private:
	bool m_onApiThread;
};

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

/// Parses one MPEG audio frame header. False for anything that is not a frame header, including a
/// sync pattern that happens to occur inside tag or payload bytes, which is what the index scan
/// walks past. `available` must be at least MPEG_FRAME_HEADER_BYTES.
bool parseMpegFrameHeader(const unsigned char* at, size_t available, MpegFrameHeader& out);

/// Total length of an ID3v2 tag at the front of a file, including its header and any footer; 0 when
/// there is none. 54 of the 56 retail tracks have one, so the first frame is not at offset 0.
unsigned int id3v2TagLength(const unsigned char* front, size_t size);

/// Creates/destroys/rewinds an MPEG decoder. `destroyMpegDecoder(nullptr)` is a no-op.
MpegDecoder* createMpegDecoder();
void destroyMpegDecoder(MpegDecoder* decoder);
void resetMpegDecoder(MpegDecoder* decoder);

/// Decodes one MPEG frame at `data` into interleaved 16-bit PCM, returning the PCM frames written
/// per channel, or 0 when the frame could not be decoded. `out` needs room for
/// MPEG_MAX_SAMPLES_PER_FRAME samples. `header` reports what the decoder saw, which the stream
/// checks against what it indexed.
unsigned int decodeMpegFrame(MpegDecoder& decoder, const unsigned char* data, unsigned int size,
	int16_t* out, MpegFrameHeader& header);

/// Reads the next chunk of a stream's file through the engine's file callbacks and decodes it to
/// interleaved 16-bit PCM, returning the PCM bytes produced and rewinding at the end of a looping
/// stream. This is what the service thread queues into OpenAL, and what the audio probe drains at
/// full speed to measure a decode without waiting for real time to pass.
unsigned int decodeStreamChunk(StreamVoice& stream, std::vector<unsigned char>& into);

/// Refills a playing stream's buffer queue. Sets `finished` when the stream has run out of data
/// and OpenAL has drained the queue. Called from the service thread with the library lock held.
void serviceStream(StreamVoice& stream, bool& finished);

} // namespace OpenALAudio
