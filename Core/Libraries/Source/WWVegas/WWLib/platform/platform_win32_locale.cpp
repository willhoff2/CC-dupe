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

/***********************************************************************************************
 *                                                                                             *
 *                 Project Name : Westwood Library                                             *
 *                                                                                             *
 *  The locale half of the Win32 compatibility layer: the date and time formatting the save     *
 *  game and replay lists display, and the FormatMessage() the error popups read a failure code  *
 *  back with.                                                                                  *
 *                                                                                             *
 *  The LCID argument is ignored. Win32 takes the format from the user's regional settings;      *
 *  here it comes from the C library's LC_TIME locale, which is the platform's answer to the     *
 *  same question and is what the engine's "use the user's locale, not the system's" tweak was    *
 *  after. LOCALE_USER_DEFAULT and LOCALE_SYSTEM_DEFAULT are therefore the same thing here.      *
 *                                                                                             *
 *  The wide entry points are over wchar_t as the engine has it today -- 4 bytes here against 2  *
 *  on MSVC. The "what is a wide string off Windows" decision is deliberately still open, so     *
 *  these widen and narrow one code unit at a time and do not touch WideChar.                   *
 *                                                                                             *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#include "WWLib/platform/platform_win32_compat.h"

#ifdef WWPLATFORM_WIN32_COMPAT

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <string>

namespace
{

/*
**	SYSTEMTIME -> struct tm. A null SYSTEMTIME means "now", as it does in Win32.
*/
struct tm To_Tm(const SYSTEMTIME * system_time)
{
	struct tm parts;
	memset(&parts, 0, sizeof(parts));

	if (system_time == nullptr) {
		time_t now = time(nullptr);
		localtime_r(&now, &parts);
		return parts;
	}

	parts.tm_year = (int)system_time->wYear - 1900;
	parts.tm_mon = (int)system_time->wMonth - 1;
	parts.tm_mday = (int)system_time->wDay;
	parts.tm_hour = (int)system_time->wHour;
	parts.tm_min = (int)system_time->wMinute;
	parts.tm_sec = (int)system_time->wSecond;
	parts.tm_wday = (int)system_time->wDayOfWeek;
	parts.tm_isdst = -1;

	/*
	**	The day of the week the caller supplied is not trusted: SerializedDateTime stores one,
	**	but a hand-built SYSTEMTIME may leave it at zero, and it is what "dddd" prints.
	*/
	struct tm normalised = parts;
	time_t stamp = mktime(&normalised);
	if (stamp != (time_t)-1) {
		parts.tm_wday = normalised.tm_wday;
		parts.tm_yday = normalised.tm_yday;
	}
	return parts;
}


std::string Strftime(const char * format, const struct tm & parts)
{
	char buffer[256];
	size_t length = strftime(buffer, sizeof(buffer), format, &parts);
	return std::string(buffer, length);
}


