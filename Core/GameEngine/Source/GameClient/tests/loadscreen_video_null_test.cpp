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

// Behaviour test and negative control for ChallengeLoadScreen::init()'s video stream, driven by
// scripts/native-loadscreen-video-test.py.
//
// TheVideoPlayer->open() returns null for a movie label it cannot serve -- a missing .bik, a
// codec the build has no decoder for, a video device that failed to come up -- and
// ChallengeLoadScreen::init() read width() and height() straight off the result. The Generals
// Challenge screen therefore did not degrade, it dereferenced null.
//
// Three things are asserted, and the second is the point of the test:
//
//   * the shape init() has now survives a null stream: it allocates no buffer, leaks nothing,
//     and still runs the rest of the screen's initialisation (the portraits, the bios and the
//     progress bar), which is what "a missing video degrades" means here.
//   * the shape it replaces is run over the same null stream, in the same process, and reaches
//     the dereference. The driver runs that shape in a child process and requires the child to
//     die on a signal: the control is the crash itself, not a description of it. If the guard is
//     removed, --shape=fixed crashes the same way and the driver fails.
//   * with a stream the player *can* open, both shapes do exactly the same thing, in the same
//     order, with the same calls on the stream. Windows is the oracle and the working path is
//     what Windows runs, so the guard is only allowed to change the null case.
//
// The doubles here stand in for VideoStreamInterface / VideoBuffer / VideoPlayer and carry the
// signatures init() uses. The real classes drag in the display, the file system, the window
// manager and the memory pools; what is under test is the ordering of a null test against a
// dereference, and the scan half of the driver is what ties these shapes to the real file.
// See docs/porting/video-decode-to-texture.md.

#include <stdio.h>
#include <string.h>

static int g_failures = 0;

static void check(bool condition, const char* what)
{
	printf("%s: %s\n", condition ? "ok" : "FAIL", what);
	if (!condition)
		++g_failures;
}

//-----------------------------------------------------------------------------------------------
// The doubles.
//-----------------------------------------------------------------------------------------------

// VideoStreamInterface, reduced to what ChallengeLoadScreen::init() calls on it. The methods are
// virtual because the real ones are: a call through a null VideoStreamInterface* loads the vtable
// pointer from address 0, which is the fault the pre-fix shape takes.
class FakeVideoStream
{
public:
	FakeVideoStream() : m_calls(0), m_closed(false), m_frameIndex(0) {}
	virtual ~FakeVideoStream() {}

	virtual int width() { ++m_calls; return 640; }
	virtual int height() { ++m_calls; return 480; }
	virtual int frameCount() { ++m_calls; return 120; }
	virtual void frameGoto(int frame) { ++m_calls; m_frameIndex = frame; }
	virtual bool isFrameReady() { ++m_calls; return true; }
	virtual void frameDecompress() { ++m_calls; }
	virtual void frameRender(void* buffer) { ++m_calls; m_renderedInto = buffer; }
	virtual void close() { ++m_calls; m_closed = true; }

	int m_calls;
	bool m_closed;
	int m_frameIndex;
	void* m_renderedInto = nullptr;
};

// VideoBuffer: allocate() is the call whose arguments came off the null stream.
class FakeVideoBuffer
{
public:
	explicit FakeVideoBuffer(bool allocateSucceeds) : m_allocateSucceeds(allocateSucceeds) {}
	virtual ~FakeVideoBuffer() { ++s_destroyed; }

	virtual bool allocate(int width, int height)
	{
		m_width = width;
		m_height = height;
		return m_allocateSucceeds;
	}

	static int s_created;
	static int s_destroyed;

	bool m_allocateSucceeds;
	int m_width = 0;
	int m_height = 0;
};

int FakeVideoBuffer::s_created = 0;
int FakeVideoBuffer::s_destroyed = 0;

// TheVideoPlayer->open() and TheDisplay->createVideoBuffer(), the two the shapes call.
struct Environment
{
	// What the player has for the mission's movie label. Null is the case under test.
	FakeVideoStream* streamToOpen = nullptr;
	bool bufferAllocateSucceeds = true;
	bool displayServesBuffers = true;

	FakeVideoStream* open() { ++openCalls; return streamToOpen; }

	FakeVideoBuffer* createVideoBuffer()
	{
		if (!displayServesBuffers)
			return nullptr;
		++FakeVideoBuffer::s_created;
		return new FakeVideoBuffer(bufferAllocateSucceeds);
	}

	int openCalls = 0;
};

