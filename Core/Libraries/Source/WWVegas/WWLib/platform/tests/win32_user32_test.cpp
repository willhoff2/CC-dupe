/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2026 TheSuperHackers
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
 *                 Project Name : Westwood Library                                             *
 *                                                                                             *
 *  Behaviour test for the user32 entry points that off Windows are defined over the window       *
 *  seam in platform_win32_user.cpp: the cursor and window-state group -- GetCursorPos(),         *
 *  ScreenToClient(), SetCursor(), IsIconic() -- and the window-sizing and gamma group --         *
 *  GetClientRect(), GetWindowLongA(), AdjustWindowRect(), MonitorFromWindow(),                   *
 *  GetMonitorInfoA(), SetWindowPos(), GetDesktopWindow(), SetDeviceGammaRamp().                  *
 *                                                                                             *
 *  What needs asserting is the translation, not the platform: whether the pointer position      *
 *  comes back in the caller's POINT, whether ScreenToClient() subtracts the client origin in     *
 *  the right direction, whether SetCursor(nullptr) hides the system pointer rather than showing  *
 *  it, and whether IsIconic() reports the seam's minimised flag. Every one of those is an        *
 *  inversion or an off-by-one away from a defect that compiles, links and starts:               *
 *  W3DMouse::draw() does GetCursorPos() then ScreenToClient() and draws the game's cursor at     *
 *  the result, so a sign error puts the cursor in the wrong place and a missed hide leaves two   *
 *  cursors on screen.                                                                           *
 *                                                                                             *
 *  The seam itself is faked here rather than driven: SDL and Cocoa both need a display server,   *
 *  CI has none, and what the real backends do with a screen is their own test's business. The    *
 *  fake records what it was asked and answers with values chosen so that a swap or a sign flip   *
 *  cannot pass -- the origin and the cursor position share no digits, and the coordinates are    *
 *  asymmetric in x and y.                                                                       *
 *                                                                                             *
 *  It also pins the POINTS decision (docs/porting/decisions-resolved.md): the numbers the seam   *
 *  reports must arrive at the caller unscaled. A Retina backing scale is 2 and the CI runner's   *
 *  is 1, so a "helpful" multiply in this layer would be invisible in CI and would put the        *
 *  cursor at double coordinates on real hardware. The check for it is that the arithmetic here    *
 *  is exact.                                                                                    *
 *                                                                                             *
 *  The sizing group is tested the way DX8Wrapper::Resize_And_Position_Window() composes it,      *
 *  because that composition is the real contract: GetClientRect() then GetWindowLong(GWL_STYLE)  *
 *  then AdjustWindowRect() then SetWindowPos(), with GetMonitorInfo(MonitorFromWindow(...)) for  *
 *  the centring. AdjustWindowRect() grows a client rectangle by the frame and SetWindowPos()     *
 *  takes a frame rectangle back to a client area, so if the two disagree by the border the       *
 *  render window comes out the wrong size and the back buffer is stretched -- which compiles,    *
 *  links, and looks almost right. The test asserts the client area that comes out the far end.   *
 *                                                                                             *
 *  Run through scripts/native-win32-user32-test.py. Windows is not the oracle for the fake       *
 *  seam, but it is for the arithmetic, and the expectations below are what user32 documents.     *
 *                                                                                             *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#include "WWLib/platform/platform_cursor.h"
#include "WWLib/platform/platform_window.h"

#include <windows.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

#include <string>
#include <vector>

static int _Failures = 0;
static int _Checks = 0;

static void Check(bool condition, const char * what)
{
	_Checks++;
	if (!condition) {
		_Failures++;
		printf("FAIL: %s\n", what);
	}
}

static void Check_Equal(long got, long expected, const char * what)
{
	_Checks++;
	if (got != expected) {
		_Failures++;
		printf("FAIL: %s: got %ld, expected %ld\n", what, got, expected);
	}
}


/***********************************************************************************************
 *  The fake seam. These are the definitions platform_win32_user.cpp links against; the real      *
 *  SDL2 and Cocoa backends are not in this link, which is what makes the test headless.          *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

namespace
{

// A handle that is not null and is not a real pointer to anything, because nothing in the layer
// under test may dereference it.
HWND const FAKE_WINDOW = reinterpret_cast<HWND>(0x5EA);

struct FakeSeam
{
	void * Current;
	bool Minimised;
	int Cursor_X;
	int Cursor_Y;
	int Origin_X;
	int Origin_Y;
	bool Have_Cursor;
	bool Have_Origin;

	// What SetCursor() did.
	int Show_Calls;
	bool Last_Show;
	void * Last_Show_Window;

	// What the cursor-shape half of the seam saw: every cursor created (a copy of the image, as
	// the real backends take), the destroys, and the selections in order.
	int Create_Calls;
	int Destroy_Calls;
	int Set_Cursor_Calls;
	void * Last_Set_Cursor;
	void * Last_Set_Cursor_Window;
	bool Refuse_Create;
	std::vector<WWPlatform::CursorImage> Created;
	std::vector<std::vector<unsigned char> > Created_Pixels;

	// The client area the seam reports, in points, and whether it knows one yet.
	int Client_Width;
	int Client_Height;

	// The window's own state.
	bool Fullscreen;

	// The decoration thickness the platform reports, and whether it will report it at all.
	bool Have_Insets;
	int Inset_Left;
	int Inset_Top;
	int Inset_Right;
	int Inset_Bottom;

	// The displays. Bounds and work area are deliberately different rectangles, so that a call
	// reading the wrong one of the two cannot pass.
	int Display_Count;
	int Display_Of_Window;

	// What SetWindowPos() did.
	int Set_Position_Calls;
	int Set_Position_X;
	int Set_Position_Y;
	int Set_Size_Calls;
	int Set_Size_Width;
	int Set_Size_Height;
	int Always_On_Top_Calls;
	bool Always_On_Top;
	int Show_Window_Calls;

	FakeSeam()
		: Current(FAKE_WINDOW), Minimised(false), Cursor_X(0), Cursor_Y(0), Origin_X(0),
		  Origin_Y(0), Have_Cursor(true), Have_Origin(true), Show_Calls(0), Last_Show(false),
		  Last_Show_Window(nullptr), Create_Calls(0), Destroy_Calls(0), Set_Cursor_Calls(0),
		  Last_Set_Cursor(nullptr), Last_Set_Cursor_Window(nullptr), Refuse_Create(false),
		  Client_Width(800), Client_Height(600), Fullscreen(false),
		  Have_Insets(true), Inset_Left(3), Inset_Top(28), Inset_Right(3), Inset_Bottom(5),
		  Display_Count(2), Display_Of_Window(0), Set_Position_Calls(0), Set_Position_X(0),
		  Set_Position_Y(0), Set_Size_Calls(0), Set_Size_Width(0), Set_Size_Height(0),
		  Always_On_Top_Calls(0), Always_On_Top(false), Show_Window_Calls(0)
	{
	}
};

/*
**	The fake displays. Display 0 is the primary, 1920x1200 with a 24-point menu bar and a
**	60-point dock, and display 1 sits to its LEFT at a negative x, which is legal on both
**	Windows and macOS and is where an implementation that assumes non-negative screen space
**	breaks.
*/
struct FakeDisplay
{
	int Bounds_X, Bounds_Y, Bounds_W, Bounds_H;
	int Work_X, Work_Y, Work_W, Work_H;
};

const FakeDisplay FAKE_DISPLAYS[2] = {
	{ 0, 0, 1920, 1200, 0, 24, 1920, 1116 },
	{ -1280, 0, 1280, 1024, -1280, 0, 1280, 1024 },
};

FakeSeam TheSeam;

}	// anonymous namespace

