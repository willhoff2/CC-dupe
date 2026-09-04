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
 *  Drive a Bink file through the engine's decode -> texture -> draw path off Windows and read  *
 *  the framebuffer back, so "the game shows a video" can be checked instead of assumed.        *
 *                                                                                             *
 *  WHY THIS EXISTS. The FFmpeg slice measured the decode half (docs/porting/video-path-        *
 *  findings.md): frames come out of `FFmpegFile` and `sws_scale` converts them. What it could  *
 *  not do was hand a converted frame to a texture, because there was no renderer to make a    *
 *  texture with. There is one now, so this harness closes the two halves into one path and     *
 *  writes down what came out of the far end.                                                  *
 *                                                                                             *
 *  Every call on the path is the engine's own, and there is exactly one upload route:          *
 *                                                                                             *
 *      FFmpegVideoPlayer::createStream(File*)        the game's own stream factory             *
 *        -> FFmpegVideoStream (real ctor: decodes the first frame)                             *
 *        -> FFmpegVideoStream::frameRender(VideoBuffer*)                                       *
 *             -> W3DVideoBuffer::lock()  -> TextureClass::Get_Surface_Level()                  *
 *                                        -> SurfaceClass::Lock()      (C1, whole-surface)      *
 *             -> sws_scale into that pointer at VideoBuffer::pitch()                           *
 *             -> W3DVideoBuffer::unlock() -> SurfaceClass::Unlock()                            *
 *        -> Render2DClass::Add_Quad(screen, VideoBuffer::Rect(0,0,1,1))                        *
 *        -> DX8Wrapper::Begin_Scene/End_Scene -> the Vulkan backend -> readback                *
 *                                                                                             *
 *  What this file supplies is what `GameClient` would have supplied and nothing else: the      *
 *  window, a `File` over a path on disk (the game's `open()` resolves a name inside            *
 *  `Data/Movies` and then does the same thing), an `AudioManager` because the stream factory   *
 *  reads a volume off it, and the two `Render2DClass` calls `W3DDisplay::drawVideoBuffer`       *
 *  makes. It does not reimplement the conversion, the lock, or the upload: a second upload      *
 *  path is exactly what this slice was told not to write, and a harness with its own copy of    *
 *  `frameRender` would prove nothing about the engine's.                                       *
 *                                                                                             *
 *  It cannot pass by accident. The pixels it prints are read back out of the colour target      *
 *  after the flip; the comparison against an independently decoded reference frame -- and the   *
 *  channel-swap, vertical-flip and pitch controls that catch a plausible-looking wrong image -- *
 *  are done by scripts/native-video-frame-run.py on the PNGs this writes. Every stage prints    *
 *  the value the engine returned and the exit code is the number of stages that failed.         *
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

// Before W3DVideoBuffer.h, which names WW3DFormat without including it (in the game the precompiled
// header has already been through ww3dformat.h by then).
#include "ww3dformat.h"
#include "W3DDevice/GameClient/W3DVideoBuffer.h"

#include "dx8wrapper.h"
#include "render2d.h"
#include "texture.h"
#include "ww3d.h"

#include "platform/platform_window.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#ifdef _WIN32
int main()
{
	std::printf("native_video_frame_run: on Windows the retail game and BINKW32/D3D8 are the "
		"oracle; this harness is the off-Windows substitute and is not built here.\n");
	return 0;
}
#else

#include "vulkanrenderbackend.h"

namespace
{

int Failures = 0;

// main()'s prologue in GeneralsMD/Code/Main/PlatformMain.cpp. Same reason as
// WW3D2/tests/native_render_run.cpp: every engine allocation, and every allocation the Vulkan
// driver makes on this thread, depends on it having run.
//
// TheSuperHackers @bugfix Devin 17/08/2026 Immortal, for the reason
// WW3D2/tests/native_render_run.cpp is: FFmpeg's and the driver's static destructors free through
// the engine's operator new after these statics would have been destroyed
// (docs/porting/allocator-lock-failure.md).
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
	std::printf("%-46s %s%s%s\n", name, ok ? "ok" : "FAILED",
		detail != NULL ? "  " : "", detail != NULL ? detail : "");
	std::fflush(stdout);
}