// What the screen looks like when init() has run: the video state, plus the pieces that have
// nothing to do with the movie and must still be there.
struct ScreenState
{
	FakeVideoStream* videoStream = nullptr;
	FakeVideoBuffer* videoBuffer = nullptr;
	bool overlaysInited = false;
	bool portraitsAndBiosInited = false;
	bool progressBarInited = false;
	bool piecesActivated = false;
	bool videoBufferHandedToWindow = false;
	// The movie loop the full-spec path runs, or the single held frame the min-spec path draws.
	bool ranMovieLoop = false;
	bool drewHeldFrame = false;
};

//-----------------------------------------------------------------------------------------------
// The two shapes of ChallengeLoadScreen::init(), in the order the real one does things.
//-----------------------------------------------------------------------------------------------

// Before this slice: Core/GameEngine/Source/GameClient/GUI/LoadScreen.cpp:948-960 at 30a7a3434.
// The stream is opened and then read, and the only null test is inside the failure branch, after
// the dereference has already happened.
static void initPreFix(Environment& environment, ScreenState& screen, bool memPassed)
{
	screen.videoStream = environment.open();

	screen.videoBuffer = environment.createVideoBuffer();
	if (screen.videoBuffer == nullptr ||
		!screen.videoBuffer->allocate(screen.videoStream->width(), screen.videoStream->height()))
	{
		delete screen.videoBuffer;
		screen.videoBuffer = nullptr;

		if (screen.videoStream)
		{
			screen.videoStream->close();
			screen.videoStream = nullptr;
		}

		return;
	}

	screen.overlaysInited = true;
	screen.portraitsAndBiosInited = true;
	screen.progressBarInited = true;

	if (memPassed)
	{
		screen.ranMovieLoop = true;
		screen.videoBufferHandedToWindow = screen.videoBuffer != nullptr;
	}
	else
	{
		screen.videoStream->frameGoto(screen.videoStream->frameCount());
		while (!screen.videoStream->isFrameReady())
			;
		screen.videoStream->frameDecompress();
		screen.videoStream->frameRender(screen.videoBuffer);
		if (screen.videoBuffer)
			screen.videoBufferHandedToWindow = true;
		screen.drewHeldFrame = true;
		screen.piecesActivated = true;
	}
}

// After: the null test is immediately after open(), the buffer is only created for a stream that
// exists, and the two playback branches ask for a stream before using one.
static void initFixed(Environment& environment, ScreenState& screen, bool memPassed)
{
	screen.videoStream = environment.open();

	if (screen.videoStream != nullptr)
	{
		screen.videoBuffer = environment.createVideoBuffer();
		if (screen.videoBuffer == nullptr ||
			!screen.videoBuffer->allocate(screen.videoStream->width(),
				screen.videoStream->height()))
		{
			delete screen.videoBuffer;
			screen.videoBuffer = nullptr;

			screen.videoStream->close();
			screen.videoStream = nullptr;

			// Kept: a stream that opens and then cannot be buffered is the pre-fix path, and
			// Windows is the oracle for it. Only the null stream is new behaviour.
			return;
		}
	}

	screen.overlaysInited = true;
	screen.portraitsAndBiosInited = true;
	screen.progressBarInited = true;

	if (screen.videoStream != nullptr && memPassed)
	{
		screen.ranMovieLoop = true;
		screen.videoBufferHandedToWindow = screen.videoBuffer != nullptr;
	}
	else
	{
		if (screen.videoStream != nullptr)
		{
			screen.videoStream->frameGoto(screen.videoStream->frameCount());
			while (!screen.videoStream->isFrameReady())
				;
			screen.videoStream->frameDecompress();
			screen.videoStream->frameRender(screen.videoBuffer);
			if (screen.videoBuffer)
				screen.videoBufferHandedToWindow = true;
			screen.drewHeldFrame = true;
		}
		screen.piecesActivated = true;
	}
}

//-----------------------------------------------------------------------------------------------

static int runFixedOverNullStream()
{
	Environment environment;          // streamToOpen stays null: the player cannot open the label
	ScreenState screen;
	FakeVideoBuffer::s_created = 0;
	FakeVideoBuffer::s_destroyed = 0;

	initFixed(environment, screen, /*memPassed=*/true);

	check(environment.openCalls == 1, "the fixed shape still asks the player for the movie");
	check(screen.videoStream == nullptr, "a null stream stays null");
	check(screen.videoBuffer == nullptr, "no video buffer is left behind for a stream that is not there");
	check(FakeVideoBuffer::s_created == 0,
		"no video buffer is even created for a stream that is not there");
	check(FakeVideoBuffer::s_created == FakeVideoBuffer::s_destroyed,
		"nothing is leaked on the no-video path");
	check(screen.overlaysInited && screen.portraitsAndBiosInited && screen.progressBarInited,
		"the rest of the screen is still initialised: a missing movie degrades, it does not abort");
	check(!screen.ranMovieLoop, "there is no movie loop to run without a stream");
	check(!screen.videoBufferHandedToWindow, "no buffer is handed to the window");
	check(screen.piecesActivated,
		"the Challenge pieces are activated, so the screen is usable without its movie");

	// The min-spec branch reaches the same calls from the other side of the fork.
	ScreenState minSpec;
	Environment minSpecEnvironment;
	initFixed(minSpecEnvironment, minSpec, /*memPassed=*/false);
	check(!minSpec.drewHeldFrame, "the min-spec branch draws no held frame without a stream");
	check(minSpec.piecesActivated, "the min-spec branch still activates the Challenge pieces");

	if (g_failures)
		printf("\nFAILED: %d assertion(s)\n", g_failures);
	else
		printf("\nOK: a null video stream is survived\n");
	return g_failures ? 1 : 0;
}