namespace WWPlatform
{

void * Window_Current()
{
	return TheSeam.Current;
}

bool Window_Is_Minimised(void * window)
{
	return window == TheSeam.Current && TheSeam.Minimised;
}

bool Window_Cursor_Position(void * window, int & x, int & y)
{
	if (window != TheSeam.Current || window == nullptr || !TheSeam.Have_Cursor) return false;
	x = TheSeam.Cursor_X;
	y = TheSeam.Cursor_Y;
	return true;
}

bool Window_Client_Origin(void * window, int & x, int & y)
{
	if (window != TheSeam.Current || window == nullptr || !TheSeam.Have_Origin) return false;
	x = TheSeam.Origin_X;
	y = TheSeam.Origin_Y;
	return true;
}

void Window_Show_System_Cursor(void * window, bool show)
{
	TheSeam.Show_Calls++;
	TheSeam.Last_Show = show;
	TheSeam.Last_Show_Window = window;
}

void * Window_Create_Cursor(const CursorImage & image)
{
	TheSeam.Create_Calls++;
	if (TheSeam.Refuse_Create || image.Pixels_BGRA == nullptr || image.Width <= 0 ||
		image.Height <= 0) {
		return nullptr;
	}
	TheSeam.Created.push_back(image);
	TheSeam.Created_Pixels.push_back(std::vector<unsigned char>(image.Pixels_BGRA,
		image.Pixels_BGRA + static_cast<size_t>(image.Width) * image.Height * 4));
	// The handle is the 1-based index into Created, so a null handle stays "no cursor".
	return reinterpret_cast<void *>(static_cast<uintptr_t>(TheSeam.Created.size()));
}

void Window_Destroy_Cursor(void * cursor)
{
	if (cursor == nullptr) return;
	TheSeam.Destroy_Calls++;
}

void Window_Set_Cursor(void * window, void * cursor)
{
	TheSeam.Set_Cursor_Calls++;
	TheSeam.Last_Set_Cursor = cursor;
	TheSeam.Last_Set_Cursor_Window = window;
}

const char * Window_Last_Error()
{
	return "fake seam refused";
}

void Window_Client_Size(void * window, int & width, int & height)
{
	/*
	**	The real seam reports zeros for a window it does not know, which is what GetClientRect()
	**	turns into its documented failure.
	*/
	if (window != TheSeam.Current || window == nullptr) {
		width = 0;
		height = 0;
		return;
	}
	width = TheSeam.Client_Width;
	height = TheSeam.Client_Height;
}

bool Window_Is_Fullscreen(void * window)
{
	return window == TheSeam.Current && window != nullptr && TheSeam.Fullscreen;
}

bool Window_Frame_Insets(void * window, int & left, int & top, int & right, int & bottom)
{
	if (window != TheSeam.Current || window == nullptr) return false;
	if (!TheSeam.Have_Insets) return false;
	if (TheSeam.Fullscreen) {
		left = 0;
		top = 0;
		right = 0;
		bottom = 0;
		return true;
	}
	left = TheSeam.Inset_Left;
	top = TheSeam.Inset_Top;
	right = TheSeam.Inset_Right;
	bottom = TheSeam.Inset_Bottom;
	return true;
}

int Window_Display_Count()
{
	return TheSeam.Display_Count;
}

int Window_Display_For_Window(void * window)
{
	if (window != TheSeam.Current || window == nullptr) return 0;
	return TheSeam.Display_Of_Window;
}

bool Window_Display_Bounds(int display, int & x, int & y, int & width, int & height)
{
	if (display < 0 || display >= TheSeam.Display_Count) return false;
	x = FAKE_DISPLAYS[display].Bounds_X;
	y = FAKE_DISPLAYS[display].Bounds_Y;
	width = FAKE_DISPLAYS[display].Bounds_W;
	height = FAKE_DISPLAYS[display].Bounds_H;
	return true;
}

bool Window_Display_Work_Area(int display, int & x, int & y, int & width, int & height)
{
	if (display < 0 || display >= TheSeam.Display_Count) return false;
	x = FAKE_DISPLAYS[display].Work_X;
	y = FAKE_DISPLAYS[display].Work_Y;
	width = FAKE_DISPLAYS[display].Work_W;
	height = FAKE_DISPLAYS[display].Work_H;
	return true;
}

bool Window_Set_Position(void * window, int x, int y)
{
	if (window != TheSeam.Current || window == nullptr) return false;
	TheSeam.Set_Position_Calls++;
	TheSeam.Set_Position_X = x;
	TheSeam.Set_Position_Y = y;
	return true;
}

bool Window_Set_Client_Size(void * window, int width, int height)
{
	if (window != TheSeam.Current || window == nullptr) return false;
	if (width <= 0 || height <= 0) return false;
	TheSeam.Set_Size_Calls++;
	TheSeam.Set_Size_Width = width;
	TheSeam.Set_Size_Height = height;
	TheSeam.Client_Width = width;
	TheSeam.Client_Height = height;
	return true;
}

void Window_Set_Always_On_Top(void * window, bool on_top)
{
	(void)window;
	TheSeam.Always_On_Top_Calls++;
	TheSeam.Always_On_Top = on_top;
}

void Window_Show(void * window, bool show)
{
	(void)window;
	(void)show;
	TheSeam.Show_Window_Calls++;
}

}	// namespace WWPlatform


/***********************************************************************************************
 *  GetCursorPos()                                                                               *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static void Test_Get_Cursor_Pos()
{
	TheSeam = FakeSeam();
	TheSeam.Cursor_X = 731;
	TheSeam.Cursor_Y = 409;

	POINT point = { -1, -1 };
	Check(GetCursorPos(&point) != FALSE, "GetCursorPos() succeeds while there is a window");
	Check_Equal(point.x, 731, "GetCursorPos() reports the seam's x, unscaled and in points");
	Check_Equal(point.y, 409, "GetCursorPos() reports the seam's y, unscaled and in points");

	// A negative screen coordinate is legal on Windows -- a second monitor left of the primary --
	// and must survive rather than being clamped.
	TheSeam.Cursor_X = -120;
	TheSeam.Cursor_Y = -7;
	Check(GetCursorPos(&point) != FALSE, "GetCursorPos() succeeds off the primary display");
	Check_Equal(point.x, -120, "a negative screen x is not clamped");
	Check_Equal(point.y, -7, "a negative screen y is not clamped");

	// No window: the documented failure, and the caller's POINT left alone rather than zeroed,
	// because W3DWaterTracks.cpp keeps using its own point when the call fails.
	TheSeam.Current = nullptr;
	point.x = 42;
	point.y = 43;
	Check(GetCursorPos(&point) == FALSE, "GetCursorPos() fails when there is no window");
	Check_Equal(point.x, 42, "a failed GetCursorPos() leaves the caller's x alone");
	Check_Equal(point.y, 43, "a failed GetCursorPos() leaves the caller's y alone");

	// A null POINT is a failure and not a crash; Win32 fails it too.
	TheSeam.Current = FAKE_WINDOW;
	Check(GetCursorPos(nullptr) == FALSE, "GetCursorPos(nullptr) fails without dereferencing it");
}


/***********************************************************************************************
 *  ScreenToClient()                                                                             *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static void Test_Screen_To_Client()
{
	TheSeam = FakeSeam();
	TheSeam.Origin_X = 100;
	TheSeam.Origin_Y = 250;

	// The direction: screen minus origin. The reverse would be ClientToScreen().
	POINT point = { 731, 409 };
	Check(ScreenToClient(FAKE_WINDOW, &point) != FALSE, "ScreenToClient() succeeds");
	Check_Equal(point.x, 631, "ScreenToClient() subtracts the client origin's x");
	Check_Equal(point.y, 159, "ScreenToClient() subtracts the client origin's y");

	// The client origin itself maps to (0, 0), which is the case a swapped subtraction gets right
	// only when x and y are equal -- they are not here.
	point.x = 100;
	point.y = 250;
	Check(ScreenToClient(FAKE_WINDOW, &point) != FALSE, "ScreenToClient() succeeds at the origin");
	Check_Equal(point.x, 0, "the client area's top-left corner is client (0, 0) in x");
	Check_Equal(point.y, 0, "the client area's top-left corner is client (0, 0) in y");

	// A point above and left of the window gives negative client coordinates, which is what
	// Win32Mouse's inside/outside test relies on.
	point.x = 90;
	point.y = 200;
	Check(ScreenToClient(FAKE_WINDOW, &point) != FALSE, "ScreenToClient() succeeds outside");
	Check_Equal(point.x, -10, "a point left of the client area is negative in x");
	Check_Equal(point.y, -50, "a point above the client area is negative in y");

	// A window at the screen origin must not shift anything: this is the configuration a
	// fullscreen game runs in, and it is the one where a wrong sign hides.
	TheSeam.Origin_X = 0;
	TheSeam.Origin_Y = 0;
	point.x = 17;
	point.y = 923;
	Check(ScreenToClient(FAKE_WINDOW, &point) != FALSE, "ScreenToClient() succeeds fullscreen");
	Check_Equal(point.x, 17, "a client area at the screen origin leaves x unchanged");
	Check_Equal(point.y, 923, "a client area at the screen origin leaves y unchanged");

	// Failures: no window, a stale handle, and a null POINT.
	point.x = 5;
	point.y = 6;
	Check(ScreenToClient(nullptr, &point) == FALSE, "ScreenToClient(nullptr) fails");
	Check_Equal(point.x, 5, "a failed ScreenToClient() leaves the caller's point alone");
	Check_Equal(point.y, 6, "a failed ScreenToClient() leaves the caller's y alone");
	Check(ScreenToClient(reinterpret_cast<HWND>(0xDEAD), &point) == FALSE,
		"ScreenToClient() with a handle that is not the window fails");
	Check(ScreenToClient(FAKE_WINDOW, nullptr) == FALSE,
		"ScreenToClient() with a null POINT fails without dereferencing it");
}


/***********************************************************************************************
 *  The pair, as W3DMouse::draw() uses them                                                      *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static void Test_Cursor_Round_Trip()
{
	/*
	**	W3DMouse.cpp:
	**	    GetCursorPos(&ptCursor);
	**	    ScreenToClient(ApplicationHWnd, &ptCursor);
	**	and then draws at ptCursor. With the pointer at a known place inside a known client area
	**	there is exactly one right answer, and it is in points at both ends.
	*/
	TheSeam = FakeSeam();
	TheSeam.Origin_X = 64;
	TheSeam.Origin_Y = 32;
	TheSeam.Cursor_X = 64 + 400;
	TheSeam.Cursor_Y = 32 + 300;

	POINT point = { 0, 0 };
	Check(GetCursorPos(&point) != FALSE, "the round trip's GetCursorPos() succeeds");
	Check(ScreenToClient(FAKE_WINDOW, &point) != FALSE, "the round trip's ScreenToClient() succeeds");
	Check_Equal(point.x, 400, "a pointer 400 points into the client area lands at client x 400");
	Check_Equal(point.y, 300, "a pointer 300 points into the client area lands at client y 300");
}


