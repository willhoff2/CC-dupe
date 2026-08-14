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

/*
**	zh-path-probe - point the engine's own directory seam at a real retail data directory.
**
**	Everything measured here goes through WWPlatform::Path, i.e. the code the engine calls
**	instead of FindFirstFile()/FindNextFile()/_access(), compiled unmodified. The data
**	directory is read only: nothing here creates, writes or deletes.
**
**	    zh-path-probe <data directory>
**
**	Exits non-zero if any check fails.
*/

#ifdef ZH_PATH_PROBE_WIN32_SEAM
/*
**	The Win32 half of the same question: the engine's own call sites still spell this
**	FindFirstFile()/FindNextFile(), and off Windows those names resolve to the seam in
**	platform_win32_file.cpp. Its behaviour test runs against a synthetic tree; this runs the same
**	entry points against the retail archives.
*/
#include <windows.h>
#endif

#include "WWLib/platform/platform_path.h"

#include <cstdio>
#include <cstring>
#include <string>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace
{

int Failures = 0;

void Check(bool ok, const char * detail)
{
	std::printf("  %-6s %s\n", ok ? "PASS" : "FAIL", detail);
	if (!ok) ++Failures;
}

int Count(const char * directory, const char * pattern)
{
	DynamicVectorClass<WWPlatform::Path::EntryClass> entries;
	if (!WWPlatform::Path::Enumerate(directory, pattern, entries)) return -1;
	return entries.Count();
}

void Print_First(const char * directory, const char * pattern, int limit)
{
	DynamicVectorClass<WWPlatform::Path::EntryClass> entries;
	if (!WWPlatform::Path::Enumerate(directory, pattern, entries)) {
		std::printf("         Enumerate(%s) failed\n", pattern);
		return;
	}
	for (int i = 0; i < entries.Count() && i < limit; ++i) {
		std::printf("         %s%s\n", entries[i].Name.Peek_Buffer(),
			entries[i].Is_Directory ? "/" : "");
	}
}

#ifdef ZH_PATH_PROBE_WIN32_SEAM
/*
**	One FindFirstFile()/FindNextFile()/FindClose() loop, written the way the engine's asset scan
**	writes it. Returns the number of entries, or -1 for the "nothing matched" return.
*/
int Find_Count(const std::string & spec, std::string * first = nullptr,
	unsigned long * last_error = nullptr)
{
	WIN32_FIND_DATAA data;
	HANDLE handle = FindFirstFileA(spec.c_str(), &data);
	if (handle == INVALID_HANDLE_VALUE) {
		if (last_error != nullptr) *last_error = GetLastError();
		return -1;
	}

	int count = 0;
	do {
		if (first != nullptr && count == 0) *first = data.cFileName;
		++count;
	} while (FindNextFileA(handle, &data) != FALSE);

	if (last_error != nullptr) *last_error = GetLastError();
	FindClose(handle);
	return count;
}

void Measure_Win32_Seam(const char * data)
{
	std::printf("\nFindFirstFile()/FindNextFile() - the seam itself, on the retail archives\n");

	const std::string base = data;
	std::string first_big;
	const int all = Find_Count(base + "\\*");
	const int big_lower = Find_Count(base + "\\*.big", &first_big);
	const int big_upper = Find_Count(base + "\\*.BIG");
	const int star_big = Find_Count(base + "\\*big");
	const int prefixed = Find_Count(base + "\\Speech*.big");
	const int slashed = Find_Count(base + "/*.big");
	const int no_extension = Find_Count(base + "\\*.");
	std::printf("         <dir>\\*           %d entries\n", all);
	std::printf("         <dir>\\*.big       %d entries (first: %s)\n", big_lower,
		first_big.empty() ? "(none)" : first_big.c_str());
	std::printf("         <dir>\\*.BIG       %d entries\n", big_upper);
	std::printf("         <dir>\\*big        %d entries\n", star_big);
	std::printf("         <dir>\\Speech*.big %d entries\n", prefixed);
	std::printf("         <dir>/*.big        %d entries\n", slashed);
	std::printf("         <dir>\\*.          %d entries (Win32: names with no extension)\n",
		no_extension);

	Check(big_lower > 0, "FindFirstFile(*.big) finds the retail archives");
	Check(big_lower == big_upper, "*.BIG matches the same set as *.big");
	Check(star_big >= big_lower, "*big is a superset of *.big");
	Check(prefixed > 0 && prefixed < big_lower, "a prefixed pattern narrows the set");
	Check(slashed == big_lower, "a forward slash separated spec matches the backslash one");
	Check(all >= big_lower, "* enumerates at least the archives");

	/*
	**	The empty match set: Win32 returns INVALID_HANDLE_VALUE with ERROR_FILE_NOT_FOUND, which is
	**	how the asset scan tells "no archives here" from "no such directory" (ERROR_PATH_NOT_FOUND).
	*/
	unsigned long empty_error = 0;
	const int nothing = Find_Count(base + "\\*.no-such-extension", nullptr, &empty_error);
	std::printf("         <dir>\\*.no-such-extension -> %d, GetLastError() = %lu\n", nothing,
		empty_error);
	Check(nothing < 0 && empty_error == ERROR_FILE_NOT_FOUND,
		"an empty match set is INVALID_HANDLE_VALUE with ERROR_FILE_NOT_FOUND");

	unsigned long missing_error = 0;
	const int missing = Find_Count(base + "\\NoSuchDirectory\\*.big", nullptr, &missing_error);
	std::printf("         <dir>\\NoSuchDirectory\\*.big -> %d, GetLastError() = %lu\n", missing,
		missing_error);
	Check(missing < 0 && missing_error == ERROR_PATH_NOT_FOUND,
		"a missing directory is ERROR_PATH_NOT_FOUND, not ERROR_FILE_NOT_FOUND");

	/*
	**	The fields the asset scan reads off WIN32_FIND_DATA: the name it opens, the size it trusts
	**	and the directory bit it skips on.
	*/
	if (!first_big.empty()) {
		WIN32_FIND_DATAA found;
		const std::string exact = base + "\\" + first_big;
		HANDLE handle = FindFirstFileA(exact.c_str(), &found);
		Check(handle != INVALID_HANDLE_VALUE, "an exact name is its own one entry match");
		if (handle != INVALID_HANDLE_VALUE) {
			const unsigned long long size =
				(static_cast<unsigned long long>(found.nFileSizeHigh) << 32) | found.nFileSizeLow;
			std::printf("         %s: %llu bytes, attributes 0x%08lx\n", found.cFileName, size,
				static_cast<unsigned long>(found.dwFileAttributes));
			Check(size > 0, "nFileSize{High,Low} carries the real archive size");
			Check((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0,
				"an archive is not flagged FILE_ATTRIBUTE_DIRECTORY");
			Check(first_big == found.cFileName,
				"cFileName is the on disk spelling, not the pattern's");
			FindClose(handle);
		}

		std::string shouty = first_big;
		for (char & c : shouty) c = static_cast<char>(std::toupper(c));
		WIN32_FIND_DATAA folded;
		HANDLE folded_handle = FindFirstFileA((base + "\\" + shouty).c_str(), &folded);
		const bool folds = folded_handle != INVALID_HANDLE_VALUE;
		std::printf("         upper cased name %s -> %s\n", shouty.c_str(),
			folds ? folded.cFileName : "(no match)");
		Check(folds, "an upper cased name resolves, as it does on Win32");
		if (folds) FindClose(folded_handle);
	}

	/*
	**	GetFileAttributes() is the other half of the asset scan's "is this a directory" test.
	*/
	const std::string data_subdir = base + "\\Data";
	const unsigned long attributes = GetFileAttributesA(data_subdir.c_str());
	std::printf("         GetFileAttributes(\"%s\") = 0x%08lx\n", data_subdir.c_str(), attributes);
	Check(attributes != INVALID_FILE_ATTRIBUTES &&
		(attributes & FILE_ATTRIBUTE_DIRECTORY) != 0,
		"GetFileAttributes() reports the retail Data directory as a directory");
}
#endif

}	// namespace

int main(int argc, char ** argv)
{
	if (argc < 2) {
		std::fprintf(stderr, "usage: %s <data directory>\n", argv[0]);
		return 2;
	}
	const char * data = argv[1];

	std::printf("data directory: %s\n\n", data);

	std::printf("Enumerate() - the FindFirstFile()/FindNextFile() loop\n");
	const int all = Count(data, "*");
	const int big_lower = Count(data, "*.big");
	const int big_upper = Count(data, "*.BIG");
	const int star_big = Count(data, "*big");
	const int prefixed = Count(data, "Speech*.big");
	std::printf("         *          %d entries\n", all);
	std::printf("         *.big      %d entries\n", big_lower);
	std::printf("         *.BIG      %d entries\n", big_upper);
	std::printf("         *big       %d entries\n", star_big);
	std::printf("         Speech*.big %d entries\n", prefixed);
	Print_First(data, "*.big", 4);
	Check(all > 0, "the directory enumerates at all");
	Check(big_lower > 0, "*.big matches the retail archives");
	Check(big_lower == big_upper,
		"*.BIG matches the same count as *.big (Win32 patterns are case insensitive)");
	Check(star_big >= big_lower, "*big is a superset of *.big");
	Check(prefixed > 0 && prefixed < big_lower, "a prefixed pattern narrows the set");

	std::printf("\nHas_Match() - the \"is there anything to load here\" test\n");
	Check(WWPlatform::Path::Has_Match(data, "*.big"), "Has_Match(*.big)");
	Check(WWPlatform::Path::Has_Match(data, "*.BIG"), "Has_Match(*.BIG)");
	Check(!WWPlatform::Path::Has_Match(data, "*.no-such-extension"),
		"Has_Match() is false for a pattern nothing matches");

	std::printf("\nExists() - _access(path, 0), with the engine's path spellings\n");
	DynamicVectorClass<WWPlatform::Path::EntryClass> archives;
	WWPlatform::Path::Enumerate(data, "*.big", archives);
	if (archives.Count() > 0) {
		const std::string name = archives[0].Name.Peek_Buffer();
		std::string upper = name, lower = name;
		for (char & c : upper) c = static_cast<char>(std::toupper(c));
		for (char & c : lower) c = static_cast<char>(std::tolower(c));
		const std::string exact = std::string(data) + "/" + name;
		const std::string as_upper = std::string(data) + "/" + upper;
		const std::string as_lower = std::string(data) + "/" + lower;
		const std::string backslashed = std::string(data) + "\\" + name;
		std::printf("         first archive: %s\n", name.c_str());
		Check(WWPlatform::Path::Exists(exact.c_str()), "the exact spelling exists");
		Check(WWPlatform::Path::Exists(as_upper.c_str()), "the upper case spelling resolves");
		Check(WWPlatform::Path::Exists(as_lower.c_str()), "the lower case spelling resolves");
		Check(WWPlatform::Path::Exists(backslashed.c_str()),
			"a backslash separated path resolves");
	} else {
		Check(false, "at least one .big archive to test spellings against");
	}

	std::printf("\nData subdirectory - the mixed case literals in the retail tree\n");
	const std::string data_dir = std::string(data) + "\\Data";
	StringClass resolved;
#ifndef _WIN32
	const bool resolves = WWPlatform::Path::Resolve(data_dir.c_str(), resolved, false);
	std::printf("         Resolve(\"%s\") -> %s\n", data_dir.c_str(),
		resolves ? resolved.Peek_Buffer() : "(no match)");
	Check(resolves, "Resolve() maps a Windows spelled path onto the real tree");
	const std::string shouty = std::string(data) + "\\DATA\\INI";
	StringClass shouty_resolved;
	const bool shouty_ok = WWPlatform::Path::Resolve(shouty.c_str(), shouty_resolved, false);
	std::printf("         Resolve(\"%s\") -> %s\n", shouty.c_str(),
		shouty_ok ? shouty_resolved.Peek_Buffer() : "(no match)");
	Check(shouty_ok, "Resolve() is case insensitive component by component");
#endif
	const std::string cursors = std::string(data) + "\\Data\\Cursors";
	const int ani_count = Count(cursors.c_str(), "*.ani");
	std::printf("         Enumerate(\"%s\", \"*.ani\") -> %d entries\n", cursors.c_str(),
		ani_count);
	Check(ani_count > 0, "Enumerate() accepts a backslash separated directory");
	/*
	**	Win32's FindFirstFile() returns INVALID_HANDLE_VALUE when a directory exists but nothing
	**	in it matches, so a false return here means "no matches", not "no such directory" - the
	**	POSIX side reproduces that rather than distinguishing the two.
	*/
	const int nothing = Count(cursors.c_str(), "*.no-such-extension");
	std::printf("         Enumerate(\"%s\", \"*.no-such-extension\") -> %s\n", cursors.c_str(),
		nothing < 0 ? "false, as FindFirstFile() is for an empty match set" : "true");
	Check(nothing < 0, "an empty match set is a false return, as it is on Win32");

#ifndef _WIN32
	/*
	**	Whether the volume itself is case insensitive decides whether Resolve()'s component walk
	**	ever runs: if the shouty spelling opens directly, the fallback the Linux build depends on
	**	is not being exercised by any of the checks above.
	*/
	const std::string shouty_dir = std::string(data) + "/DATA";
	const bool volume_folds_case = ::access(shouty_dir.c_str(), F_OK) == 0;
	std::printf("\nvolume: %s\n", volume_folds_case
		? "case insensitive - the literal spelling opens, so Resolve()'s case folding walk is "
		  "never reached"
		: "case sensitive - Resolve()'s case folding walk is what makes the spellings above work");
#endif

#ifdef ZH_PATH_PROBE_WIN32_SEAM
	Measure_Win32_Seam(data);
#endif

	std::printf("\nchecks failed: %d\n", Failures);
	return Failures == 0 ? 0 : 1;
}