/*
**	Expand a Win32 date or time picture string: a run of the same letter is one field, anything
**	else is a literal, and text inside single quotes is literal too. This is the subset the
**	engine uses -- "yyyy", "MM", "dd" in WOLLoginMenu -- plus the rest of the letters the
**	documentation defines, so that a data-driven format string does not hit a hole.
*/
std::string Expand_Picture(const char * picture, const struct tm & parts, bool date)
{
	std::string result;

	for (const char * cursor = picture; *cursor != 0; ) {
		char letter = *cursor;

		if (letter == '\'') {
			++cursor;
			while (*cursor != 0 && *cursor != '\'') {
				result += *cursor++;
			}
			if (*cursor == '\'') {
				++cursor;
			}
			continue;
		}

		int run = 0;
		while (cursor[run] == letter) {
			++run;
		}

		char field[64];
		field[0] = 0;

		if (date) {
			switch (letter) {
				case 'd':
					if (run == 1) snprintf(field, sizeof(field), "%d", parts.tm_mday);
					else if (run == 2) snprintf(field, sizeof(field), "%02d", parts.tm_mday);
					else if (run == 3) snprintf(field, sizeof(field), "%s", Strftime("%a", parts).c_str());
					else snprintf(field, sizeof(field), "%s", Strftime("%A", parts).c_str());
					break;
				case 'M':
					if (run == 1) snprintf(field, sizeof(field), "%d", parts.tm_mon + 1);
					else if (run == 2) snprintf(field, sizeof(field), "%02d", parts.tm_mon + 1);
					else if (run == 3) snprintf(field, sizeof(field), "%s", Strftime("%b", parts).c_str());
					else snprintf(field, sizeof(field), "%s", Strftime("%B", parts).c_str());
					break;
				case 'y':
					if (run <= 2) snprintf(field, sizeof(field), "%02d", (parts.tm_year + 1900) % 100);
					else snprintf(field, sizeof(field), "%d", parts.tm_year + 1900);
					break;
				case 'g':
					/*
					**	The era. Win32 prints "A.D." for the Gregorian calendar; nothing in the
					**	engine asks for it, and there is no C library equivalent.
					*/
					snprintf(field, sizeof(field), "%s", "A.D.");
					break;
				default:
					break;
			}
		} else {
			switch (letter) {
				case 'h': {
					int hour = parts.tm_hour % 12;
					if (hour == 0) hour = 12;
					if (run == 1) snprintf(field, sizeof(field), "%d", hour);
					else snprintf(field, sizeof(field), "%02d", hour);
					break;
				}
				case 'H':
					if (run == 1) snprintf(field, sizeof(field), "%d", parts.tm_hour);
					else snprintf(field, sizeof(field), "%02d", parts.tm_hour);
					break;
				case 'm':
					if (run == 1) snprintf(field, sizeof(field), "%d", parts.tm_min);
					else snprintf(field, sizeof(field), "%02d", parts.tm_min);
					break;
				case 's':
					if (run == 1) snprintf(field, sizeof(field), "%d", parts.tm_sec);
					else snprintf(field, sizeof(field), "%02d", parts.tm_sec);
					break;
				case 't': {
					std::string marker = Strftime("%p", parts);
					if (run == 1 && !marker.empty()) {
						marker.erase(1);
					}
					snprintf(field, sizeof(field), "%s", marker.c_str());
					break;
				}
				default:
					break;
			}
		}

		if (field[0] != 0) {
			result += field;
		} else {
			/*
			**	Not a field letter: the run is literal text, quoted or not.
			*/
			result.append(cursor, run);
		}
		cursor += run;
	}

	return result;
}


std::string Default_Date(DWORD flags, const struct tm & parts)
{
	/*
	**	DATE_LONGDATE is "Friday, 14 August 2026"; %x is the locale's own short date, which is
	**	the closest thing there is to Windows' regional short date setting.
	*/
	if ((flags & DATE_LONGDATE) != 0) {
		return Strftime("%A, %d %B %Y", parts);
	}
	return Strftime("%x", parts);
}


std::string Default_Time(DWORD flags, const struct tm & parts)
{
	bool force24 = (flags & TIME_FORCE24HOURFORMAT) != 0;
	bool marker = (flags & TIME_NOTIMEMARKER) == 0;

	std::string format;
	if (force24) {
		format = "%H";
	} else {
		format = "%I";
	}

	if ((flags & TIME_NOMINUTESORSECONDS) == 0) {
		format += ":%M";
		if ((flags & TIME_NOSECONDS) == 0) {
			format += ":%S";
		}
	}
	if (marker && !force24) {
		format += " %p";
	}
	return Strftime(format.c_str(), parts);
}


/*
**	Copy a narrow result out under the Win32 buffer protocol: a zero buffer size asks how much
**	room the result needs, and the count includes the terminator.
*/
int Copy_Out(const std::string & text, LPSTR buffer, int size)
{
	int needed = (int)text.size() + 1;
	if (size == 0 || buffer == nullptr) {
		return needed;
	}
	if (size < needed) {
		WWPlatform::Win32::Set_Last_Error(ERROR_MORE_DATA);
		return 0;
	}
	memcpy(buffer, text.c_str(), (size_t)needed);
	return needed;
}