/***********************************************************************************************
 *  Synthetic .CUR / .ANI fixtures. Built byte by byte from the RIFF ACON and ICO layouts, so   *
 *  that the parser is tested against the formats' definitions rather than against itself.       *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

typedef std::vector<unsigned char> Bytes;

static void Put_U16(Bytes & out, unsigned value)
{
	out.push_back(static_cast<unsigned char>(value & 0xFF));
	out.push_back(static_cast<unsigned char>((value >> 8) & 0xFF));
}

static void Put_U32(Bytes & out, unsigned long value)
{
	Put_U16(out, static_cast<unsigned>(value & 0xFFFF));
	Put_U16(out, static_cast<unsigned>((value >> 16) & 0xFFFF));
}

static void Put_Tag(Bytes & out, const char * tag)
{
	out.insert(out.end(), tag, tag + 4);
}

static void Put_Chunk(Bytes & out, const char * tag, const Bytes & body)
{
	Put_Tag(out, tag);
	Put_U32(out, body.size());
	out.insert(out.end(), body.begin(), body.end());
	if (body.size() & 1) out.push_back(0);		// RIFF chunks are word aligned
}

/*
**	A width x height .CUR with one BI_RGB image whose pixel (x, y) is BGR (x, y, 0x80), except
**	the top row, which is black, alpha 0 (at 32 bpp) and set in the AND mask: the way a real
**	cursor spells a transparent pixel, so both the alpha route and the mask route decode to the
**	same picture. `bpp` 24 drops the alpha channel so the mask is the only transparency.
*/
static Bytes Make_Cur(int width, int height, int hot_x, int hot_y, int bpp)
{
	Bytes image;
	const int header_size = 40;
	const int xor_stride = ((width * bpp + 31) / 32) * 4;
	const int and_stride = ((width + 31) / 32) * 4;
	const int image_size = header_size + xor_stride * height + and_stride * height;

	Put_U32(image, header_size);
	Put_U32(image, width);
	Put_U32(image, height * 2);				// XOR and AND stacked
	Put_U16(image, 1);
	Put_U16(image, bpp);
	Put_U32(image, 0);							// BI_RGB
	Put_U32(image, 0);							// biSizeImage may be 0 for BI_RGB
	Put_U32(image, 0); Put_U32(image, 0); Put_U32(image, 0); Put_U32(image, 0);

	for (int row = height - 1; row >= 0; --row) {			// bottom-up
		Bytes line;
		for (int x = 0; x < width; ++x) {
			line.push_back(row == 0 ? 0 : static_cast<unsigned char>(x));	// B
			line.push_back(row == 0 ? 0 : static_cast<unsigned char>(row));	// G
			line.push_back(row == 0 ? 0 : 0x80);							// R
			if (bpp == 32) line.push_back(row == 0 ? 0x00 : 0xFF);			// A
		}
		line.resize(xor_stride, 0);
		image.insert(image.end(), line.begin(), line.end());
	}
	for (int row = height - 1; row >= 0; --row) {
		Bytes line(and_stride, 0);
		if (row == 0) line.assign(and_stride, 0xFF);				// row 0: transparent (1 bits)
		image.insert(image.end(), line.begin(), line.end());
	}

	Bytes cur;
	Put_U16(cur, 0);				// reserved
	Put_U16(cur, 2);				// type: cursor
	Put_U16(cur, 1);				// one image
	cur.push_back(static_cast<unsigned char>(width));
	cur.push_back(static_cast<unsigned char>(height));
	cur.push_back(0);				// colour count
	cur.push_back(0);				// reserved
	Put_U16(cur, hot_x);
	Put_U16(cur, hot_y);
	Put_U32(cur, image_size);
	Put_U32(cur, 6 + 16);			// offset of the image
	cur.insert(cur.end(), image.begin(), image.end());
	return cur;
}

/*
**	A RIFF ACON around `frames` .CUR images, each with a different hotspot so that the test can
**	tell which frame was decoded. With `sequence`, a 'seq ' chunk lists the frames in reverse
**	order, so step 0 is the LAST frame, and there are frames+1 steps.
*/
static Bytes Make_Ani(int frames, bool sequence, int width, int height, int rate)
{
	Bytes anih;
	Put_U32(anih, 36);
	Put_U32(anih, frames);
	Put_U32(anih, sequence ? frames + 1 : frames);
	Put_U32(anih, 0); Put_U32(anih, 0); Put_U32(anih, 0); Put_U32(anih, 0);
	Put_U32(anih, rate);
	Put_U32(anih, 1 | (sequence ? 2 : 0));		// AF_ICON | AF_SEQUENCE

	Bytes fram;
	Put_Tag(fram, "fram");
	for (int i = 0; i < frames; ++i) {
		Put_Chunk(fram, "icon", Make_Cur(width, height, 1 + i, 2 + i, 32));
	}

	Bytes body;
	Put_Tag(body, "ACON");
	Put_Chunk(body, "anih", anih);
	if (sequence) {
		Bytes seq;
		for (int i = 0; i < frames + 1; ++i) Put_U32(seq, ((frames - 1 - i) % frames + frames) % frames);
		Put_Chunk(body, "seq ", seq);
	}
	Put_Chunk(body, "LIST", fram);

	Bytes riff;
	Put_Chunk(riff, "RIFF", body);
	return riff;
}

static std::string Write_Temp(const char * name, const Bytes & bytes)
{
	const char * dir = getenv("TMPDIR");
	std::string path = std::string(dir != nullptr && *dir ? dir : "/tmp") + "/" + name;
	FILE * file = fopen(path.c_str(), "wb");
	if (file != nullptr) {
		fwrite(bytes.data(), 1, bytes.size(), file);
		fclose(file);
	}
	return path;
}


