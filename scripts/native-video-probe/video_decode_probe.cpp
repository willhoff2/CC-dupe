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

// A probe, not a port fix: it drives the engine's own FFmpegFile translation unit over a real
// container and reports what comes out. Nothing here is compiled into the game.
//
// The engine's video path is FFmpegFile (demux + decode) -> FFmpegVideoStream::frameRender
// (sws_scale into a locked VideoBuffer). FFmpegVideoStream itself cannot be instantiated without
// TheAudio, TheGlobalData and a renderer, so this reproduces *only* its frameRender conversion,
// with the same pixel-format mapping and the same SWS_BICUBIC flag as
// Core/GameEngineDevice/Source/VideoDevice/FFmpeg/FFmpegVideoPlayer.cpp, and says so in its output.
//
// See docs/porting/video-path-findings.md.

#include "VideoDevice/FFmpeg/FFmpegFile.h"
#include "Common/file.h"
#include "Common/AsciiString.h"
#include "Common/UnicodeString.h"
#include "Common/AudioEventRTS.h"
#include "Common/CriticalSection.h"
#include "Common/GameMemory.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

//----------------------------------------------------------------------------
// Link-level stand-ins for the engine machinery `File.cpp` references.
//
// The probe links the real `File.cpp` object out of the native build, and that object pulls in the
// memory-pool factory, the string allocator and (through Debug.h) the audio event destructor --
// i.e. an initialised engine. None of it is on the video path: FFmpegFile only ever calls read()
// and close(). Rather than start an engine to measure a decoder, the probe defines exactly the
// symbols the linker asks for and makes every one that is not provably a no-op abort loudly, so a
// stub can never be mistaken for working code in the output.
//----------------------------------------------------------------------------
static void probeStubReached(const char *what)
{
	fprintf(stderr, "\nPROBE STUB REACHED: %s -- the probe's assumption that the video path does "
	                  "not need this is wrong; report it rather than trusting the numbers above.\n",
	        what);
	abort();
}

// `File::File()` calls `setName("<no file>")`, so the real AsciiString is on the path and the probe
// links `AsciiString.cpp` / `UnicodeString.cpp` rather than stubbing them. What those need is a
// dynamic allocator; the engine's is the pool machinery, and malloc is a faithful stand-in for a
// measurement about video. The critical sections are documented as null outside WinMain.
CriticalSection *TheAsciiStringCriticalSection = nullptr;
CriticalSection *TheUnicodeStringCriticalSection = nullptr;
CriticalSection *TheDmaCriticalSection = nullptr;

alignas(16) static unsigned char g_dmaStorage[sizeof(DynamicMemoryAllocator)] = {};
DynamicMemoryAllocator *TheDynamicMemoryAllocator =
	reinterpret_cast<DynamicMemoryAllocator *>(g_dmaStorage);

void *DynamicMemoryAllocator::allocateBytesDoNotZeroImplementation(Int numBytes)
{
	return malloc((size_t)numBytes);
}
void *DynamicMemoryAllocator::allocateBytesImplementation(Int numBytes)
{
	return calloc(1, (size_t)numBytes);
}
void DynamicMemoryAllocator::freeBytes(void *p) { free(p); }
Int DynamicMemoryAllocator::getActualAllocationSize(Int numBytes) { return numBytes; }

// Referenced by the objects above but not reachable from the video path; loud rather than silent.
MemoryPoolFactory *TheMemoryPoolFactory = nullptr;
void MemoryPool::freeBlock(void *) { probeStubReached("MemoryPool::freeBlock"); }
MemoryPool *MemoryPoolFactory::createMemoryPool(const char *, Int, Int, Int)
{
	probeStubReached("MemoryPoolFactory::createMemoryPool");
	return nullptr;
}
AudioEventRTS::~AudioEventRTS() { probeStubReached("AudioEventRTS::~AudioEventRTS"); }

//----------------------------------------------------------------------------
// A File the probe can hand to FFmpegFile.
//
// The engine's own File subclasses (StdLocalFile, RAMFile, ArchiveFile) need TheFileSystem and the
// memory-pool machinery; FFmpegFile only ever calls read() and close() on what it is given, and
// what matters for the measurement is that the read side behaves like the game's: a forward-only
// stream, since FFmpegFile installs no seek callback on its AVIOContext.
//----------------------------------------------------------------------------
class ProbeFile : public File
{
public:
	ProbeFile() : m_fp(nullptr), m_seeks(0) {}
	~ProbeFile() override { if (m_fp) fclose(m_fp); }

