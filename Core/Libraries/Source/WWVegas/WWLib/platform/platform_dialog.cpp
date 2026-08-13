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

#include "WWLib/platform/platform_dialog.h"

#ifndef _WIN32

#include <stdio.h>

namespace WWPlatform
{

DialogResult Dialog_Message_Box(const char * caption, const char * text, DialogButtons buttons)
{
	const char * answer = "OK";
	DialogResult result = DIALOG_RESULT_OK;

	switch (buttons)
	{
		case DIALOG_BUTTONS_YES_NO:
			answer = "No";
			result = DIALOG_RESULT_NO;
			break;

		case DIALOG_BUTTONS_ABORT_RETRY_IGNORE:
			answer = "Ignore";
			result = DIALOG_RESULT_IGNORE;
			break;

		case DIALOG_BUTTONS_OK:
		default:
			break;
	}

	fprintf(stderr, "\n!!! MESSAGE BOX (no native dialog; answering \"%s\")\n!!! %s\n!!! %s\n",
		answer, caption != nullptr ? caption : "", text != nullptr ? text : "");
	fflush(stderr);

	return result;
}

}	// namespace WWPlatform

#endif // !_WIN32
