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
 *  Behaviour test for the Win32 file API seam. The seam links -- the native build proves that   *
 *  -- but linking says nothing about whether FindFirstFile() enumerates what the asset scan      *
 *  expects, or whether GetFullPathName() honours Win32's buffer size protocol. Those were        *
 *  argued from documentation and call sites; this runs them against a real directory tree and    *
 *  asserts the answers.                                                                        *
 *                                                                                             *
 *  Windows is the oracle for every expectation here: each assertion is what kernel32 does,      *
 *  taken from the documented behaviour the engine's call sites depend on. The test also builds   *
 *  and passes on Windows, where the real API is the implementation, so a divergence shows up as *
 *  a failure rather than as a difference nobody looks at. See                                    *
 *  docs/porting/win32-file-api-seam.md.                                                        *
 *                                                                                             *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#include <windows.h>

#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <filesystem>
#include <set>
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

static void Check_Equal(const std::string & actual, const std::string & expected, const char * what)
{
	_Checks++;
	if (actual != expected) {
		_Failures++;
		printf("FAIL: %s\n  expected: %s\n  actual:   %s\n", what, expected.c_str(), actual.c_str());
	}
}


/*
**	The tree the assertions run against: a couple of .big archives the way the retail data spells
**	them, a mixed case name, a file with no extension, and a subdirectory.
*/
static std::string _Root;

static bool Write_File(const std::string & path)
{
	FILE * file = fopen(path.c_str(), "wb");
	if (file == nullptr) {
		return false;
	}
	fputs("x", file);
	fclose(file);
	return true;
}


/*
**	The scaffolding deliberately uses std::filesystem rather than the seam: a test that built its
**	own fixture out of the API under test would pass by agreeing with itself.
*/
static bool Make_Tree()
{
	std::error_code error;
	std::filesystem::path root = std::filesystem::temp_directory_path(error)
		/ "win32-file-api-test";
	if (error) {
		return false;
	}

	std::filesystem::remove_all(root, error);
	if (!std::filesystem::create_directories(root / "Maps", error)) {
		printf("FAIL: could not create the test tree: %s\n", error.message().c_str());
		return false;
	}
	_Root = root.string();

	return Write_File(_Root + "/INI.big")
		&& Write_File(_Root + "/W3D.big")
		&& Write_File(_Root + "/English.big")
		&& Write_File(_Root + "/ReadMe")
		&& Write_File(_Root + "/MixedCase.INI");
}


static void Remove_Tree()
{
	std::error_code error;
	std::filesystem::remove_all(_Root, error);
}


/*
**	Everything FindFirstFile()/FindNextFile() returns for a pattern, plus each entry's directory
**	flag, so the assertions can talk about the set rather than about an order Win32 does not
**	promise.
*/
struct EntryStruct
{
	std::string Name;
	bool Is_Directory;

	bool operator<(const EntryStruct & other) const { return Name < other.Name; }
};

static std::vector<EntryStruct> Enumerate(const std::string & pattern)
{
	std::vector<EntryStruct> entries;
	WIN32_FIND_DATAA data;
	memset(&data, 0, sizeof(data));

	HANDLE handle = FindFirstFileA(pattern.c_str(), &data);
	if (handle == INVALID_HANDLE_VALUE) {
		return entries;
	}

	do {
		EntryStruct entry;
		entry.Name = data.cFileName;
		entry.Is_Directory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
		entries.push_back(entry);
	} while (FindNextFileA(handle, &data));

	FindClose(handle);
	std::sort(entries.begin(), entries.end());
	return entries;
}


static std::set<std::string> Names(const std::vector<EntryStruct> & entries)
{
	std::set<std::string> names;
	for (size_t index = 0; index < entries.size(); index++) {
		names.insert(entries[index].Name);
	}
	return names;
}