int Copy_Out_Wide(const std::string & text, LPWSTR buffer, int size)
{
	int needed = (int)text.size() + 1;
	if (size == 0 || buffer == nullptr) {
		return needed;
	}
	if (size < needed) {
		WWPlatform::Win32::Set_Last_Error(ERROR_MORE_DATA);
		return 0;
	}
	for (size_t index = 0; index < text.size(); ++index) {
		buffer[index] = (wchar_t)(unsigned char)text[index];
	}
	buffer[text.size()] = 0;
	return needed;
}


std::string Narrow(const wchar_t * wide)
{
	std::string result;
	for (; wide != nullptr && *wide != 0; ++wide) {
		result += (char)(*wide < 0x100 ? (char)*wide : '?');
	}
	return result;
}

}	// anonymous namespace


extern "C" {

int GetDateFormatA(LCID, DWORD flags, const SYSTEMTIME * system_time, LPCSTR picture,
	LPSTR buffer, int size)
{
	struct tm parts = To_Tm(system_time);
	std::string text = (picture != nullptr && *picture != 0)
		? Expand_Picture(picture, parts, true)
		: Default_Date(flags, parts);
	return Copy_Out(text, buffer, size);
}


int GetDateFormatW(LCID, DWORD flags, const SYSTEMTIME * system_time, LPCWSTR picture,
	LPWSTR buffer, int size)
{
	struct tm parts = To_Tm(system_time);
	std::string narrow_picture = Narrow(picture);
	std::string text = !narrow_picture.empty()
		? Expand_Picture(narrow_picture.c_str(), parts, true)
		: Default_Date(flags, parts);
	return Copy_Out_Wide(text, buffer, size);
}


int GetTimeFormatA(LCID, DWORD flags, const SYSTEMTIME * system_time, LPCSTR picture,
	LPSTR buffer, int size)
{
	struct tm parts = To_Tm(system_time);
	std::string text = (picture != nullptr && *picture != 0)
		? Expand_Picture(picture, parts, false)
		: Default_Time(flags, parts);
	return Copy_Out(text, buffer, size);
}


int GetTimeFormatW(LCID, DWORD flags, const SYSTEMTIME * system_time, LPCWSTR picture,
	LPWSTR buffer, int size)
{
	struct tm parts = To_Tm(system_time);
	std::string narrow_picture = Narrow(picture);
	std::string text = !narrow_picture.empty()
		? Expand_Picture(narrow_picture.c_str(), parts, false)
		: Default_Time(flags, parts);
	return Copy_Out_Wide(text, buffer, size);
}


/*
**	FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM) over the error codes this layer produces: the code
**	is turned back into the errno it came from and described by strerror(). Unlike Windows the
**	text has no trailing CR/LF -- the call sites trim() it anyway.
**
**	FORMAT_MESSAGE_FROM_STRING and FORMAT_MESSAGE_FROM_HMODULE are not implemented (there is no
**	message table in an ELF or Mach-O image, and nothing in the engine passes either), and
**	neither is FORMAT_MESSAGE_ALLOCATE_BUFFER.
*/
DWORD FormatMessageW(DWORD flags, LPCVOID, DWORD message_id, DWORD, LPWSTR buffer, DWORD size,
	va_list *)
{
	if (buffer == nullptr || size == 0) {
		return 0;
	}

	if ((flags & FORMAT_MESSAGE_ALLOCATE_BUFFER) != 0) {
		WWPlatform::Win32::Report_Stub("FormatMessageW",
			"FORMAT_MESSAGE_ALLOCATE_BUFFER is not supported");
		buffer[0] = 0;
		return 0;
	}

	std::string text;
	if ((flags & FORMAT_MESSAGE_FROM_SYSTEM) != 0) {
		const char * message = strerror(WWPlatform::Win32::Errno_From_Error(message_id));
		text = (message != nullptr) ? message : "";
	} else {
		WWPlatform::Win32::Report_Stub("FormatMessageW",
			"only FORMAT_MESSAGE_FROM_SYSTEM is supported");
		buffer[0] = 0;
		return 0;
	}

	DWORD count = 0;
	while (count < size - 1 && count < (DWORD)text.size()) {
		buffer[count] = (wchar_t)(unsigned char)text[count];
		++count;
	}
	buffer[count] = 0;
	return count;
}

}	// extern "C"

#endif	// WWPLATFORM_WIN32_COMPAT