// ------------------------------------------------------------------ the movie, as a File

// The game hands `createStream` whatever `TheFileSystem->openFile` returned for
// `Data\Movies\<name>.bik`; that resolution is already measured (video-path-findings.md §3) and
// needs TheFileSystem and TheGlobalData, which would drag the data half of the engine into a
// renderer run. `FFmpegFile` only ever calls read() and close(), and it installs no seek callback,
// so this is a forward-only reader over a path -- the same shape the game's file has.
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

	// Not on the video path. A harness must not claim they work.
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

// `FFmpegVideoPlayer::createStream` is protected, because in the game only the player itself opens
// a stream. This is the player, with that one entry point named -- not a replacement for it.
class VideoPlayerHarnessClass : public FFmpegVideoPlayer
{
public:
	VideoStreamInterface * Open_File(File * file) { return createStream(file); }
};

// ------------------------------------------------------------------ the buffer format

const char * Format_Name(VideoBuffer::Type format)
{
	switch (format) {
		case VideoBuffer::TYPE_R8G8B8:		return "R8G8B8";
		case VideoBuffer::TYPE_X8R8G8B8:	return "X8R8G8B8";
		case VideoBuffer::TYPE_R5G6B5:		return "R5G6B5";
		case VideoBuffer::TYPE_X1R5G5B5:	return "X1R5G5B5";
		default:							return "UNKNOWN";
	}
}

VideoBuffer::Type Format_From_Name(const char * name)
{
	if (strcmp(name, "R8G8B8") == 0) return VideoBuffer::TYPE_R8G8B8;
	if (strcmp(name, "X8R8G8B8") == 0) return VideoBuffer::TYPE_X8R8G8B8;
	if (strcmp(name, "R5G6B5") == 0) return VideoBuffer::TYPE_R5G6B5;
	if (strcmp(name, "X1R5G5B5") == 0) return VideoBuffer::TYPE_X1R5G5B5;
	return VideoBuffer::TYPE_UNKNOWN;
}

// W3DDisplay::createVideoBuffer(), which is where the game decides what a movie is uploaded as:
// the backbuffer format if the caps support it as a texture, then X8R8G8B8, R8G8B8, R5G6B5,
// X1R5G5B5. The one branch not reproduced is the low-memory downgrade to R5G6B5, which needs
// TheGameLODManager; --format forces a specific one instead, so all four can be measured.
VideoBuffer::Type Buffer_Format_The_Display_Would_Choose()
{
	const WW3DFormat display = DX8Wrapper::getBackBufferFormat();
	std::printf("backbuffer format %d, caps say texture support:", (int)display);
	static const VideoBuffer::Type candidates[] = { VideoBuffer::TYPE_X8R8G8B8,
		VideoBuffer::TYPE_R8G8B8, VideoBuffer::TYPE_R5G6B5, VideoBuffer::TYPE_X1R5G5B5 };
	for (unsigned index = 0; index < sizeof(candidates) / sizeof(candidates[0]); index++) {
		std::printf(" %s=%d", Format_Name(candidates[index]),
			DX8Wrapper::Get_Current_Caps()->Support_Texture_Format(
				W3DVideoBuffer::TypeToW3DFormat(candidates[index])) ? 1 : 0);
	}
	std::printf("\n");
	if (DX8Wrapper::Get_Current_Caps()->Support_Texture_Format(display)) {
		const VideoBuffer::Type native = W3DVideoBuffer::W3DFormatToType(display);
		if (native != VideoBuffer::TYPE_UNKNOWN) return native;
	}
	static const VideoBuffer::Type fallbacks[] = { VideoBuffer::TYPE_X8R8G8B8,
		VideoBuffer::TYPE_R8G8B8, VideoBuffer::TYPE_R5G6B5, VideoBuffer::TYPE_X1R5G5B5 };
	for (unsigned index = 0; index < sizeof(fallbacks) / sizeof(fallbacks[0]); index++) {
		if (DX8Wrapper::Get_Current_Caps()->Support_Texture_Format(
				W3DVideoBuffer::TypeToW3DFormat(fallbacks[index]))) {
			return fallbacks[index];
		}
	}
	return VideoBuffer::TYPE_UNKNOWN;
}