/***********************************************************************************************
 *  Cursor_Decode()                                                                              *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static void Test_Cursor_Decode()
{
	std::string error;
	WWPlatform::CursorFile file;

	// A bare .CUR: 32x32, hotspot (5, 7), 32-bit with alpha.
	Bytes cur = Make_Cur(32, 32, 5, 7, 32);
	Check(WWPlatform::Cursor_Decode(cur.data(), cur.size(), file, error),
		"a bare .CUR decodes");
	Check_Equal(file.Frame_Count, 1, ".CUR: one frame");
	Check_Equal(file.Step_Count, 1, ".CUR: one step");
	Check_Equal(file.First.Width, 32, ".CUR: width");
	Check_Equal(file.First.Height, 32, ".CUR: height is the XOR half, not the doubled DIB height");
	Check_Equal(file.First.Hotspot_X, 5, ".CUR: hotspot x from the directory entry");
	Check_Equal(file.First.Hotspot_Y, 7, ".CUR: hotspot y from the directory entry");
	Check_Equal(file.First.Bits_Per_Pixel, 32, ".CUR: source depth reported");
	Check_Equal(static_cast<long>(file.First.Pixels_BGRA.size()), 32 * 32 * 4,
		".CUR: one BGRA quad per pixel");
	if (file.First.Pixels_BGRA.size() == 32 * 32 * 4) {
		const unsigned char * px = file.First.Pixels_BGRA.data();
		// (x=3, y=0) is on the transparent top row; (x=3, y=9) is opaque. Rows come out top-down.
		Check_Equal(px[3 * 4 + 3], 0x00, ".CUR: top row is transparent");
		Check_Equal(px[(9 * 32 + 3) * 4 + 0], 3, ".CUR: blue = x, so rows were un-flipped");
		Check_Equal(px[(9 * 32 + 3) * 4 + 1], 9, ".CUR: green = y, so rows were un-flipped");
		Check_Equal(px[(9 * 32 + 3) * 4 + 2], 0x80, ".CUR: red");
		Check_Equal(px[(9 * 32 + 3) * 4 + 3], 0xFF, ".CUR: opaque below the top row");
	}

	// 24-bit: no alpha channel, so transparency comes from the AND mask alone.
	Bytes cur24 = Make_Cur(16, 16, 0, 15, 24);
	Check(WWPlatform::Cursor_Decode(cur24.data(), cur24.size(), file, error),
		"a 24-bit .CUR decodes");
	Check_Equal(file.First.Bits_Per_Pixel, 24, "24-bit: source depth reported");
	Check_Equal(file.First.Hotspot_Y, 15, "24-bit: hotspot on the last row is in range");
	if (file.First.Pixels_BGRA.size() == 16 * 16 * 4) {
		Check_Equal(file.First.Pixels_BGRA[3], 0x00, "24-bit: AND mask makes the top row transparent");
		Check_Equal(file.First.Pixels_BGRA[(16 + 0) * 4 + 3], 0xFF, "24-bit: mask leaves row 1 opaque");
	}

	// An .ANI: 3 frames, no sequence -> the first frame is frame 0 (hotspot (1, 2)).
	Bytes ani = Make_Ani(3, false, 32, 32, 6);
	Check(WWPlatform::Cursor_Decode(ani.data(), ani.size(), file, error),
		"a RIFF ACON with 3 frames decodes");
	if (!error.empty()) printf("      error: %s\n", error.c_str());
	Check_Equal(file.Frame_Count, 3, ".ANI: cFrames");
	Check_Equal(file.Step_Count, 3, ".ANI: cSteps equals cFrames without a sequence");
	Check_Equal(file.Display_Rate_Jiffies, 6, ".ANI: JifRate");
	Check_Equal(file.First.Width, 32, ".ANI: first frame width");
	Check_Equal(file.First.Height, 32, ".ANI: first frame height");
	Check_Equal(file.First.Hotspot_X, 1, ".ANI: first frame is frame 0 (hotspot x)");
	Check_Equal(file.First.Hotspot_Y, 2, ".ANI: first frame is frame 0 (hotspot y)");

	// With a sequence, step 0 names the LAST frame, hotspot (3, 4).
	Bytes seq = Make_Ani(3, true, 24, 24, 10);
	Check(WWPlatform::Cursor_Decode(seq.data(), seq.size(), file, error),
		"a RIFF ACON with a 'seq ' chunk decodes");
	Check_Equal(file.Frame_Count, 3, ".ANI seq: cFrames");
	Check_Equal(file.Step_Count, 4, ".ANI seq: cSteps is the sequence length");
	Check_Equal(file.First.Width, 24, ".ANI seq: width");
	Check_Equal(file.First.Hotspot_X, 3, ".ANI seq: the first STEP's frame is shown (hotspot x)");
	Check_Equal(file.First.Hotspot_Y, 4, ".ANI seq: the first STEP's frame is shown (hotspot y)");

	// Rejections: must say why, and must never claim a frame.
	Check(!WWPlatform::Cursor_Decode(nullptr, 0, file, error) && !error.empty(),
		"empty input is rejected with a reason");
	Bytes junk(64, 0x5A);
	Check(!WWPlatform::Cursor_Decode(junk.data(), junk.size(), file, error) && !error.empty(),
		"junk is rejected with a reason");
	Bytes truncated(ani.begin(), ani.begin() + ani.size() / 2);
	Check(!WWPlatform::Cursor_Decode(truncated.data(), truncated.size(), file, error),
		"a truncated .ANI is rejected rather than read past its end");
	Bytes wave = ani;
	memcpy(&wave[8], "WAVE", 4);
	Check(!WWPlatform::Cursor_Decode(wave.data(), wave.size(), file, error),
		"a RIFF that is not ACON is rejected");
}


/***********************************************************************************************
 *  SetCursor() / LoadCursorFromFile() / DestroyCursor()                                         *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static void Test_Set_Cursor()
{
	TheSeam = FakeSeam();

	/*
	**	The call the engine makes for NONE or an invisible mouse: SetCursor(nullptr) means "no
	**	cursor", so the system pointer must be hidden. Showing it instead is the two-cursors
	**	defect, and it is a `!` away.
	*/
	Check(SetCursor(nullptr) == nullptr, "the first SetCursor() reports no previous cursor");
	Check_Equal(TheSeam.Show_Calls, 1, "SetCursor() goes through the seam rather than doing nothing");
	Check(TheSeam.Last_Show == false, "SetCursor(nullptr) HIDES the system pointer");
	Check(TheSeam.Last_Show_Window == FAKE_WINDOW,
		"SetCursor() addresses the seam's own window");
	Check_Equal(TheSeam.Set_Cursor_Calls, 0, "SetCursor(nullptr) does not change the shape");

	/*
	**	Win32Mouse::initCursorResources() loads "data\cursors\Name.ANI". Two distinguishable
	**	cursors, so that the test can tell which one the seam was handed.
	*/
	std::string attack_path = Write_Temp("SCCAttack.ANI", Make_Ani(2, false, 32, 32, 4));
	std::string move_path = Write_Temp("SCCMove.ANI", Make_Ani(1, false, 24, 24, 4));
	HCURSOR attack = LoadCursorFromFile(attack_path.c_str());
	HCURSOR move = LoadCursorFromFile(move_path.c_str());
	Check(attack != nullptr && move != nullptr, "LoadCursorFromFile() returns handles");
	Check(attack != move, "distinct files give distinct handles");
	Check_Equal(TheSeam.Create_Calls, 2, "each load creates one backend cursor");
	Check_Equal(static_cast<long>(TheSeam.Created.size()), 2, "both were accepted by the seam");
	if (TheSeam.Created.size() == 2) {
		Check_Equal(TheSeam.Created[0].Width, 32, "attack: the frame's width reached the seam");
		Check_Equal(TheSeam.Created[0].Hotspot_X, 1, "attack: the first frame's hotspot x");
		Check_Equal(TheSeam.Created[0].Hotspot_Y, 2, "attack: the first frame's hotspot y");
		Check_Equal(TheSeam.Created[1].Width, 24, "move: the frame's width reached the seam");
		Check_Equal(TheSeam.Created_Pixels[1][(9 * 24 + 3) * 4 + 1], 9,
			"move: decoded top-down BGRA pixels reached the seam");
	}

	// The call the engine makes every frame the cursor is visible.
	Check(SetCursor(attack) == nullptr, "SetCursor() returns the previous cursor, which was null");
	Check_Equal(TheSeam.Show_Calls, 2, "a non-null cursor still reaches the visibility seam");
	Check(TheSeam.Last_Show == true, "a non-null cursor shows the pointer");
	Check_Equal(TheSeam.Set_Cursor_Calls, 1, "and selects a shape through the seam");
	Check(TheSeam.Last_Set_Cursor == reinterpret_cast<void *>(1),
		"the shape selected is the one LoadCursorFromFile() created for that handle");
	Check(TheSeam.Last_Set_Cursor_Window == FAKE_WINDOW, "on the seam's own window");

	Check(SetCursor(attack) == attack, "re-selecting the same cursor returns it as the previous");
	Check_Equal(TheSeam.Set_Cursor_Calls, 1, "re-selecting the current shape is not a seam call");

	Check(SetCursor(move) == attack, "switching cursors returns the previous one");
	Check_Equal(TheSeam.Set_Cursor_Calls, 2, "switching selects the new shape");
	Check(TheSeam.Last_Set_Cursor == reinterpret_cast<void *>(2), "and it is the OTHER cursor");

	Check(SetCursor(nullptr) == move, "SetCursor() returns the handle set before it");
	Check(TheSeam.Last_Show == false, "back to nullptr hides the pointer again");
	Check_Equal(TheSeam.Set_Cursor_Calls, 2, "hiding does not touch the shape");

	Check(SetCursor(move) == nullptr, "showing again returns null as the previous");
	Check_Equal(TheSeam.Set_Cursor_Calls, 2,
		"the shape was still selected while hidden, so showing again needs no reselection");
	Check(TheSeam.Last_Show == true, "and the pointer is shown");

	/*
	**	MISSING DATA at runtime: the retail Data\Cursors\*.ANI is not in the .big archives, so a
	**	native install may well lack it. The load must not fail (null would make setCursor()
	**	hide the pointer) and selecting the handle must fall back to the platform's arrow, which
	**	the seam spells as Window_Set_Cursor(window, nullptr).
	*/
	HCURSOR missing = LoadCursorFromFile("data\\cursors\\NoSuchCursor.ANI");
	Check(missing != nullptr, "a missing .ANI still yields a handle");
	Check_Equal(TheSeam.Create_Calls, 2, "a missing file creates no backend cursor");
	SetCursor(missing);
	Check_Equal(TheSeam.Set_Cursor_Calls, 3, "selecting the missing cursor reaches the seam");
	Check(TheSeam.Last_Set_Cursor == nullptr, "as the default arrow, not as garbage");
	Check(TheSeam.Last_Show == true, "and the pointer is VISIBLE: an arrow, not nothing");

	// A file that exists but is not a cursor behaves the same way.
	std::string junk_path = Write_Temp("Junk.ANI", Bytes(100, 0x42));
	HCURSOR junk = LoadCursorFromFile(junk_path.c_str());
	Check(junk != nullptr, "an undecodable .ANI still yields a handle");
	Check_Equal(TheSeam.Create_Calls, 2, "an undecodable file creates no backend cursor");

	// And so does a backend that refuses (no display, out of memory).
	TheSeam.Refuse_Create = true;
	HCURSOR refused = LoadCursorFromFile(attack_path.c_str());
	TheSeam.Refuse_Create = false;
	Check(refused != nullptr, "a backend refusal still yields a handle");
	SetCursor(refused);
	Check(TheSeam.Last_Set_Cursor == nullptr, "which selects the default arrow");

	// Destroying the current cursor must not leave a dangling selection in the compat layer.
	SetCursor(attack);
	Check(TheSeam.Last_Set_Cursor == reinterpret_cast<void *>(1), "attack reselected");
	Check(DestroyCursor(attack) == TRUE, "DestroyCursor() accepts a live handle");
	Check_Equal(TheSeam.Destroy_Calls, 1, "and destroys the backend cursor");
	Check(DestroyCursor(nullptr) == FALSE, "DestroyCursor(nullptr) fails as Win32 does");
	Check(DestroyCursor(move) == TRUE && DestroyCursor(missing) == TRUE &&
		DestroyCursor(junk) == TRUE && DestroyCursor(refused) == TRUE,
		"handles without a backend cursor destroy cleanly");
	Check_Equal(TheSeam.Destroy_Calls, 2, "only the two real backend cursors were destroyed");

	remove(attack_path.c_str());
	remove(move_path.c_str());
	remove(junk_path.c_str());

	// Before the window exists the call must still be safe; the seam is what rejects the null
	// window, and it is reached with it rather than being second-guessed here.
	TheSeam.Current = nullptr;
	TheSeam.Show_Calls = 0;
	SetCursor(nullptr);
	Check_Equal(TheSeam.Show_Calls, 1, "SetCursor() before the window exists still calls the seam");
	Check(TheSeam.Last_Show_Window == nullptr, "and passes the null window through unchanged");
}