// The negative control. This function is expected to fault: the driver runs it as a child process
// and requires the child to die on a signal. Nothing here catches or reports anything, because a
// null dereference is not catchable -- that is the finding.
static int runPreFixOverNullStream()
{
	Environment environment;          // the same null stream the fixed shape survives
	ScreenState screen;

	printf("running the pre-fix shape over a null stream; it is expected to fault\n");
	fflush(stdout);

	initPreFix(environment, screen, /*memPassed=*/true);

	// Only reached if the dereference did not fault, which would mean the control proves nothing.
	printf("the pre-fix shape returned without faulting\n");
	return 0;
}

// The working path, which Windows is the oracle for: both shapes must be indistinguishable.
static int compareShapesOverAWorkingStream()
{
	for (int memPassed = 0; memPassed <= 1; ++memPassed)
	{
		for (int allocateSucceeds = 0; allocateSucceeds <= 1; ++allocateSucceeds)
		{
			for (int displayServes = 0; displayServes <= 1; ++displayServes)
			{
				FakeVideoStream beforeStream;
				FakeVideoStream afterStream;

				Environment beforeEnvironment;
				beforeEnvironment.streamToOpen = &beforeStream;
				beforeEnvironment.bufferAllocateSucceeds = allocateSucceeds != 0;
				beforeEnvironment.displayServesBuffers = displayServes != 0;

				Environment afterEnvironment;
				afterEnvironment.streamToOpen = &afterStream;
				afterEnvironment.bufferAllocateSucceeds = allocateSucceeds != 0;
				afterEnvironment.displayServesBuffers = displayServes != 0;

				ScreenState before;
				ScreenState after;
				initPreFix(beforeEnvironment, before, memPassed != 0);
				initFixed(afterEnvironment, after, memPassed != 0);

				char what[192];
				snprintf(what, sizeof(what),
					"both shapes agree over a working stream (memPassed=%d allocate=%d display=%d)",
					memPassed, allocateSucceeds, displayServes);

				const bool sameVideoState =
					(before.videoStream == nullptr) == (after.videoStream == nullptr) &&
					(before.videoBuffer == nullptr) == (after.videoBuffer == nullptr);
				const bool sameScreen =
					before.overlaysInited == after.overlaysInited &&
					before.portraitsAndBiosInited == after.portraitsAndBiosInited &&
					before.progressBarInited == after.progressBarInited &&
					before.piecesActivated == after.piecesActivated &&
					before.videoBufferHandedToWindow == after.videoBufferHandedToWindow &&
					before.ranMovieLoop == after.ranMovieLoop &&
					before.drewHeldFrame == after.drewHeldFrame;
				// The same calls on the stream, in the same number: a guard that skipped a
				// frameDecompress() or an extra close() would change what Windows does.
				const bool sameStreamUse = beforeStream.m_calls == afterStream.m_calls &&
					beforeStream.m_closed == afterStream.m_closed &&
					beforeStream.m_frameIndex == afterStream.m_frameIndex;

				check(sameVideoState && sameScreen && sameStreamUse, what);

				delete before.videoBuffer;
				delete after.videoBuffer;
			}
		}
	}

	if (g_failures)
		printf("\nFAILED: %d assertion(s)\n", g_failures);
	else
		printf("\nOK: the guard changes nothing about a stream that opens\n");
	return g_failures ? 1 : 0;
}

int main(int argc, char** argv)
{
	const char* shape = argc > 1 ? argv[1] : "--shape=fixed";

	if (strcmp(shape, "--shape=fixed") == 0)
		return runFixedOverNullStream();
	if (strcmp(shape, "--shape=pre-fix") == 0)
		return runPreFixOverNullStream();
	if (strcmp(shape, "--shape=working") == 0)
		return compareShapesOverAWorkingStream();

	printf("usage: %s [--shape=fixed|--shape=pre-fix|--shape=working]\n", argv[0]);
	return 2;
}