// ------------------------------------------------------------------ the requirement ledger

// What the video path needs that it does not have. Every line below is read out of engine code at
// run time over the movie it was given -- the integers the engine's own arithmetic produces, the
// frames its own decoder delivers, the instants its own readiness predicate becomes true -- so the
// ranked list in the doc is measurement rather than a reading of the source. No render device is
// needed for any of it, which is what makes it cheap enough to run over the whole retail inventory.
// scripts/video-path-gaps.py supplies the independent frame rate and duration and does the
// comparing; this only reports.
int Measure_Gaps(const char * movie, int pacing_frames)
{
	std::printf("== %s\n", movie);

	// The container facts, from the engine's own accessors rather than from ffprobe: getNumFrames()
	// and getFrameTime() are the two integers the load screen and Display::update() steer by.
	DiskFileClass header_file;
	if (!header_file.Open_Path(movie)) {
		std::printf("gap: the movie file did not open\n");
		return 1;
	}
	FFmpegFile * header = NEW FFmpegFile();
	if (!header->open(&header_file)) {
		std::printf("gap: FFmpegFile::open failed\n");
		delete header;
		return 1;
	}
	std::printf("size %dx%d pixfmt %d\n", header->getWidth(), header->getHeight(),
		header->getPixelFormat());
	std::printf("frameCount %d\n", header->getNumFrames());
	std::printf("frameTime %u\n", header->getFrameTime());
	std::printf("audio %d channels %d sampleRate %d bytesPerSample %d\n",
		header->hasAudio() ? 1 : 0, header->getNumChannels(), header->getSampleRate(),
		header->getBytesPerSample());
	// LoadScreen.cpp's movie loop divides the frame index by this. It is an integer division of
	// frameCount() by FRAME_FUDGE_ADD (30), so any movie shorter than 30 frames makes it zero.
	std::printf("progressUpdateCount %d\n", header->getNumFrames() / 30);
	delete header;

	// Now the stream the game would play, and every frame of it. `frameNext()` decodes until the
	// next video frame arrives, so the count below is frames actually delivered to a caller.
	DiskFileClass play_file;
	if (!play_file.Open_Path(movie)) {
		std::printf("gap: the movie file did not open twice\n");
		return 1;
	}
	VideoPlayerHarnessClass * player = NEW VideoPlayerHarnessClass;
	VideoStreamInterface * stream = player->Open_File(&play_file);
	if (stream == NULL) {
		std::printf("gap: createStream returned null\n");
		delete player;
		return 1;
	}
	const int reported = stream->frameCount();
	std::printf("firstFrameIndex %d\n", stream->frameIndex());

	// The load screen's own loop condition, and its own readiness gate, at the speed the engine
	// would run them: spin on isFrameReady() exactly as LoadScreen.cpp does (Sleep(1) between
	// tries) and write down when each frame was allowed through. Capped, because a 23 MB briefing
	// paces itself in real time and the point is made in the first seconds.
	const std::chrono::steady_clock::time_point began = std::chrono::steady_clock::now();
	int delivered = 0;
	int gate_waits = 0;
	std::printf("pacing");
	while (stream->frameIndex() < reported - 1 && delivered < pacing_frames) {
		if (!stream->isFrameReady()) {
			gate_waits++;
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
			continue;
		}
		stream->frameNext();
		delivered++;
		const long long at = std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now() - began).count();
		std::printf(" %lld", at);
	}
	std::printf("\n");
	std::printf("pacedFrames %d gateWaits %d lastFrameIndex %d\n", delivered, gate_waits,
		stream->frameIndex());

	// And the rest of the movie, without the gate, so "how many frames are actually in it" is a
	// count and not duration x rate. frameNext() stops advancing once the decoder is done.
	int decoded_to_end = 0;
	int previous = stream->frameIndex();
	for (;;) {
		stream->frameNext();
		const int now = stream->frameIndex();
		if (now == previous) break;
		previous = now;
		decoded_to_end++;
		if (decoded_to_end > 200000) break;
	}
	std::printf("framesDecodable %d (frameCount said %d)\n", previous, reported);

	stream->close();
	delete player;

	// frameGoto() is the load screen's min-spec path, and it gets its own stream: a seek that
	// damages the stream would otherwise be indistinguishable from the end of the movie in the
	// count above. Ask for a frame, report where the stream says it is, and then decode a few more
	// frames to see whether it still can.
	DiskFileClass seek_file;
	if (!seek_file.Open_Path(movie)) {
		std::printf("gap: the movie file did not open a third time\n");
		return 1;
	}
	VideoPlayerHarnessClass * seek_player = NEW VideoPlayerHarnessClass;
	VideoStreamInterface * seek_stream = seek_player->Open_File(&seek_file);
	if (seek_stream == NULL) {
		std::printf("gap: createStream returned null for the seek probe\n");
		delete seek_player;
		return 1;
	}
	const int goto_target = reported > 4 ? reported / 2 : 0;
	seek_stream->frameGoto(goto_target);
	const int after_goto = seek_stream->frameIndex();
	int after_next = after_goto;
	for (int step = 0; step < 4; step++) {
		seek_stream->frameNext();
		after_next = seek_stream->frameIndex();
	}
	std::printf("frameGoto %d -> frameIndex %d -> after 4x frameNext %d\n", goto_target, after_goto,
		after_next);
	seek_stream->close();
	delete seek_player;

	std::printf("\n");
	return 0;
}