/***********************************************************************************************
 *  The retail cursor set. Win32Mouse.cpp names these in its table and loads                     *
 *  data\cursors\<Name>.ANI (plus <Name>0..7 for the 8-direction Scroll cursor) relative to the *
 *  game directory. The files ship LOOSE with the retail installer -- they are not in any .big   *
 *  -- so this test reads them from $GENERALSMD_PATH/Data/Cursors or ./Data/Cursors, and when   *
 *  neither exists it SKIPS, saying so, rather than failing or inventing a result.               *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static const char * const RETAIL_CURSOR_NAMES[] = {
	"SCCPointer", "SCCNoAction", "SCCSelect", "SCCMove", "SCCAttMov", "SCCAttack", "SCCEnter",
	"SCCExit", "SCCFriendly", "SCCHostile", "SCCHostile2", "SCCHostile3", "SCCKnifeAttack",
	"SCCNoBomb", "SCCNoKnife", "SCCPlaceBeacon", "SCCRallyPnt", "SCCRemoteChg", "SCCRepair",
	"SCCResumeC", "SCCSDIUplink", "SCCSniper", "SCCTNTAttack", "SCCTimedChg", "SCCWaypoint",
	"SCCCashHack",
};

static bool Directory_Exists(const std::string & path)
{
	struct stat info;
	return stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

static bool Read_File(const std::string & path, Bytes & out)
{
	FILE * file = fopen(path.c_str(), "rb");
	if (file == nullptr) return false;
	unsigned char chunk[4096];
	size_t got;
	while ((got = fread(chunk, 1, sizeof(chunk), file)) > 0) out.insert(out.end(), chunk, chunk + got);
	fclose(file);
	return true;
}

// Case-insensitive lookup of <name>.ani in a directory, since the retail files are mixed case
// and the game's spelling ("data\cursors\Name.ANI") is not necessarily the disk's.
static std::string Find_Cursor_File(const std::string & dir, const std::string & name)
{
	DIR * handle = opendir(dir.c_str());
	if (handle == nullptr) return "";
	std::string want = name + ".ani";
	std::string found;
	while (struct dirent * entry = readdir(handle)) {
		if (strcasecmp(entry->d_name, want.c_str()) == 0) {
			found = dir + "/" + entry->d_name;
			break;
		}
	}
	closedir(handle);
	return found;
}

static void Test_Retail_Cursor_Set()
{
	std::string dir;
	const char * install = getenv("GENERALSMD_PATH");
	if (install != nullptr && *install && Directory_Exists(std::string(install) + "/Data/Cursors")) {
		dir = std::string(install) + "/Data/Cursors";
	} else if (Directory_Exists("Data/Cursors")) {
		dir = "Data/Cursors";
	}

	if (dir.empty()) {
		printf("SKIP: retail cursor set: no Data/Cursors directory (checked $GENERALSMD_PATH/Data/"
			"Cursors and ./Data/Cursors). The retail .ANI cursors ship loose with the installer and "
			"are not in the .big archives, so this run cannot measure them; the synthetic fixtures "
			"above are the evidence for the parser. Set GENERALSMD_PATH to a retail install to run "
			"it.\n");
		return;
	}

	printf("retail cursor set from %s:\n", dir.c_str());
	const int names = static_cast<int>(sizeof(RETAIL_CURSOR_NAMES) / sizeof(RETAIL_CURSOR_NAMES[0]));
	for (int i = 0; i < names + 8; ++i) {
		std::string name = i < names ? RETAIL_CURSOR_NAMES[i]
			: "SCCScroll" + std::string(1, static_cast<char>('0' + (i - names)));
		std::string path = Find_Cursor_File(dir, name);
		Check(!path.empty(), (name + ".ANI is present").c_str());
		if (path.empty()) continue;

		Bytes bytes;
		std::string error;
		WWPlatform::CursorFile file;
		Check(Read_File(path, bytes), (name + ".ANI is readable").c_str());
		bool ok = WWPlatform::Cursor_Decode(bytes.data(), bytes.size(), file, error);
		Check(ok, (name + ".ANI decodes: " + error).c_str());
		if (!ok) continue;
		printf("  %-16s %2dx%-2d hotspot (%2d,%2d) frames %2d steps %2d rate %d bpp %d\n",
			name.c_str(), file.First.Width, file.First.Height, file.First.Hotspot_X,
			file.First.Hotspot_Y, file.Frame_Count, file.Step_Count, file.Display_Rate_Jiffies,
			file.First.Bits_Per_Pixel);
		Check(file.First.Width > 0 && file.First.Width <= 256 && file.First.Height > 0 &&
			file.First.Height <= 256, (name + ": frame size is a cursor's").c_str());
		Check(file.First.Hotspot_X >= 0 && file.First.Hotspot_X < file.First.Width &&
			file.First.Hotspot_Y >= 0 && file.First.Hotspot_Y < file.First.Height,
			(name + ": hotspot lies inside the frame").c_str());
		Check(file.Frame_Count >= 1, (name + ": at least one frame").c_str());
	}
}


/***********************************************************************************************
 *  IsIconic()                                                                                   *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static void Test_Is_Iconic()
{
	TheSeam = FakeSeam();

	TheSeam.Minimised = false;
	Check(IsIconic(FAKE_WINDOW) == FALSE, "IsIconic() is false for a window that is not minimised");

	TheSeam.Minimised = true;
	Check(IsIconic(FAKE_WINDOW) != FALSE, "IsIconic() is true for a minimised window");

	/*
	**	W3DDisplay::draw() and Win32GameEngine::update() both do `if (ApplicationHWnd &&
	**	IsIconic(ApplicationHWnd))`, so a null handle reaching here is not the engine's normal
	**	path -- but Win32 answers false for a handle it cannot find, and answering true would
	**	park the game in the minimised branch forever.
	*/
	Check(IsIconic(nullptr) == FALSE, "IsIconic(nullptr) is false, not true");
	Check(IsIconic(reinterpret_cast<HWND>(0xDEAD)) == FALSE,
		"IsIconic() with a handle that is not the window is false");
}