	Bool openPath(const char *path)
	{
		m_fp = fopen(path, "rb");
		return m_fp != nullptr;
	}

	Int read(void *buffer, Int bytes) override
	{
		if (!m_fp) return -1;
		return (Int)fread(buffer, 1, (size_t)bytes, m_fp);
	}

	void close() override
	{
		if (m_fp) { fclose(m_fp); m_fp = nullptr; }
	}

	Int seek(Int bytes, seekMode mode = CURRENT) override
	{
		// Counted, not used: FFmpegFile passes a null seek callback to avio_alloc_context(), so
		// libavformat cannot reach this even when the underlying file is perfectly seekable.
		++m_seeks;
		if (!m_fp) return -1;
		const int whence = (mode == START) ? SEEK_SET : (mode == END) ? SEEK_END : SEEK_CUR;
		if (fseek(m_fp, bytes, whence) != 0) return -1;
		return (Int)ftell(m_fp);
	}

	Int position() override { return m_fp ? (Int)ftell(m_fp) : -1; }
	Int size() override
	{
		if (!m_fp) return -1;
		const long here = ftell(m_fp);
		fseek(m_fp, 0, SEEK_END);
		const long end = ftell(m_fp);
		fseek(m_fp, here, SEEK_SET);
		return (Int)end;
	}

	Int seeksRequested() const { return m_seeks; }
	void resetSeeks() { m_seeks = 0; }

	// Unused by the video path; a probe must not pretend they work.
	Int readChar() override { return -1; }
	Int readWideChar() override { return -1; }
	Int write(const void *, Int) override { return -1; }
	Int writeFormat(const Char *, ...) override { return -1; }
	Int writeFormat(const WideChar *, ...) override { return -1; }
	Int writeChar(const Char *) override { return -1; }
	Int writeChar(const WideChar *) override { return -1; }
	Bool flush() override { return FALSE; }
	void nextLine(Char * = nullptr, Int = 0) override {}
	Bool scanInt(Int &) override { return FALSE; }
	Bool scanReal(Real &) override { return FALSE; }
	Bool scanString(AsciiString &) override { return FALSE; }
	char *readEntireAndClose() override { return nullptr; }
	File *convertToRAMFile() override { return nullptr; }

private:
	FILE *m_fp;
	Int m_seeks;
};

//----------------------------------------------------------------------------
// VideoBuffer::Type, verbatim from Core/GameEngine/Include/GameClient/VideoPlayer.h, and the
// AVPixelFormat each maps to in FFmpegVideoStream::frameRender(). Duplicated rather than included
// because VideoPlayer.h drags in the whole subsystem list.
//----------------------------------------------------------------------------
struct BufferFormat
{
	const char *name;
	AVPixelFormat pix_fmt;
	int bytes_per_pixel;
};

static const BufferFormat BUFFER_FORMATS[] = {
	{ "TYPE_R8G8B8",   AV_PIX_FMT_RGB24,  3 },
	{ "TYPE_X8R8G8B8", AV_PIX_FMT_BGR0,   4 },
	{ "TYPE_R5G6B5",   AV_PIX_FMT_RGB565, 2 },
	{ "TYPE_X1R5G5B5", AV_PIX_FMT_RGB555, 2 },
};

static int g_videoFrames = 0;
static int g_audioFrames = 0;
static int64_t g_audioSamples = 0;
static AVFrame *g_firstFrame = nullptr;
static AVFrame *g_brightestFrame = nullptr;
static int g_brightestIndex = -1;
static double g_brightestMean = -1.0;

// Mean of the first plane. Used only to pick a frame worth looking at: intros open and close on
// black, and a probe that converted only frame 1 would report an all-zero buffer and prove nothing.
static double planeMean(const AVFrame *frame)
{
	if (frame->data[0] == nullptr || frame->linesize[0] <= 0) return 0.0;
	double total = 0.0;
	for (int y = 0; y < frame->height; ++y) {
		const uint8_t *row = frame->data[0] + (size_t)y * frame->linesize[0];
		for (int x = 0; x < frame->width; ++x) total += row[x];
	}
	return total / ((double)frame->width * frame->height);
}