// ------------------------------------------------------------------ the draw

// W3DDisplay::drawVideoBuffer(), which is three calls: the 2D render state for a textured quad,
// the quad itself over the video's own texture rectangle, and Render(). `setup2DRenderState` is a
// W3DDisplay member and W3DDisplay needs the whole GameClient, so the two state calls it makes for
// DRAW_IMAGE_ALPHA are made here directly, on the engine's Render2DClass.
// The two switches are diagnostic controls, not fallbacks: `alpha` is the DRAW_IMAGE_ALPHA state the
// display asks for, and `textured` is what separates "the quad did not draw" from "the quad drew the
// texture and the texture is wrong". Both are reported in the run's output.
void Draw_Video_Buffer(Render2DClass & renderer, VideoBuffer * buffer, int start_x, int start_y,
	int end_x, int end_y, bool alpha = true, bool textured = true)
{
	W3DVideoBuffer * vbuffer = (W3DVideoBuffer *)buffer;
	renderer.Reset();
	renderer.Enable_Texturing(textured);
	renderer.Set_Texture(textured ? vbuffer->texture() : NULL);
	renderer.Enable_Additive(false);
	renderer.Enable_Alpha(alpha);
	renderer.Enable_Grayscale(false);
	renderer.Add_Quad(RectClass(start_x, start_y, end_x, end_y), vbuffer->Rect(0, 0, 1, 1));
	renderer.Render();
}

}	// namespace