/***********************************************************************************************
 *  GetClientRect()                                                                              *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static void Test_Get_Client_Rect()
{
	TheSeam = FakeSeam();
	TheSeam.Client_Width = 1024;
	TheSeam.Client_Height = 768;

	RECT rect = { -1, -1, -1, -1 };
	Check(GetClientRect(FAKE_WINDOW, &rect) != FALSE, "GetClientRect() succeeds for the window");
	Check_Equal(rect.left, 0, "GetClientRect() puts the client origin at left 0");
	Check_Equal(rect.top, 0, "GetClientRect() puts the client origin at top 0");
	Check_Equal(rect.right, 1024, "right is the client WIDTH, in points, not the screen x");
	Check_Equal(rect.bottom, 768, "bottom is the client HEIGHT, in points, not the screen y");

	/*
	**	The points/pixels trap. DX8Wrapper::Resize_And_Position_Window() compares
	**	(right - left) against ResolutionWidth and resizes when they differ, so if this returned
	**	the Retina backing size the window would be resized every time it was asked -- on real
	**	hardware only, since the CI runner's backing scale is 1. The assertion is that the number
	**	is the seam's client size exactly.
	*/
	TheSeam.Client_Width = 800;
	TheSeam.Client_Height = 600;
	Check(GetClientRect(FAKE_WINDOW, &rect) != FALSE, "GetClientRect() succeeds again");
	Check_Equal(rect.right - rect.left, 800, "the width is the point width, never doubled");
	Check_Equal(rect.bottom - rect.top, 600, "the height is the point height, never doubled");

	Check(GetClientRect(FAKE_WINDOW, nullptr) == FALSE, "GetClientRect(nullptr rect) fails");
	Check(GetClientRect(nullptr, &rect) == FALSE, "GetClientRect() with no window fails");
	Check(GetClientRect(reinterpret_cast<HWND>(0xDEAD), &rect) == FALSE,
		"GetClientRect() with a handle that is not the window fails");
}


/***********************************************************************************************
 *  GetWindowLongA()                                                                             *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static void Test_Get_Window_Long()
{
	TheSeam = FakeSeam();

	/*
	**	A windowed device: the style must carry the frame bits, because AdjustWindowRect() reads
	**	them to decide whether to add a border at all, and Debug.cpp tests WS_CAPTION.
	*/
	TheSeam.Fullscreen = false;
	LONG style = GetWindowLongA(FAKE_WINDOW, GWL_STYLE);
	Check((style & WS_CAPTION) == WS_CAPTION, "a windowed window reports WS_CAPTION");
	Check((style & WS_THICKFRAME) != 0, "a windowed window reports a resizable frame");
	Check((style & WS_POPUP) == 0, "a windowed window is not WS_POPUP");
	Check((style & WS_MINIMIZE) == 0, "a window that is not minimised does not report WS_MINIMIZE");

	/*
	**	A fullscreen device window is borderless, and the whole sizing path depends on this
	**	answer: with frame bits reported here, AdjustWindowRect() would inflate the fullscreen
	**	window past the screen.
	*/
	TheSeam.Fullscreen = true;
	style = GetWindowLongA(FAKE_WINDOW, GWL_STYLE);
	Check((style & WS_POPUP) != 0, "a fullscreen window reports WS_POPUP");
	Check((style & (WS_BORDER | WS_DLGFRAME | WS_THICKFRAME)) == 0,
		"a fullscreen window reports no frame bits at all");

	TheSeam.Fullscreen = false;
	TheSeam.Minimised = true;
	style = GetWindowLongA(FAKE_WINDOW, GWL_STYLE);
	Check((style & WS_MINIMIZE) != 0, "a minimised window reports WS_MINIMIZE");

	Check_Equal(GetWindowLongA(FAKE_WINDOW, GWL_EXSTYLE), 0, "there are no extended styles");
	Check_Equal(GetWindowLongA(nullptr, GWL_STYLE), 0, "no window means no style bits");
	// An index that does not exist off Windows answers zero rather than inventing a value.
	Check_Equal(GetWindowLongA(FAKE_WINDOW, -6 /* GWL_HINSTANCE */), 0,
		"an index with no portable meaning answers zero, loudly");
}


/***********************************************************************************************
 *  AdjustWindowRect()                                                                           *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static void Test_Adjust_Window_Rect()
{
	TheSeam = FakeSeam();

	/*
	**	Win32's direction: left/top move NEGATIVE, right/bottom move positive, so the client
	**	origin stays at (0,0) and rect.left is the offset from the frame corner to the client
	**	corner. DX8Wrapper subtracts rect.left/rect.top from the centred position, so a sign
	**	error there moves the window by twice the border.
	*/
	RECT rect = { 0, 0, 800, 600 };
	Check(AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE) != FALSE,
		"AdjustWindowRect() succeeds for a decorated window");
	Check_Equal(rect.left, -3, "left moves out by the left border, i.e. negative");
	Check_Equal(rect.top, -28, "top moves out by the title bar, i.e. negative");
	Check_Equal(rect.right, 803, "right grows by the right border");
	Check_Equal(rect.bottom, 605, "bottom grows by the bottom border");
	Check_Equal(rect.right - rect.left, 800 + 3 + 3, "the frame width is the client width plus both borders");
	Check_Equal(rect.bottom - rect.top, 600 + 28 + 5, "the frame height includes the title bar");

	/*
	**	A borderless style adds nothing. This is the fullscreen path, and adding a border here
	**	would size the fullscreen back buffer to more than the screen.
	*/
	RECT popup = { 0, 0, 1920, 1200 };
	Check(AdjustWindowRect(&popup, WS_POPUP | WS_VISIBLE, FALSE) != FALSE,
		"AdjustWindowRect() succeeds for a borderless window");
	Check_Equal(popup.left, 0, "a borderless window's frame starts where its client does in x");
	Check_Equal(popup.top, 0, "a borderless window's frame starts where its client does in y");
	Check_Equal(popup.right, 1920, "a borderless window's frame is its client width");
	Check_Equal(popup.bottom, 1200, "a borderless window's frame is its client height");

	// A non-zero client origin is displaced, not replaced: Win32 works on whatever rectangle it
	// is handed.
	RECT offset = { 100, 200, 900, 800 };
	Check(AdjustWindowRect(&offset, WS_OVERLAPPEDWINDOW, FALSE) != FALSE,
		"AdjustWindowRect() succeeds on a rectangle away from the origin");
	Check_Equal(offset.left, 97, "an offset rectangle's left still moves out by the border");
	Check_Equal(offset.bottom, 805, "an offset rectangle's bottom still grows by the border");

	/*
	**	When the platform will not report its decorations, the CLIENT SIZE must still come out
	**	right -- that is what the back buffer matches -- so this adds nothing and says so. The
	**	rectangle is left as the client rectangle rather than half adjusted.
	*/
	TheSeam.Have_Insets = false;
	RECT unknown = { 0, 0, 640, 480 };
	Check(AdjustWindowRect(&unknown, WS_OVERLAPPEDWINDOW, FALSE) != FALSE,
		"AdjustWindowRect() still succeeds when the insets are unknown");
	Check_Equal(unknown.right - unknown.left, 640,
		"with unknown insets the frame width falls back to the client width");
	Check_Equal(unknown.bottom - unknown.top, 480,
		"with unknown insets the frame height falls back to the client height");

	Check(AdjustWindowRect(nullptr, WS_OVERLAPPEDWINDOW, FALSE) == FALSE,
		"AdjustWindowRect(nullptr) fails without dereferencing it");
}


