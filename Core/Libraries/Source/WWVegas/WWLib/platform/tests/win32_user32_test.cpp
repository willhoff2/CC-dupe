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
 *  Behaviour test for the user32 cursor and window-state entry points -- GetCursorPos(),        *
 *  ScreenToClient(), SetCursor() and IsIconic() -- which off Windows are defined over the       *
 *  window seam in platform_win32_user.cpp.                                                     *
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
 *  Run through scripts/native-win32-cursor-test.py. Windows is not the oracle for the fake       *
 *  seam, but it is for the arithmetic, and the expectations below are what user32 documents.     *
 *                                                                                             *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#include "WWLib/platform/platform_window.h"

#include <windows.h>

#include <stdio.h>

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

	FakeSeam()
		: Current(FAKE_WINDOW), Minimised(false), Cursor_X(0), Cursor_Y(0), Origin_X(0),
		  Origin_Y(0), Have_Cursor(true), Have_Origin(true), Show_Calls(0), Last_Show(false),
		  Last_Show_Window(nullptr)
	{
	}
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
 *  SetCursor()                                                                                  *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static void Test_Set_Cursor()
{
	TheSeam = FakeSeam();

	/*
	**	The call the engine makes: SetCursor(nullptr) means "no cursor", so the system pointer
	**	must be hidden. Showing it instead is the two-cursors defect, and it is a `!` away.
	*/
	Check(SetCursor(nullptr) == nullptr, "the first SetCursor() reports no previous cursor");
	Check_Equal(TheSeam.Show_Calls, 1, "SetCursor() goes through the seam rather than doing nothing");
	Check(TheSeam.Last_Show == false, "SetCursor(nullptr) HIDES the system pointer");
	Check(TheSeam.Last_Show_Window == FAKE_WINDOW,
		"SetCursor() addresses the seam's own window");

	/*
	**	A non-null handle cannot be resolved to a shape here. What is asserted is that it does not
	**	silently do nothing: the system pointer is shown, so the symptom is a visible wrong cursor
	**	rather than no cursor at all, and the previous handle is returned as Win32 documents.
	*/
	HCURSOR arrow = reinterpret_cast<HCURSOR>(0xA110C);
	Check(SetCursor(arrow) == nullptr, "SetCursor() returns the previous cursor, which was null");
	Check_Equal(TheSeam.Show_Calls, 2, "a non-null cursor still reaches the seam");
	Check(TheSeam.Last_Show == true, "a non-null cursor shows the system pointer");

	Check(SetCursor(nullptr) == arrow, "SetCursor() returns the handle set before it");
	Check(TheSeam.Last_Show == false, "back to nullptr hides the system pointer again");

	// Before the window exists the call must still be safe; the seam is what rejects the null
	// window, and it is reached with it rather than being second-guessed here.
	TheSeam.Current = nullptr;
	TheSeam.Show_Calls = 0;
	SetCursor(nullptr);
	Check_Equal(TheSeam.Show_Calls, 1, "SetCursor() before the window exists still calls the seam");
	Check(TheSeam.Last_Show_Window == nullptr, "and passes the null window through unchanged");
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


int main()
{
	Test_Get_Cursor_Pos();
	Test_Screen_To_Client();
	Test_Cursor_Round_Trip();
	Test_Set_Cursor();
	Test_Is_Iconic();

	printf("%d checks, %d failure(s)\n", _Checks, _Failures);
	return (_Failures == 0) ? 0 : 1;
}