static void onFrame(AVFrame *frame, int, int stream_type, void *)
{
	if (stream_type == AVMEDIA_TYPE_VIDEO) {
		++g_videoFrames;
		if (g_firstFrame == nullptr) g_firstFrame = av_frame_clone(frame);
		const double mean = planeMean(frame);
		if (mean > g_brightestMean) {
			g_brightestMean = mean;
			g_brightestIndex = g_videoFrames;
			av_frame_free(&g_brightestFrame);
			g_brightestFrame = av_frame_clone(frame);
		}
	} else if (stream_type == AVMEDIA_TYPE_AUDIO) {
		++g_audioFrames;
		g_audioSamples += frame->nb_samples;
	}
}

// The conversion FFmpegVideoStream::frameRender() performs, against a buffer allocated the way
// W3DVideoBuffer would: pitch is the row stride the locked surface reports.
static bool convertFrame(AVFrame *frame, const BufferFormat &fmt, int dstWidth, int dstHeight,
                         std::vector<uint8_t> &out, int &pitch, const char *pngPath)
{
	pitch = dstWidth * fmt.bytes_per_pixel;
	out.assign((size_t)pitch * dstHeight, 0);

	SwsContext *sws = sws_getCachedContext(nullptr, frame->width, frame->height,
	                                       (AVPixelFormat)frame->format, dstWidth, dstHeight,
	                                       fmt.pix_fmt, SWS_BICUBIC, nullptr, nullptr, nullptr);
	if (sws == nullptr) {
		printf("      %-14s sws_getCachedContext FAILED\n", fmt.name);
		return false;
	}

	uint8_t *dstData[] = { out.data() };
	int dstStride[] = { pitch };
	const int result = sws_scale(sws, frame->data, frame->linesize, 0, frame->height,
	                             dstData, dstStride);
	sws_freeContext(sws);
	if (result < 0) {
		printf("      %-14s sws_scale FAILED (%d)\n", fmt.name, result);
		return false;
	}

	// Non-black check: a conversion that silently produces an empty buffer must not read as a pass.
	size_t nonZero = 0;
	for (uint8_t byte : out) if (byte != 0) ++nonZero;
	printf("      %-14s -> %-10s pitch %5d  rows %4d  non-zero bytes %zu/%zu\n",
	       fmt.name, av_get_pix_fmt_name(fmt.pix_fmt), pitch, result, nonZero, out.size());

	if (pngPath != nullptr && fmt.pix_fmt == AV_PIX_FMT_RGB24) {
		if (stbi_write_png(pngPath, dstWidth, dstHeight, 3, out.data(), pitch) == 0)
			printf("      (failed to write %s)\n", pngPath);
		else
			printf("      wrote %s\n", pngPath);
	}
	return nonZero > 0;
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: %s <movie file> [png output]\n", argv[0]);
		return 2;
	}
	const char *path = argv[1];
	const char *pngPath = (argc > 2) ? argv[2] : nullptr;

	printf("libavformat %d.%d.%d, libavcodec %d.%d.%d, libswscale %d.%d.%d\n",
	       LIBAVFORMAT_VERSION_MAJOR, LIBAVFORMAT_VERSION_MINOR, LIBAVFORMAT_VERSION_MICRO,
	       LIBAVCODEC_VERSION_MAJOR, LIBAVCODEC_VERSION_MINOR, LIBAVCODEC_VERSION_MICRO,
	       LIBSWSCALE_VERSION_MAJOR, LIBSWSCALE_VERSION_MINOR, LIBSWSCALE_VERSION_MICRO);

	// A static object, not `new ProbeFile`: File's memory-pool glue makes operator new protected,
	// and the pool it wants does not exist outside a running engine.
	static ProbeFile probeFile;
	ProbeFile *file = &probeFile;
	if (!file->openPath(path)) {
		fprintf(stderr, "cannot open %s\n", path);
		return 2;
	}
	unsigned char magic[4] = {0, 0, 0, 0};
	file->read(magic, 4);
	file->seek(0, File::START);
	printf("file        %s (%d bytes), first four bytes %02x %02x %02x %02x '%c%c%c%c'\n",
	       path, file->size(), magic[0], magic[1], magic[2], magic[3],
	       magic[0], magic[1], magic[2], magic[3]);
	// Only libavformat's seeks are interesting; discard the two the probe just made itself.
	file->resetSeeks();

	FFmpegFile ffmpeg;
	const auto openStart = std::chrono::steady_clock::now();
	const Bool opened = ffmpeg.open(file);
	const auto openEnd = std::chrono::steady_clock::now();
	printf("FFmpegFile::open() -> %s (%lld ms)\n", opened ? "true" : "FALSE",
	       (long long)std::chrono::duration_cast<std::chrono::milliseconds>(openEnd - openStart)
	           .count());
	if (!opened)
		return 1;

	const Int pixFmt = ffmpeg.getPixelFormat();
	const char *pixName = av_get_pix_fmt_name((AVPixelFormat)pixFmt);
	printf("  video     %d x %d, pixel format %s\n", ffmpeg.getWidth(), ffmpeg.getHeight(),
	       pixName ? pixName : "<none>");
	printf("  frames    getNumFrames() %d, getFrameTime() %u ms, getCurrentFrame() %d\n",
	       ffmpeg.getNumFrames(), ffmpeg.getFrameTime(), ffmpeg.getCurrentFrame());
	printf("  audio     hasAudio() %s, %d channel(s), %d Hz, %d bytes/sample\n",
	       ffmpeg.hasAudio() ? "true" : "false", ffmpeg.getNumChannels(), ffmpeg.getSampleRate(),
	       ffmpeg.getBytesPerSample());

	ffmpeg.setFrameCallback(onFrame);
	ffmpeg.setUserData(nullptr);

	const auto decodeStart = std::chrono::steady_clock::now();
	int packets = 0;
	while (ffmpeg.decodePacket())
		++packets;
	const auto decodeEnd = std::chrono::steady_clock::now();
	const long long decodeMs =
		std::chrono::duration_cast<std::chrono::milliseconds>(decodeEnd - decodeStart).count();

	printf("  decode    %d packet(s) accepted, %d video frame(s), %d audio frame(s), "
	       "%lld audio sample(s), %lld ms\n",
	       packets, g_videoFrames, g_audioFrames, (long long)g_audioSamples, decodeMs);
	printf("  after     getCurrentFrame() %d\n", ffmpeg.getCurrentFrame());
	printf("  seeks     File::seek() calls libavformat made through FFmpegFile: %d\n",
	       file->seeksRequested());

	// Whether decoding keeps ahead of playback: the engine's clock gives each frame
	// getFrameTime() ms, and FFmpegVideoStream decodes on the game thread.
	if (g_videoFrames > 0 && decodeMs >= 0) {
		const double budget = (double)ffmpeg.getFrameTime() * g_videoFrames;
		printf("  realtime  %.2f ms/frame decoded vs %u ms/frame budget (%.1fx faster than "
		       "playback)\n", (double)decodeMs / g_videoFrames, ffmpeg.getFrameTime(),
		       decodeMs > 0 ? budget / decodeMs : 0.0);
	}

	bool converted = false;
	if (g_brightestFrame != nullptr) {
		AVFrame *frame = g_brightestFrame;
		const char *fmtName = av_get_pix_fmt_name((AVPixelFormat)frame->format);
		printf("  frame %-3d %d x %d, %s, key_frame %d, mean luma %.1f (the brightest frame; "
		       "frame 1 of an intro is black)\n", g_brightestIndex, frame->width, frame->height,
		       fmtName ? fmtName : "?",
#if LIBAVUTIL_VERSION_MAJOR >= 58
		       (frame->flags & AV_FRAME_FLAG_KEY) ? 1 : 0,
#else
		       frame->key_frame,
#endif
		       g_brightestMean);
		printf("  frameRender() conversions, VideoBuffer format -> AVPixelFormat:\n");
		std::vector<uint8_t> out;
		int pitch = 0;
		for (const BufferFormat &fmt : BUFFER_FORMATS) {
			const bool ok = convertFrame(frame, fmt, frame->width, frame->height, out, pitch,
			                             fmt.pix_fmt == AV_PIX_FMT_RGB24 ? pngPath : nullptr);
			converted = converted || ok;
		}
	}

	// frameGoto()'s seek, which the engine calls from VideoStream::frameGoto(). Probed because the
	// comment in FFmpegFile::seekFrame() says it was never tested.
	printf("  seekFrame(0):\n");
	ffmpeg.seekFrame(0);

	printf("\nRESULT: open=%s frames=%d converted=%s\n", opened ? "ok" : "failed", g_videoFrames,
	       converted ? "ok" : "no");
	av_frame_free(&g_firstFrame);
	av_frame_free(&g_brightestFrame);
	return (opened && g_videoFrames > 0 && converted) ? 0 : 1;
}