/***********************************************************************************************
 *  MonitorFromWindow() and GetMonitorInfoA()                                                    *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static void Test_Monitor_Info()
{
	TheSeam = FakeSeam();

	// The handle is opaque, but it must never be null for a display that exists, because null is
	// what MonitorFromWindow() means by "no monitor".
	HMONITOR primary = MonitorFromWindow(FAKE_WINDOW, MONITOR_DEFAULTTOPRIMARY);
	Check(primary != nullptr, "a window on a display gives a non-null HMONITOR");

	MONITORINFO info = { sizeof(MONITORINFO), { 0, 0, 0, 0 }, { 0, 0, 0, 0 }, 0 };
	Check(GetMonitorInfoA(primary, &info) != FALSE, "GetMonitorInfoA() succeeds for that handle");
	Check_Equal(info.rcMonitor.left, 0, "the primary display's bounds start at x 0");
	Check_Equal(info.rcMonitor.right, 1920, "rcMonitor is left+width, not width");
	Check_Equal(info.rcMonitor.bottom, 1200, "rcMonitor is top+height, not height");
	/*
	**	rcWork is the one DX8Wrapper centres inside, and it is NOT rcMonitor: the menu bar is
	**	excluded, exactly as the taskbar is on Windows. Returning the bounds for both would put
	**	the window's title bar under the menu bar.
	*/
	Check_Equal(info.rcWork.top, 24, "rcWork excludes the menu bar");
	Check_Equal(info.rcWork.bottom, 1140, "rcWork excludes the dock");
	Check(info.rcWork.bottom - info.rcWork.top < info.rcMonitor.bottom - info.rcMonitor.top,
		"the work area is shorter than the display");
	Check((info.dwFlags & MONITORINFOF_PRIMARY) != 0, "display 0 is flagged primary");

	// The second display, which lies at a negative x. An implementation that clamps or takes an
	// absolute value fails here and nowhere else.
	TheSeam.Display_Of_Window = 1;
	HMONITOR second = MonitorFromWindow(FAKE_WINDOW, MONITOR_DEFAULTTONEAREST);
	Check(second != primary, "a window on the second display gives a different HMONITOR");
	Check(GetMonitorInfoA(second, &info) != FALSE, "GetMonitorInfoA() succeeds for it");
	Check_Equal(info.rcMonitor.left, -1280, "a display left of the primary has a negative left");
	Check_Equal(info.rcMonitor.right, 0, "and its right edge is the primary's left edge");
	Check_Equal(info.rcWork.left, -1280, "its work area is negative too");
	Check((info.dwFlags & MONITORINFOF_PRIMARY) == 0, "display 1 is not flagged primary");

	// MONITOR_DEFAULTTONULL with no window is the one case that must answer "no monitor".
	Check(MonitorFromWindow(nullptr, MONITOR_DEFAULTTONULL) == nullptr,
		"MONITOR_DEFAULTTONULL with no window gives no monitor");
	Check(MonitorFromWindow(nullptr, MONITOR_DEFAULTTOPRIMARY) != nullptr,
		"MONITOR_DEFAULTTOPRIMARY with no window still gives the primary");

	// Win32 fails when cbSize was not filled in, and callers use that to detect a foreign struct.
	MONITORINFO unsized = { 0, { 0, 0, 0, 0 }, { 0, 0, 0, 0 }, 0 };
	Check(GetMonitorInfoA(primary, &unsized) == FALSE, "GetMonitorInfoA() fails without cbSize");
	Check(GetMonitorInfoA(primary, nullptr) == FALSE, "GetMonitorInfoA(nullptr) fails");
	Check(GetMonitorInfoA(nullptr, &info) == FALSE, "GetMonitorInfoA() fails for no monitor");
	// A handle for a display that has gone away -- a monitor unplugged between the two calls.
	Check(GetMonitorInfoA(reinterpret_cast<HMONITOR>(9), &info) == FALSE,
		"GetMonitorInfoA() fails for a display that does not exist");
}


/***********************************************************************************************
 *  SetWindowPos()                                                                               *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static void Test_Set_Window_Pos()
{
	TheSeam = FakeSeam();

	/*
	**	Win32 is given a FRAME rectangle; the seam places a CLIENT area. So the client origin is
	**	the frame origin plus the top-left insets, and the client size is the frame size minus
	**	both. Getting this backwards makes the render window grow by the border every time it is
	**	positioned.
	*/
	Check(SetWindowPos(FAKE_WINDOW, nullptr, 200, 100, 806, 633, SWP_NOZORDER) != FALSE,
		"SetWindowPos() succeeds");
	Check_Equal(TheSeam.Set_Position_Calls, 1, "the window was moved once");
	Check_Equal(TheSeam.Set_Position_X, 203, "the client origin is the frame x plus the left border");
	Check_Equal(TheSeam.Set_Position_Y, 128, "the client origin is the frame y plus the title bar");
	Check_Equal(TheSeam.Set_Size_Calls, 1, "the window was sized once");
	Check_Equal(TheSeam.Set_Size_Width, 800, "the client width is the frame width minus both borders");
	Check_Equal(TheSeam.Set_Size_Height, 600, "the client height is the frame height minus the title bar");

	// SWP_NOMOVE / SWP_NOSIZE mean exactly what they say: the ignored pair must not be applied.
	TheSeam = FakeSeam();
	Check(SetWindowPos(FAKE_WINDOW, nullptr, 999, 999, 1006, 833, SWP_NOZORDER | SWP_NOMOVE) != FALSE,
		"SetWindowPos(SWP_NOMOVE) succeeds");
	Check_Equal(TheSeam.Set_Position_Calls, 0, "SWP_NOMOVE does not move the window");
	Check_Equal(TheSeam.Set_Size_Width, 1000, "SWP_NOMOVE still sizes it");

	TheSeam = FakeSeam();
	Check(SetWindowPos(FAKE_WINDOW, nullptr, 12, 34, 999, 999, SWP_NOZORDER | SWP_NOSIZE) != FALSE,
		"SetWindowPos(SWP_NOSIZE) succeeds");
	Check_Equal(TheSeam.Set_Size_Calls, 0, "SWP_NOSIZE does not resize the window");
	Check_Equal(TheSeam.Set_Position_X, 15, "SWP_NOSIZE still moves it");

	/*
	**	The Z order. DX8Wrapper's fullscreen branch passes HWND_TOPMOST with no SWP_NOZORDER, and
	**	that is the only Z-order request the engine makes off Windows.
	*/
	TheSeam = FakeSeam();
	TheSeam.Fullscreen = true;
	Check(SetWindowPos(FAKE_WINDOW, HWND_TOPMOST, 0, 0, 1920, 1200, 0) != FALSE,
		"SetWindowPos(HWND_TOPMOST) succeeds");
	Check_Equal(TheSeam.Always_On_Top_Calls, 1, "HWND_TOPMOST reaches the seam's always-on-top");
	Check(TheSeam.Always_On_Top == true, "HWND_TOPMOST turns always-on-top ON");
	Check_Equal(TheSeam.Set_Size_Width, 1920,
		"a fullscreen window has no insets, so its frame size IS its client size");
	Check_Equal(TheSeam.Set_Position_X, 0, "and its frame origin IS its client origin");

	TheSeam = FakeSeam();
	Check(SetWindowPos(FAKE_WINDOW, HWND_NOTOPMOST, 0, 0, 806, 633, 0) != FALSE,
		"SetWindowPos(HWND_NOTOPMOST) succeeds");
	Check(TheSeam.Always_On_Top == false, "HWND_NOTOPMOST turns always-on-top OFF");

	// SWP_NOZORDER wins over whatever is in insert_after, as Win32 documents.
	TheSeam = FakeSeam();
	Check(SetWindowPos(FAKE_WINDOW, HWND_TOPMOST, 0, 0, 806, 633, SWP_NOZORDER) != FALSE,
		"SetWindowPos(HWND_TOPMOST | SWP_NOZORDER) succeeds");
	Check_Equal(TheSeam.Always_On_Top_Calls, 0, "SWP_NOZORDER ignores insert_after entirely");

	// SWP_SHOWWINDOW is the one other flag with a portable meaning.
	TheSeam = FakeSeam();
	Check(SetWindowPos(FAKE_WINDOW, nullptr, 0, 0, 806, 633,
		SWP_NOZORDER | SWP_SHOWWINDOW) != FALSE, "SetWindowPos(SWP_SHOWWINDOW) succeeds");
	Check_Equal(TheSeam.Show_Window_Calls, 1, "SWP_SHOWWINDOW shows the window");

	Check(SetWindowPos(nullptr, nullptr, 0, 0, 100, 100, SWP_NOZORDER) == FALSE,
		"SetWindowPos() with no window fails");
}