static void Test_Enumeration()
{
	/*
	**	The asset scan's own pattern. Only the archives, and by their real spelling -- the engine
	**	concatenates the name onto a directory afterwards, so a wrong case here becomes a failed
	**	open on a case sensitive filesystem.
	*/
	std::set<std::string> big = Names(Enumerate(_Root + "\\*.big"));
	Check(big == std::set<std::string>({"English.big", "INI.big", "W3D.big"}),
		"*.big matches exactly the three archives, spelled as they are on disk");

	/*
	**	"*.*" is every entry on Windows, including the ones with no dot in the name and including
	**	"." and "..", which the engine's directory walks skip explicitly.
	*/
	std::vector<EntryStruct> all = Enumerate(_Root + "\\*.*");
	std::set<std::string> all_names = Names(all);
	Check(all_names.count(".") == 1 && all_names.count("..") == 1,
		"*.* yields the . and .. entries the directory walks filter out");
	Check(all_names.count("ReadMe") == 1,
		"*.* yields an extensionless name (Win32 does not require the dot to be present)");
	Check(all_names.count("Maps") == 1, "*.* yields the subdirectory");

	for (size_t index = 0; index < all.size(); index++) {
		if (all[index].Name == "Maps" || all[index].Name == "." || all[index].Name == "..") {
			Check(all[index].Is_Directory,
				"FILE_ATTRIBUTE_DIRECTORY is set on the directory entries");
		}
		if (all[index].Name == "INI.big") {
			Check(!all[index].Is_Directory, "FILE_ATTRIBUTE_DIRECTORY is clear on a file entry");
		}
	}

	/*
	**	"*." is Win32's "no extension" pattern, and the one place a naive fnmatch() translation is
	**	wrong: it matches ReadMe and nothing that has a dot in it.
	*/
	std::set<std::string> no_extension = Names(Enumerate(_Root + "\\*."));
	Check(no_extension.count("ReadMe") == 1, "*. matches the extensionless name");
	Check(no_extension.count("INI.big") == 0, "*. does not match a name with an extension");

	/*
	**	A pattern that matches nothing fails rather than enumerating nothing, and reports
	**	ERROR_FILE_NOT_FOUND, which is what the callers check before falling back.
	*/
	WIN32_FIND_DATAA data;
	memset(&data, 0, sizeof(data));
	HANDLE handle = FindFirstFileA((_Root + "\\*.absent").c_str(), &data);
	Check(handle == INVALID_HANDLE_VALUE, "a pattern matching nothing returns INVALID_HANDLE_VALUE");
	Check(GetLastError() == ERROR_FILE_NOT_FOUND,
		"a pattern matching nothing sets ERROR_FILE_NOT_FOUND");
	if (handle != INVALID_HANDLE_VALUE) {
		FindClose(handle);
	}

	/*
	**	A name with no wildcard in it enumerates just that one entry.
	*/
	std::set<std::string> single = Names(Enumerate(_Root + "\\INI.big"));
	Check(single == std::set<std::string>({"INI.big"}), "a literal name enumerates only itself");

	/*
	**	The retail data is spelled inconsistently and the engine's literals do not always match the
	**	filesystem's case. On Windows and on macOS's default filesystem that is invisible; the seam
	**	makes it invisible on a case sensitive filesystem too, which is the behaviour this asserts.
	*/
	std::set<std::string> wrong_case = Names(Enumerate(_Root + "\\ini.BIG"));
	Check(wrong_case.size() == 1, "a differently cased literal still finds the file");
	if (wrong_case.size() == 1) {
		Check_Equal(*wrong_case.begin(), "INI.big",
			"the enumerated name is the on-disk spelling, not the caller's");
	}
}


static void Test_Attributes_And_Size()
{
	DWORD attributes = GetFileAttributesA((_Root + "\\Maps").c_str());
	Check(attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0,
		"GetFileAttributes reports a directory as one");

	attributes = GetFileAttributesA((_Root + "\\INI.big").c_str());
	Check(attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0,
		"GetFileAttributes reports a file as not a directory");

	Check(GetFileAttributesA((_Root + "\\absent.big").c_str()) == INVALID_FILE_ATTRIBUTES,
		"GetFileAttributes fails on a name that does not exist");

	WIN32_FIND_DATAA data;
	memset(&data, 0, sizeof(data));
	HANDLE handle = FindFirstFileA((_Root + "\\INI.big").c_str(), &data);
	Check(handle != INVALID_HANDLE_VALUE, "FindFirstFile opens on a literal name");
	if (handle != INVALID_HANDLE_VALUE) {
		Check(data.nFileSizeLow == 1 && data.nFileSizeHigh == 0,
			"the find data carries the file's size");
		/*
		**	The engine compares these against each other (verchk.cpp) rather than converting them
		**	to a date, so what matters is that they are populated and ordered, not their epoch.
		*/
		ULARGE_INTEGER write;
		write.LowPart = data.ftLastWriteTime.dwLowDateTime;
		write.HighPart = data.ftLastWriteTime.dwHighDateTime;
		Check(write.QuadPart != 0, "the find data carries a non-zero last write FILETIME");
		FindClose(handle);
	}
}