int main(int argc, char ** argv)
{
	const char * movie = NULL;
	const char * png_prefix = NULL;
	const char * forced_format = NULL;
	int frames = 3;
	int start_frame = 0;
	bool seek_probe = false;
	bool no_alpha = false;
	bool untextured = false;
	bool opaque_white = false;
	bool measure_gaps = false;
	int pacing_frames = 60;
	for (int index = 1; index < argc; index++) {
		if (strcmp(argv[index], "--movie") == 0 && index + 1 < argc) movie = argv[++index];
		else if (strcmp(argv[index], "--png-prefix") == 0 && index + 1 < argc) png_prefix = argv[++index];
		else if (strcmp(argv[index], "--format") == 0 && index + 1 < argc) forced_format = argv[++index];
		else if (strcmp(argv[index], "--frames") == 0 && index + 1 < argc) frames = atoi(argv[++index]);
		// Logos open on black, and a black frame drawn correctly is indistinguishable from nothing
		// drawn at all; this walks the stream forward with the engine's own frameNext() first.
		else if (strcmp(argv[index], "--start-frame") == 0 && index + 1 < argc)
			start_frame = atoi(argv[++index]);
		// frameGoto()'s only caller is the load screen's min-spec path; this asks the engine to
		// perform one and prints what it answered.
		else if (strcmp(argv[index], "--seek-probe") == 0) seek_probe = true;
		else if (strcmp(argv[index], "--no-alpha") == 0) no_alpha = true;
		else if (strcmp(argv[index], "--untextured-quad") == 0) untextured = true;
		else if (strcmp(argv[index], "--opaque-white") == 0) opaque_white = true;
		// The requirement ledger: no render device, so it can be run over every retail movie.
		else if (strcmp(argv[index], "--measure-gaps") == 0) measure_gaps = true;
		else if (strcmp(argv[index], "--pacing-frames") == 0 && index + 1 < argc)
			pacing_frames = atoi(argv[++index]);
	}
	if (movie == NULL) {
		std::printf("usage: native_video_frame_run --movie FILE.bik [--frames N] "
			"[--start-frame N] [--png-prefix PREFIX] [--format X8R8G8B8] [--seek-probe]\n");
		return 2;
	}

	Engine_Prologue();

	// `FFmpegVideoPlayer::createStream` reads the speech volume off TheAudio to scale the movie's
	// audio track, so a null TheAudio is a null dereference before any decoding happens. This is
	// the engine's own headless AudioManager, the one --headless runs with; its getHandleForBink
	// returns null, so the stream decodes its audio and has nowhere to send it. The sound track is
	// native_movie_audio_run.cpp's business, with an AudioManager that lends a real AIL sample.
	TheAudio = NEW MilesAudioManagerDummy;

	if (measure_gaps) {
		return Measure_Gaps(movie, pacing_frames);
	}

	std::printf("== the movie\n");
	// On the stack, and outliving everything below: `File` is a memory-pool class whose operator
	// new is protected, and `FFmpegFile` closes the file it was given but never deletes it, so the
	// caller owns it. In the game the caller is TheFileSystem.
	DiskFileClass file;
	const bool opened = file.Open_Path(movie);
	Stage("the movie file opened", opened, movie);
	if (!opened) return 1;

	std::printf("\n== the window and the device (as W3DDisplay::init does it)\n");
	WWPlatform::WindowConfig config;
	config.Title = "Zero Hour native video frame run";
	// Sized in the first stage below, once the stream has been asked how big the movie is: at 1:1
	// the readback and the decoded frame are the same grid of pixels, so a delta is a delta and
	// not a resampling artefact.
	config.Width = 640;
	config.Height = 480;

	// The stream first, because the window wants the movie's size. This is the real
	// FFmpegVideoStream constructor: it installs the frame callback and decodes packets until the
	// first video frame arrives.
	VideoPlayerHarnessClass * player = NEW VideoPlayerHarnessClass;
	VideoStreamInterface * stream = player->Open_File(&file);
	Stage("FFmpegVideoPlayer::createStream", stream != NULL);
	if (stream == NULL) return 1;

	const int movie_width = stream->width();
	const int movie_height = stream->height();
	std::printf("movie: %dx%d, frameCount() %d, first frameIndex() %d\n", movie_width,
		movie_height, stream->frameCount(), stream->frameIndex());
	Stage("the stream reports a size", movie_width > 0 && movie_height > 0);
	if (movie_width <= 0 || movie_height <= 0) return 1;

	config.Width = movie_width;
	config.Height = movie_height;
	void * window = WWPlatform::Window_Create(config);
	Stage("WWPlatform::Window_Create", window != NULL,
		window != NULL ? NULL : WWPlatform::Window_Last_Error());
	if (window == NULL) return 1;
	WWPlatform::Window_Show(window, true);
	WWPlatform::Window_Pump(window);

	// WW3D::Init, not DX8Wrapper::Init: this is what W3DDisplay::init calls, and it is the call that
	// populates the D3D<->WW3D format conversion tables. Without it DX8Caps sees a display format of
	// WW3D_FORMAT_UNKNOWN and reports every texture format as unsupported, which the display would
	// read as "this device cannot show a movie".
	const bool initted = WW3D::Init(window) == WW3D_ERROR_OK;
	Stage("WW3D::Init", initted);
	if (!initted) return 1;
	const bool device = WW3D::Set_Render_Device(0, movie_width, movie_height, 32,
		1 /* windowed */, false /* resize_window */) == WW3D_ERROR_OK;
	Stage("WW3D::Set_Render_Device", device);
	if (!device) return 1;
	std::printf("device: %s\n", WW3D::Get_Render_Device_Name(0));

	std::printf("\n== the buffer (W3DVideoBuffer: TextureClass + SurfaceClass)\n");
	VideoBuffer::Type format = forced_format != NULL ? Format_From_Name(forced_format)
		: Buffer_Format_The_Display_Would_Choose();
	Stage("a supported buffer format", format != VideoBuffer::TYPE_UNKNOWN, Format_Name(format));
	if (format == VideoBuffer::TYPE_UNKNOWN) return 1;

	VideoBuffer * buffer = NEW W3DVideoBuffer(format);
	const bool allocated = buffer->allocate(movie_width, movie_height);
	Stage("W3DVideoBuffer::allocate", allocated);
	if (!allocated) return 1;
	// allocate() locks and unlocks once, so pitch() is the surface's own pitch, not a guess.
	std::printf("visible %ux%u, texture %ux%u, pitch %u bytes (%s), Rect(0,0,1,1) = "
		"%.6f,%.6f..%.6f,%.6f\n", buffer->width(), buffer->height(), buffer->textureWidth(),
		buffer->textureHeight(), buffer->pitch(), Format_Name(buffer->format()),
		buffer->Rect(0, 0, 1, 1).Left, buffer->Rect(0, 0, 1, 1).Top,
		buffer->Rect(0, 0, 1, 1).Right, buffer->Rect(0, 0, 1, 1).Bottom);
	Stage("the lock reported a pitch", buffer->pitch() > 0);
	Stage("W3DVideoBuffer::valid", buffer->valid() == TRUE);

	if (seek_probe) {
		std::printf("\n== frameGoto (the load screen's min-spec path calls this)\n");
		const int before = stream->frameIndex();
		stream->frameGoto(0);
		std::printf("frameGoto(0): frameIndex() %d -> %d\n", before, stream->frameIndex());
	}

	std::printf("\n== %d frames: frameRender into the buffer, then draw and read back\n"
		"(alpha blending %s, texturing %s)\n", frames, no_alpha ? "off" : "on",
		untextured ? "off" : "on");
	Render2DClass * renderer = NEW Render2DClass;
	renderer->Set_Coordinate_Range(RectClass(0, 0, movie_width, movie_height));

	for (int skipped = 0; skipped < start_frame; skipped++) {
		stream->frameNext();
		stream->frameDecompress();
	}
	if (start_frame > 0) {
		std::printf("skipped forward to frameIndex() %d\n", stream->frameIndex());
	}

	for (int frame = 0; frame < frames; frame++) {
		WWPlatform::Window_Pump(window);
		if (frame > 0) {
			// The playback path: the display calls frameNext() when isFrameReady() says the
			// frame's time has come. The pacing is measured separately; here every frame is taken.
			stream->frameNext();
		}
		stream->frameDecompress();
		stream->frameRender(buffer);

		// A known texel written through the same funnel, replacing the frame: opaque white with a
		// full alpha byte. It separates three things a black readback cannot -- the funnel not
		// reaching the sampled image, the quad not sampling the texture, and the frame's own alpha
		// byte (swscale's BGR0 writes zero there) making an otherwise correct upload invisible.
		if (opaque_white) {
			unsigned char * bytes = (unsigned char *)buffer->lock();
			if (bytes != NULL) {
				for (unsigned row = 0; row < buffer->height(); row++) {
					std::memset(bytes + (size_t)row * buffer->pitch(), 0xFF,
						buffer->pitch());
				}
			}
			buffer->unlock();
		}

		// What frameRender left in the buffer, read back through the same lock funnel it wrote
		// through. This separates "the conversion produced nothing" from "the conversion produced a
		// frame the renderer then failed to show", which are different findings with different
		// owners, and it is the only way to tell them apart from a black readback.
		{
			const unsigned char * bytes = (const unsigned char *)buffer->lock();
			unsigned long nonzero = 0;
			unsigned char maximum = 0;
			if (bytes != NULL) {
				for (unsigned row = 0; row < buffer->height(); row++) {
					const unsigned char * line = bytes + (size_t)row * buffer->pitch();
					// The row is as wide as the lock said it was: --format can select a 16- or
					// 24-bit buffer, where a fixed 4 bytes per pixel would read the next row and,
					// on the last row, past the mapping.
					for (unsigned byte = 0; byte < buffer->pitch(); byte++) {
						if (line[byte] != 0) nonzero++;
						if (line[byte] > maximum) maximum = line[byte];
					}
				}
			}
			buffer->unlock();
			std::printf("in the locked buffer after frameRender: %lu non-zero bytes, max %u\n",
				nonzero, maximum);
		}

		DX8Wrapper::Begin_Scene();
		// Black, so that a quad that drew nothing is distinguishable from a quad that drew the
		// movie, and so that any pixel outside the quad is obviously outside it.
		DX8Wrapper::Clear(true, true, Vector3(0.0f, 0.0f, 0.0f), 0.0f, 1.0f, 0);
		// drawScaledVideoBuffer's geometry, which at a window the size of the movie is the whole
		// window: the aspect ratios are equal, so there is no letterbox to reason about.
		Draw_Video_Buffer(*renderer, buffer, 0, 0, movie_width, movie_height, !no_alpha,
			!untextured);
		DX8Wrapper::End_Scene(true);

		VulkanRenderBackendClass::FrameProofClass proof;
		char png_path[512];
		const char * png = NULL;
		if (png_prefix != NULL) {
			std::snprintf(png_path, sizeof(png_path), "%s-frame%d.png", png_prefix, frame);
			png = png_path;
		}
		// Expecting pure black with zero tolerance, so `Matching` counts the pixels the movie did
		// *not* reach. The picture itself is compared by the runner against a reference decode;
		// this is the blank-frame detector, and it is the engine's own readback either way.
		const bool measured = TheVulkanRenderBackend.Measure_Frame(0, 0, 0, 0, png, proof);
		if (!measured) {
			Stage("frame read back", false);
			continue;
		}
		std::printf("frame %d: index %d, %ux%u read back, %lu/%lu pixels still pure black, "
			"centre rgba %u,%u,%u,%u, range r %u..%u g %u..%u b %u..%u%s%s\n", frame,
			stream->frameIndex(), proof.Width, proof.Height, proof.Matching, proof.Pixels,
			proof.CentreRGBA[0], proof.CentreRGBA[1], proof.CentreRGBA[2], proof.CentreRGBA[3],
			proof.MinRGB[0], proof.MaxRGB[0], proof.MinRGB[1], proof.MaxRGB[1], proof.MinRGB[2],
			proof.MaxRGB[2], png != NULL ? ", wrote " : "", png != NULL ? png : "");
		std::fflush(stdout);
		char detail[128];
		std::snprintf(detail, sizeof(detail), "(%lu of %lu pixels are not black)",
			proof.Pixels - proof.Matching, proof.Pixels);
		// A frame that is entirely the clear colour means the quad, the texture or the upload did
		// nothing; it is the failure this whole harness exists to be able to see.
		Stage("the frame is not the clear colour", proof.Matching < proof.Pixels, detail);
	}

	std::printf("\n== the unimplemented-call ledger (each entry is a finding, not a fallback)\n");
	const unsigned kinds = VulkanRenderBackendClass::Unimplemented_Call_Kinds();
	if (kinds == 0) {
		std::printf("(empty: every D3D8 entry point the video path reached is implemented)\n");
	}
	for (unsigned index = 0; index < kinds; index++) {
		const VulkanRenderBackendClass::UnimplementedCallClass * call =
			VulkanRenderBackendClass::Unimplemented_Call(index);
		if (call == NULL) continue;
		std::printf("%6u x  %s\n            %s\n", call->Count, call->Name, call->Why);
	}

	const long validation_messages = TheVulkanRenderBackend.Validation_Message_Count();
	std::printf("\nvalidation messages: %ld%s\n", validation_messages,
		validation_messages < 0 ? " (no device: the layer was never asked anything)" : "");
	Stage("validation layer silent", validation_messages == 0);

	std::printf("\n== shutdown\n");
	renderer->Set_Texture((TextureClass *)NULL);
	delete renderer;
	buffer->free();
	delete buffer;
	stream->close();
	delete player;
	WW3D::Shutdown();
	WWPlatform::Window_Destroy(window);
	Stage("shutdown", true);

	std::printf("\nstages failed: %d\n", Failures);
	return Failures == 0 ? 0 : 1;
}

#endif	// _WIN32