/***********************************************************************************************
 *  The sizing path, composed as DX8Wrapper::Resize_And_Position_Window() composes it             *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static void Test_Resize_And_Position_Round_Trip()
{
	/*
	**	dx8wrapper.cpp, windowed branch, transcribed:
	**
	**	    GetClientRect(hwnd, &rect);
	**	    if size differs from Resolution*:
	**	        rect = { 0, 0, ResolutionWidth, ResolutionHeight };
	**	        AdjustWindowRect(&rect, GetWindowLong(hwnd, GWL_STYLE), FALSE);
	**	        width = rect.right - rect.left; height = rect.bottom - rect.top;
	**	        GetMonitorInfo(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &mi);
	**	        left = (mi.rcWork.left + mi.rcWork.right - width) / 2;
	**	        top  = (mi.rcWork.top + mi.rcWork.bottom - height) / 2;
	**	        SetWindowPos(hwnd, nullptr, left, top, width, height, SWP_NOZORDER);
	**
	**	The invariant that matters is that the client area ends up exactly the requested
	**	resolution, and that the window sits inside the work area. Both are properties of the
	**	whole composition, which is why they are checked here rather than per entry point.
	*/
	TheSeam = FakeSeam();
	TheSeam.Client_Width = 640;
	TheSeam.Client_Height = 480;

	const int RESOLUTION_WIDTH = 1024;
	const int RESOLUTION_HEIGHT = 768;

	RECT rect = { 0, 0, 0, 0 };
	Check(GetClientRect(FAKE_WINDOW, &rect) != FALSE, "the round trip reads the client rect");
	Check(rect.right - rect.left != RESOLUTION_WIDTH,
		"the round trip starts from a window of the wrong size, so the resize happens");

	rect.left = 0;
	rect.top = 0;
	rect.right = RESOLUTION_WIDTH;
	rect.bottom = RESOLUTION_HEIGHT;
	const DWORD style = (DWORD)GetWindowLongA(FAKE_WINDOW, GWL_STYLE);
	Check(AdjustWindowRect(&rect, style, FALSE) != FALSE, "the round trip adjusts for the frame");
	const int width = rect.right - rect.left;
	const int height = rect.bottom - rect.top;

	MONITORINFO mi = { sizeof(MONITORINFO), { 0, 0, 0, 0 }, { 0, 0, 0, 0 }, 0 };
	Check(GetMonitorInfoA(MonitorFromWindow(FAKE_WINDOW, MONITOR_DEFAULTTOPRIMARY), &mi) != FALSE,
		"the round trip reads the monitor it is on");
	const int left = (mi.rcWork.left + mi.rcWork.right - width) / 2;
	const int top = (mi.rcWork.top + mi.rcWork.bottom - height) / 2;

	Check(SetWindowPos(FAKE_WINDOW, nullptr, left, top, width, height, SWP_NOZORDER) != FALSE,
		"the round trip positions the window");

	Check_Equal(TheSeam.Set_Size_Width, RESOLUTION_WIDTH,
		"the client area ends up exactly ResolutionWidth");
	Check_Equal(TheSeam.Set_Size_Height, RESOLUTION_HEIGHT,
		"the client area ends up exactly ResolutionHeight");

	// And GetClientRect() now agrees, which is the loop-termination condition in the caller.
	Check(GetClientRect(FAKE_WINDOW, &rect) != FALSE, "the client rect is readable afterwards");
	Check_Equal(rect.right - rect.left, RESOLUTION_WIDTH,
		"GetClientRect() now reports the requested width, so the caller stops resizing");

	// The frame, title bar included, is inside the work area: centred, not off the top.
	Check(top >= mi.rcWork.top, "the frame's top is not above the work area");
	Check(top + height <= mi.rcWork.bottom, "the frame's bottom is not below the work area");
	Check_Equal(TheSeam.Set_Position_Y, top + 28,
		"the client area starts one title bar below the frame");
}


/***********************************************************************************************
 *  GetDesktopWindow() and SetDeviceGammaRamp()                                                  *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static void Test_Gamma_Fallback()
{
	/*
	**	dx8wrapper.cpp, when the device reports no gamma support:
	**	    HWND hwnd = GetDesktopWindow();
	**	    HDC hdc = GetDC(hwnd);
	**	    if (hdc) { SetDeviceGammaRamp(hdc, &ramp); ReleaseDC(hwnd, hdc); }
	**
	**	What must hold: the handle is usable (non-null, so the caller does not skip its own
	**	cleanup), it is NOT the game's window (so code that mistook it for one cannot resize or
	**	hide the game), and the ramp call REFUSES rather than reporting success.
	*/
	TheSeam = FakeSeam();

	HWND desktop = GetDesktopWindow();
	Check(desktop != nullptr, "GetDesktopWindow() returns a usable handle");
	Check(desktop != FAKE_WINDOW, "the desktop handle is not the game's window");
	Check(GetDesktopWindow() == desktop, "the desktop handle is stable across calls");
	// The seam does not recognise it, so a stray window call on it fails rather than hitting the
	// game's window.
	Check(IsIconic(desktop) == FALSE, "the desktop handle is not a window the seam will act on");
	RECT rect = { 0, 0, 0, 0 };
	Check(GetClientRect(desktop, &rect) == FALSE, "the desktop handle has no client area here");

	/*
	**	The refusal. FALSE is what Win32 returns when the ramp was not applied, and the caller
	**	ignores the result -- but a TRUE here would be the invisible lie: the brightness slider
	**	would appear to work and nothing on screen would change.
	*/
	unsigned short ramp[3][256];
	for (int channel = 0; channel < 3; channel++) {
		for (int index = 0; index < 256; index++) {
			ramp[channel][index] = (unsigned short)(index << 8);
		}
	}
	Check(SetDeviceGammaRamp(reinterpret_cast<HDC>(0xDC), ramp) == FALSE,
		"SetDeviceGammaRamp() refuses rather than claiming to have set a ramp");
	Check(SetDeviceGammaRamp(nullptr, nullptr) == FALSE,
		"SetDeviceGammaRamp() refuses a null ramp too, without dereferencing it");
}


int main()
{
	Test_Get_Cursor_Pos();
	Test_Screen_To_Client();
	Test_Cursor_Round_Trip();
	Test_Cursor_Decode();
	Test_Set_Cursor();
	Test_Retail_Cursor_Set();
	Test_Is_Iconic();
	Test_Get_Client_Rect();
	Test_Get_Window_Long();
	Test_Adjust_Window_Rect();
	Test_Monitor_Info();
	Test_Set_Window_Pos();
	Test_Resize_And_Position_Round_Trip();
	Test_Gamma_Fallback();

	printf("%d checks, %d failure(s)\n", _Checks, _Failures);
	return (_Failures == 0) ? 0 : 1;
}