static void Test_Full_Path_Name()
{
	/*
	**	Win32's two call buffer protocol, which Win32LocalFileSystem::normalizePath() uses exactly
	**	this way: a null buffer returns the required size *including* the terminator, and a
	**	sufficient buffer returns the length *excluding* it.
	*/
	std::string absolute = _Root + "\\Maps\\..\\INI.big";
	DWORD required = GetFullPathNameA(absolute.c_str(), 0, nullptr, nullptr);
	Check(required > 0, "GetFullPathName reports a required size for a null buffer");

	std::vector<char> buffer(required + 16, '\0');
	char * file_part = nullptr;
	DWORD length = GetFullPathNameA(absolute.c_str(), (DWORD)buffer.size(), &buffer[0], &file_part);
	Check(length == required - 1,
		"the required size includes the terminator and the written length does not");

	std::string normalized(&buffer[0]);
	Check(normalized.find("..") == std::string::npos, "GetFullPathName folds .. away");
	Check(normalized.find("INI.big") != std::string::npos, "the folded path keeps the file name");
	Check(file_part != nullptr && strcmp(file_part, "INI.big") == 0,
		"the file part points at the last component");

	/*
	**	Normalisation is lexical: a path that does not exist still normalises. The engine calls
	**	this on a path it is about to create.
	*/
	std::string absent = _Root + "\\Maps\\.\\does-not-exist\\..\\file.txt";
	std::vector<char> absent_buffer(1024, '\0');
	length = GetFullPathNameA(absent.c_str(), (DWORD)absent_buffer.size(), &absent_buffer[0],
		nullptr);
	Check(length > 0, "GetFullPathName normalises a path that does not exist");
	Check(std::string(&absent_buffer[0]).find("does-not-exist") == std::string::npos,
		"a . and a .. fold away even when the components are absent from the disk");

	/*
	**	A relative path is resolved against the current directory, so the result is absolute. That
	**	is what makes FileSystem::isPathInDirectory()'s prefix comparison meaningful.
	*/
	std::vector<char> relative_buffer(1024, '\0');
	length = GetFullPathNameA("relative.txt", (DWORD)relative_buffer.size(), &relative_buffer[0],
		nullptr);
	Check(length > 0, "GetFullPathName accepts a relative path");
	std::string relative(&relative_buffer[0]);
#ifdef _WIN32
	Check(relative.size() > 2 && relative[1] == ':',
		"a relative path comes back with a drive letter");
#else
	Check(!relative.empty() && relative[0] == '/', "a relative path comes back absolute");
#endif

	/*
	**	Too small a buffer reports the size it wants and leaves the caller to retry, rather than
	**	truncating.
	*/
	char tiny[2] = {'\0', '\0'};
	DWORD wanted = GetFullPathNameA(absolute.c_str(), sizeof(tiny), tiny, nullptr);
	Check(wanted > sizeof(tiny), "too small a buffer returns the size it needs");
}


static void Test_File_Operations()
{
	std::string source = _Root + "\\INI.big";
	std::string destination = _Root + "\\Copied.big";

	Check(CopyFileA(source.c_str(), destination.c_str(), TRUE) != 0, "CopyFile copies a file");
	Check(GetFileAttributesA(destination.c_str()) != INVALID_FILE_ATTRIBUTES,
		"the copy exists afterwards");
	Check(CopyFileA(source.c_str(), destination.c_str(), TRUE) == 0,
		"CopyFile with fail_if_exists refuses to overwrite");
	Check(CopyFileA(source.c_str(), destination.c_str(), FALSE) != 0,
		"CopyFile without fail_if_exists overwrites");
	Check(DeleteFileA(destination.c_str()) != 0, "DeleteFile removes a file");
	Check(GetFileAttributesA(destination.c_str()) == INVALID_FILE_ATTRIBUTES,
		"the file is gone afterwards");
	Check(DeleteFileA(destination.c_str()) == 0, "DeleteFile fails on a name that does not exist");

	std::string directory = _Root + "\\Made";
	Check(CreateDirectoryA(directory.c_str(), nullptr) != 0, "CreateDirectory makes a directory");
	Check((GetFileAttributesA(directory.c_str()) & FILE_ATTRIBUTE_DIRECTORY) != 0,
		"the created directory is there and is a directory");
	Check(CreateDirectoryA(directory.c_str(), nullptr) == 0,
		"CreateDirectory fails when the directory is already there");
	Check(GetLastError() == ERROR_ALREADY_EXISTS,
		"the second CreateDirectory reports ERROR_ALREADY_EXISTS");
}


static void Test_Current_Directory()
{
	DWORD required = GetCurrentDirectoryA(0, nullptr);
	Check(required > 0, "GetCurrentDirectory reports a required size for a null buffer");

	std::vector<char> buffer(required + 16, '\0');
	DWORD length = GetCurrentDirectoryA((DWORD)buffer.size(), &buffer[0]);
	Check(length == required - 1,
		"GetCurrentDirectory's required size includes the terminator and its length does not");
	Check(strlen(&buffer[0]) == length, "the written directory is terminated at the reported length");
}


int main()
{
	if (!Make_Tree()) {
		printf("FAIL: could not create the test tree\n");
		return 1;
	}

	printf("== win32 file API seam, tree at %s\n", _Root.c_str());
	Test_Enumeration();
	Test_Attributes_And_Size();
	Test_Full_Path_Name();
	Test_File_Operations();
	Test_Current_Directory();
	Remove_Tree();

	printf("%d checks, %d failures\n", _Checks, _Failures);
	return (_Failures == 0) ? 0 : 1;
}
