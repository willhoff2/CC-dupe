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
 *                 Project Name : Westwood Library                                             *
 *                                                                                             *
 *  MessageBox() for the platforms that have no MessageBox(). This is a **stub that fails       *
 *  loudly**, in the same sense as the DbgHelp crash reporting stub: the caption, the text and  *
 *  the button set are written to stderr with a marker, and the answer is the one the Win32     *
 *  call's default button would have given. Nothing is drawn on screen and nothing waits for a  *
 *  human, because a modal dialog is not on the path to running the game and because the only   *
 *  callers are assert and crash paths that must work when the window is already gone.          *
 *                                                                                             *
 *  See docs/porting/window-event-loop.md, section "What is deliberately stubbed".              *
 *                                                                                             *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#pragma once

#ifndef _WIN32

namespace WWPlatform
{

/*
**	The button sets Debug.cpp actually asks for: MB_OK, MB_YESNO and MB_ABORTRETRYIGNORE.
*/
enum DialogButtons
{
	DIALOG_BUTTONS_OK = 0,
	DIALOG_BUTTONS_YES_NO,
	DIALOG_BUTTONS_ABORT_RETRY_IGNORE,
};

/*
**	The answers, numbered as the Win32 ID* constants are, so that a caller may compare against
**	IDOK/IDABORT/IDRETRY/IDIGNORE/IDYES/IDNO without a translation table.
*/
enum DialogResult
{
	DIALOG_RESULT_OK = 1,
	DIALOG_RESULT_ABORT = 3,
	DIALOG_RESULT_RETRY = 4,
	DIALOG_RESULT_IGNORE = 5,
	DIALOG_RESULT_YES = 6,
	DIALOG_RESULT_NO = 7,
};

/*
**	Writes the box to stderr and returns without waiting: OK for DIALOG_BUTTONS_OK, NO for
**	DIALOG_BUTTONS_YES_NO, and IGNORE for DIALOG_BUTTONS_ABORT_RETRY_IGNORE, which is the button
**	MB_DEFBUTTON3 selects on the assert dialog. So an assert reports and continues rather than
**	stopping a machine nobody is watching; it never silently swallows the report.
*/
DialogResult Dialog_Message_Box(const char * caption, const char * text, DialogButtons buttons);

}	// namespace WWPlatform

#endif // !_WIN32
