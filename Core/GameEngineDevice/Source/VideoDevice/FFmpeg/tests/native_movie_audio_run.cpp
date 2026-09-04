/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 TheSuperHackers
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

/***********************************************************************************************
 *                                                                                             *
 *  Play a Bink file through the engine's video player with a live AIL/OpenAL audio backend and *
 *  count where the movie's sound goes, so "the intro has no audio" can be measured instead of  *
 *  read off the source.                                                                        *
 *                                                                                             *
 *  The sibling native_video_frame_run.cpp measures the picture. This one measures the sound   *
 *  track of the same stream, over the same engine calls:                                       *
 *                                                                                             *
 *      FFmpegVideoPlayer::createStream(File*)   -> FFmpegVideoStream (real ctor)              *
 *        -> AudioManager::getHandleForBink()    the engine's movie-audio handle                *
 *        -> FFmpegVideoStream::onFrame          decoded audio -> AIL_load_sample_buffer        *
 *      isFrameReady()/frameNext()               paced as LoadScreen.cpp / Display::update do   *
 *        -> AudioManager::releaseHandleForBink()                                              *
 *                                                                                             *
 *  What this file supplies is what the game's AudioManager would have supplied and nothing     *
 *  else: an AIL driver opened the way MilesAudioManager::openDevice opens one, one pooled      *
 *  HSAMPLE handed out by getHandleForBink() and taken back by releaseHandleForBink() the way   *
 *  MilesAudioManager does it, and stdio file callbacks so a stream can be opened afterwards.   *
 *  It does not touch OpenAL: everything below the AIL_* line is the shim the game links.       *
 *                                                                                             *
 *  The rendered PCM is captured by OpenAL Soft's wave writer (scripts/native-movie-audio-      *
 *  run.py sets ALSOFT_CONF) and compared there against an independent ffmpeg decode; this      *
 *  prints the counts -- audio frames the decoder produced, buffers the sink accepted, the      *
 *  audio clock against the video clock at every frame -- and the exit code is the number of    *
 *  stages that failed.                                                                        *
 *                                                                                             *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#include "Common/CriticalSection.h"
#include "Common/GameAudio.h"
#include "Common/GameMemory.h"
#include "Common/file.h"

#include "GameClient/VideoPlayer.h"
#include "MilesAudioDevice/MilesAudioManager.h"
#include "VideoDevice/FFmpeg/FFmpegFile.h"
#include "VideoDevice/FFmpeg/FFmpegVideoPlayer.h"

extern "C" {
	#include <libavcodec/avcodec.h>
	#include <libavutil/frame.h>
}

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#ifdef _WIN32
int main()
{
	std::printf("native_movie_audio_run: on Windows Bink and Miles are the oracle; this harness is "
		"the off-Windows substitute and is not built here.\n");
	return 0;
}
#else

namespace
{

int Failures = 0;

ImmortalCriticalSection AsciiStringSection;
ImmortalCriticalSection UnicodeStringSection;
ImmortalCriticalSection DmaSection;
ImmortalCriticalSection MemoryPoolSection;
ImmortalCriticalSection DebugLogSection;

void Engine_Prologue()
{
	TheAsciiStringCriticalSection = AsciiStringSection.get();
	TheUnicodeStringCriticalSection = UnicodeStringSection.get();
	TheDmaCriticalSection = DmaSection.get();
	TheMemoryPoolCriticalSection = MemoryPoolSection.get();
	TheDebugLogCriticalSection = DebugLogSection.get();
	initMemoryManager();
}

void Stage(const char * name, bool ok, const char * detail = NULL)
{
	if (!ok) Failures++;
	std::printf("%-52s %s%s%s\n", name, ok ? "ok" : "FAILED",
		detail != NULL ? "  " : "", detail != NULL ? detail : "");
	std::fflush(stdout);
}

// ------------------------------------------------------------------ the movie, as a File

class DiskFileClass : public File
{
public:
	DiskFileClass() : Handle(NULL) {}
	~DiskFileClass() override { if (Handle != NULL) fclose(Handle); }

	bool Open_Path(const char * path)
	{
		Handle = fopen(path, "rb");
		return Handle != NULL;
	}

	Int read(void * buffer, Int bytes) override
	{
		if (Handle == NULL) return -1;
		return (Int)fread(buffer, 1, (size_t)bytes, Handle);
	}

	void close() override
	{
		if (Handle != NULL) fclose(Handle);
		Handle = NULL;
	}

	Int seek(Int bytes, seekMode mode = CURRENT) override
	{
		if (Handle == NULL) return -1;
		const int whence = (mode == START) ? SEEK_SET : (mode == END) ? SEEK_END : SEEK_CUR;
		if (fseek(Handle, bytes, whence) != 0) return -1;
		return (Int)ftell(Handle);
	}

	Int position() override { return Handle != NULL ? (Int)ftell(Handle) : -1; }

	Int size() override
	{
		if (Handle == NULL) return -1;
		const long here = ftell(Handle);
		fseek(Handle, 0, SEEK_END);
		const long end = ftell(Handle);
		fseek(Handle, here, SEEK_SET);
		return (Int)end;
	}

	Int readChar() override { return -1; }
	Int readWideChar() override { return -1; }
	Int write(const void *, Int) override { return -1; }
	Int writeFormat(const Char *, ...) override { return -1; }
	Int writeFormat(const WideChar *, ...) override { return -1; }
	Int writeChar(const Char *) override { return -1; }
	Int writeChar(const WideChar *) override { return -1; }
	Bool flush() override { return FALSE; }
	void nextLine(Char * = NULL, Int = 0) override {}
	Bool scanInt(Int &) override { return FALSE; }
	Bool scanReal(Real &) override { return FALSE; }
	Bool scanString(AsciiString &) override { return FALSE; }
	char * readEntireAndClose() override { return NULL; }
	File * convertToRAMFile() override { return NULL; }

private:
	FILE * Handle;
};

class VideoPlayerHarnessClass : public FFmpegVideoPlayer
{
public:
	VideoStreamInterface * Open_File(File * file) { return createStream(file); }
};

// ------------------------------------------------------------------ the audio manager

// stdio behind AIL_set_file_callbacks, so a stream (the menu music's route) can be opened after
// the movie without TheFileSystem. MilesAudioManager registers the engine's own set of these.
unsigned long __stdcall File_Open(const char * name, void ** handle)
{
	FILE * file = fopen(name, "rb");
	*handle = file;
	return file != NULL ? 1 : 0;
}

void __stdcall File_Close(void * handle) { if (handle != NULL) fclose((FILE *)handle); }

long __stdcall File_Seek(void * handle, long offset, unsigned long whence)
{
	const int mode = (whence == AIL_FILE_SEEK_BEGIN) ? SEEK_SET
		: (whence == AIL_FILE_SEEK_END) ? SEEK_END : SEEK_CUR;
	fseek((FILE *)handle, offset, mode);
	return ftell((FILE *)handle);
}

unsigned long __stdcall File_Read(void * handle, void * buffer, unsigned long bytes)
{
	return (unsigned long)fread(buffer, 1, bytes, (FILE *)handle);
}

// The game's AudioManager with sound on is MilesAudioManager over the AIL shim, whose
// getHandleForBink() lends the video player one pooled HSAMPLE and whose releaseHandleForBink()
// takes it back. Its init() needs the INI data half of the engine, so this opens the driver
// exactly as MilesAudioManager::openDevice does, keeps a pool of one, and does what
// getHandleForBink / releaseMilesHandles do to it -- counting every call, which is the
// measurement.
class MovieAudioManagerClass : public MilesAudioManagerDummy
{
public:
	MovieAudioManagerClass(unsigned rate) : Digital(NULL), Pooled(NULL), Lent(NULL),
		Handle_Requests(0), Handle_Releases(0)
	{
		AIL_set_file_callbacks(File_Open, File_Close, File_Seek, File_Read);
		AIL_startup();
		Started = AIL_quick_startup(1, 0, rate, 16, 2) != 0;
		AIL_quick_handles(&Digital, NULL, NULL);
		if (Started) {
			Pooled = AIL_allocate_sample_handle(Digital);
			if (Pooled != NULL) AIL_init_sample(Pooled);
		}
	}

	~MovieAudioManagerClass() override
	{
		if (Pooled != NULL) AIL_release_sample_handle(Pooled);
		AIL_shutdown();
	}

	void * getHandleForBink() override
	{
		Handle_Requests++;
		if (Lent == NULL && Pooled != NULL) {
			// MilesAudioManager::getHandleForBink: a sample off the 2D pool, kept until released.
			Lent = Pooled;
		}
#ifdef MSS_SAMPLE_BUFFER_API
		return Lent;
#else
		AILLPDIRECTSOUND lpDS = NULL;
		if (Lent != NULL) AIL_get_DirectSound_info(Lent, &lpDS, NULL);
		return lpDS;
#endif
	}

	void releaseHandleForBink() override
	{
		Handle_Releases++;
		if (Lent != NULL) {
			// MilesAudioManager::stopPlayingAudio -> releaseMilesHandles on a PAT_Sample.
			AIL_register_EOS_callback(Lent, NULL);
			AIL_stop_sample(Lent);
			Lent = NULL;
		}
	}

	virtual Real getVolume(AudioAffect which) override { return Speech_Volume; }

	bool Started = false;
	HDIGDRIVER Digital;
	HSAMPLE Pooled;
	HSAMPLE Lent;
	int Handle_Requests;
	int Handle_Releases;
	Real Speech_Volume = 0.7f;
};

// ------------------------------------------------------------------ the independent decode count

// How many audio frames the engine's own FFmpegFile produces for this movie, counted by a second
// FFmpegFile over the same file with this callback in place of the video stream's. It shares the
// decoder with the stream under test (that is the point: it is what the stream *could* deliver)
// and nothing else.
struct Decode_Count
{
	int audio_frames = 0;
	long long audio_samples = 0;
	int video_frames = 0;
	int sample_rate = 0;
	int channels = 0;
	unsigned frame_time_ms = 0;
};

void Count_Frame(AVFrame * frame, int, int stream_type, void * user_data)
{
	Decode_Count * count = (Decode_Count *)user_data;
	if (stream_type == AVMEDIA_TYPE_AUDIO) {
		count->audio_frames++;
		count->audio_samples += frame->nb_samples;
		count->sample_rate = frame->sample_rate;
		count->channels = frame->ch_layout.nb_channels;
	} else if (stream_type == AVMEDIA_TYPE_VIDEO) {
		count->video_frames++;
	}
}

Decode_Count Count_Decodable(const char * movie)
{
	Decode_Count count;
	DiskFileClass file;
	if (!file.Open_Path(movie)) return count;
	FFmpegFile * decoder = NEW FFmpegFile();
	if (!decoder->open(&file)) {
		delete decoder;
		return count;
	}
	count.frame_time_ms = decoder->getFrameTime();
	decoder->setFrameCallback(Count_Frame);
	decoder->setUserData(&count);
	while (decoder->decodePacket()) {}
	delete decoder;
	return count;
}

long long Now_Ms()
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
}

}	// namespace

int main(int argc, char ** argv)
{
	const char * movie = NULL;
	const char * music = NULL;
	int pump_ms = 1;
	int max_frames = 1 << 30;
	for (int index = 1; index < argc; index++) {
		if (strcmp(argv[index], "--movie") == 0 && index + 1 < argc) movie = argv[++index];
		// A WAV to open as an AIL stream once the movie is over: the menu music's route, so "the
		// menu still plays" is a rendered fact and not the absence of a crash.
		else if (strcmp(argv[index], "--music") == 0 && index + 1 < argc) music = argv[++index];
		// How often the display pumps the stream. LoadScreen.cpp spins with Sleep(1); the intro
		// route (Display::update) pumps once per engine frame, ~33 ms.
		else if (strcmp(argv[index], "--pump-ms") == 0 && index + 1 < argc) pump_ms = atoi(argv[++index]);
		else if (strcmp(argv[index], "--max-frames") == 0 && index + 1 < argc)
			max_frames = atoi(argv[++index]);
	}
	if (movie == NULL) {
		std::printf("usage: native_movie_audio_run --movie FILE.bik [--music FILE.wav] "
			"[--pump-ms N] [--max-frames N]\n");
		return 2;
	}

	Engine_Prologue();

	std::printf("== the decoder\n");
	const Decode_Count decodable = Count_Decodable(movie);
	std::printf("FFmpegFile audio stream: %s, %d audio frames, %lld samples, %d Hz, %d channels; "
		"%d video frames\n", decodable.audio_frames > 0 ? "found" : "NOT FOUND",
		decodable.audio_frames, decodable.audio_samples, decodable.sample_rate, decodable.channels,
		decodable.video_frames);
	Stage("FFmpegFile finds an audio stream", decodable.audio_frames > 0);

	std::printf("== the audio manager\n");
	const unsigned device_rate = decodable.sample_rate > 0 ? (unsigned)decodable.sample_rate : 44100u;
	MovieAudioManagerClass * audio = NEW MovieAudioManagerClass(device_rate);
	TheAudio = audio;
	Stage("AIL driver opened (MilesAudioManager::openDevice's calls)", audio->Started);
	Stage("a pooled HSAMPLE exists", audio->Pooled != NULL);
#ifdef MSS_SAMPLE_BUFFER_API
	std::printf("MSS_SAMPLE_BUFFER_API defined: getHandleForBink hands out the HSAMPLE\n");
#else
	std::printf("MSS_SAMPLE_BUFFER_API not defined: getHandleForBink asks AIL_get_DirectSound_info\n");
#endif
#ifdef RTS_USE_OPENAL
	std::printf("RTS_USE_OPENAL defined\n");
#else
	std::printf("RTS_USE_OPENAL not defined: the upstream OpenALAudioStream movie path is compiled out\n");
#endif

	std::printf("== the stream\n");
	DiskFileClass file;
	Stage("the movie file opened", file.Open_Path(movie), movie);
	VideoPlayerHarnessClass * player = NEW VideoPlayerHarnessClass;
	const long long created_at = Now_Ms();
	VideoStreamInterface * stream = player->Open_File(&file);
	Stage("FFmpegVideoPlayer::createStream", stream != NULL);
	if (stream == NULL) return Failures;
	std::printf("getHandleForBink calls during createStream: %d; handle %s\n",
		audio->Handle_Requests, audio->Lent != NULL ? "non-null" : "NULL");
	Stage("the stream took the audio handle", audio->Lent != NULL);

	// The display's loop: pump, and when the stream says the next frame is due, advance. At each
	// advance write down the video clock (frame index x frame time) and the audio clock (what the
	// AIL sample says it has played), which is the A/V sync measurement.
	const int frame_count = stream->frameCount();
	const long frame_time = (long)decodable.frame_time_ms;
	long total_ms = 0, current_ms = 0;
	int delivered = 0;
	long worst_av_ms = 0;
	int frames_over_one_frame = 0;
	std::printf("av (frame, videoMs, audioMs, wallMs):");
	while (stream->frameIndex() < frame_count - 1 && delivered < max_frames) {
		if (!stream->isFrameReady()) {
			std::this_thread::sleep_for(std::chrono::milliseconds(pump_ms));
			continue;
		}
		stream->frameNext();
		delivered++;
		if (audio->Lent != NULL) {
			AIL_sample_ms_position(audio->Lent, &total_ms, &current_ms);
		}
		const long long wall = Now_Ms() - created_at;
		// frameIndex() counts decoded frames, so frame (index - 1) is the one now showing, and
		// isFrameReady() let it through at frameTime x (index - 1): that product is the video
		// clock the engine itself steers by. The audio clock is what the AIL sample reports it
		// has played since AIL_start_sample in the same ctor.
		const long video_ms = (long)(stream->frameIndex() - 1) * frame_time;
		std::printf(" (%d,%ld,%ld,%lld)", stream->frameIndex(), video_ms, current_ms, wall);
		const long av = video_ms - current_ms;
		if (labs(av) > labs(worst_av_ms)) worst_av_ms = av;
		if (labs(av) > frame_time + pump_ms) frames_over_one_frame++;
	}
	std::printf("\n");
	std::printf("frames delivered %d of %d; audio loaded %ld ms, played %ld ms at the last frame\n",
		delivered, frame_count, total_ms, current_ms);
	std::printf("A/V: worst (video - audio) %ld ms; frames beyond one video frame (%ld ms) + pump: %d\n",
		worst_av_ms, frame_time, frames_over_one_frame);
	Stage("audio was loaded into the sink", total_ms > 0);
	Stage("audio played (the sample's clock advanced)", current_ms > 0);
	Stage("A/V within one video frame at every frame", frames_over_one_frame == 0);

	std::printf("== the transition\n");
	stream->close();
	player->notifyVideoPlayerOfNewProvider(FALSE);
	Stage("releaseHandleForBink was called", audio->Handle_Releases > 0);
	Stage("the handle went back to the pool", audio->Lent == NULL);
	if (audio->Pooled != NULL) {
		long after_total = 0, after_current = 0;
		AIL_sample_ms_position(audio->Pooled, &after_total, &after_current);
		std::printf("pooled sample after release: %ld ms loaded, %ld ms position\n", after_total,
			after_current);
		Stage("the pooled sample holds no movie audio", after_total == 0);
	}

	if (music != NULL && audio->Digital != NULL) {
		// The menu music's route: AIL_open_stream on the driver the movie just used.
		HSTREAM stream_handle = AIL_open_stream(audio->Digital, music, 0);
		Stage("AIL_open_stream after the movie", stream_handle != NULL, AIL_last_error());
		if (stream_handle != NULL) {
			AIL_set_stream_volume(stream_handle, 127);
			AIL_start_stream(stream_handle);
			S32 music_total = 0, music_current = 0;
			for (int wait = 0; wait < 200; wait++) {
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
				AIL_stream_ms_position(stream_handle, &music_total, &music_current);
				if (music_current >= 500) break;
			}
			std::printf("music stream: %d ms long, %d ms played\n", (int)music_total, (int)music_current);
			Stage("music stream played after the movie", music_current > 0);
			AIL_close_stream(stream_handle);
		}
	}

	delete player;
	TheAudio = NULL;
	delete audio;

	std::printf("\nfailures: %d\n", Failures);
	return Failures;
}

#endif
