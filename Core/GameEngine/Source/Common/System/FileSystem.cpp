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

////////////////////////////////////////////////////////////////////////////////
//																																						//
//  (c) 2001-2003 Electronic Arts Inc.																				//
//																																						//
////////////////////////////////////////////////////////////////////////////////

//----------------------------------------------------------------------------
//
//                       Westwood Studios Pacific.
//
//                       Confidential Information
//                Copyright(C) 2001 - All Rights Reserved
//
//----------------------------------------------------------------------------
//
// Project:   WSYS Library
//
// Module:    IO
//
// File name: IO_FileSystem.cpp
//
// Created:   4/23/01
//
//----------------------------------------------------------------------------

//----------------------------------------------------------------------------
//         Includes
//----------------------------------------------------------------------------

#include "PreRTS.h"
#include "Common/file.h"
#include "Common/FileSystem.h"

#include "Common/ArchiveFileSystem.h"
#include "Common/GameAudio.h"
#include "Common/LocalFileSystem.h"
#include "Common/PerfTimer.h"

#include "Lib/PathUtil.h"


DECLARE_PERF_TIMER(FileSystem)

//----------------------------------------------------------------------------
//         Externals
//----------------------------------------------------------------------------



//----------------------------------------------------------------------------
//         Defines
//----------------------------------------------------------------------------



//----------------------------------------------------------------------------
//         Private Types
//----------------------------------------------------------------------------



//----------------------------------------------------------------------------
//         Private Data
//----------------------------------------------------------------------------

#ifndef _WIN32
namespace
{

// TheSuperHackers @port The engine's path spelling is the Windows one: the retail data, the INI
// literals and every path the engine builds itself use '\'. A path crossing into the host
// filesystem is translated on the way in (WWPlatform::Path::Resolve, called from LocalFile::open
// and from the Win32 file API seam), and a name coming back out of a directory listing has to be
// translated the other way, or the caller receives a path spelled in a way it never asked for.
// That is not cosmetic: enumerated names are used as map cache keys, compared against '\' built
// suffixes and written into MapCache.ini, so the spelling is part of the value.
//
// The listing is therefore respelled here, at the one boundary every engine caller goes through,
// rather than in each LocalFileSystem implementation: the two implementations (Win32Device and
// StdDevice) disagree today, and the archive file system has its own spelling again, so the
// guarantee belongs to the interface, not to one backend.
//
// The requested directory is put back verbatim -- it may legitimately contain host separators,
// because the user data path is a host path -- and only the part the listing appended is
// respelled. See docs/porting/path-separator-seam.md.
Bool sameCharIgnoringSeparator(char left, char right)
{
	if (left == right)
	{
		return TRUE;
	}
	return (left == '/' || left == '\\') && (right == '/' || right == '\\');
}

void respellAsRequested(const AsciiString& directory, FilenameList& filenameList)
{
	const Int prefixLength = directory.getLength();
	FilenameList respelled;

	FilenameListIter it = filenameList.begin();
	for (; it != filenameList.end(); ++it)
	{
		const AsciiString& name = *it;
		Int index = 0;

		// Only a name that really is the requested directory plus something keeps the prefix.
		if (name.getLength() >= prefixLength)
		{
			for (; index < prefixLength; ++index)
			{
				if (!sameCharIgnoringSeparator(name.getCharAt(index), directory.getCharAt(index)))
				{
					break;
				}
			}
		}

		AsciiString result;
		if (index == prefixLength)
		{
			result = directory;
		}
		else
		{
			index = 0;
		}

		for (; index < name.getLength(); ++index)
		{
			const char c = name.getCharAt(index);
			result.concat(c == '/' ? '\\' : c);
		}

		respelled.insert(result);
	}

	filenameList.swap(respelled);
}

}	// anonymous namespace
#endif	// !_WIN32



//----------------------------------------------------------------------------
//         Public Data
//----------------------------------------------------------------------------

//===============================
// TheFileSystem
//===============================
/**
  *	This is the FileSystem's singleton class. All file access
	* should be through TheFileSystem, unless code needs to use an explicit
	* File or FileSystem derivative.
	*
	* Using TheFileSystem->open and File exclusively for file access, particularly
	* in library or modular code, allows applications to transparently implement
	* file access as they see fit. This is particularly important for code that
	* needs to be shared between applications, such as games and tools.
	*/
//===============================

FileSystem	*TheFileSystem = nullptr;

//----------------------------------------------------------------------------
//         Private Prototypes
//----------------------------------------------------------------------------

//----------------------------------------------------------------------------
//         Private Functions
//----------------------------------------------------------------------------


//----------------------------------------------------------------------------
//         Public Functions
//----------------------------------------------------------------------------


//============================================================================
// FileSystem::FileSystem
//============================================================================

FileSystem::FileSystem()
{

}

//============================================================================
// FileSystem::~FileSystem
//============================================================================

FileSystem::~FileSystem()
{

}

//============================================================================
// FileSystem::init
//============================================================================

void		FileSystem::init()
{
	TheLocalFileSystem->init();
	TheArchiveFileSystem->init();
}

//============================================================================
// FileSystem::update
//============================================================================

void		FileSystem::update()
{
	USE_PERF_TIMER(FileSystem)
	TheLocalFileSystem->update();
	TheArchiveFileSystem->update();
}

//============================================================================
// FileSystem::reset
//============================================================================

void		FileSystem::reset()
{
	USE_PERF_TIMER(FileSystem)
	TheLocalFileSystem->reset();
	TheArchiveFileSystem->reset();
}

//============================================================================
// FileSystem::open
//============================================================================

File*		FileSystem::openFile( const Char *filename, Int access, size_t bufferSize, FileInstance instance )
{
	USE_PERF_TIMER(FileSystem)
	File *file = nullptr;

	if ( TheLocalFileSystem != nullptr )
	{
		if (instance != 0)
		{
			if (TheLocalFileSystem->doesFileExist(filename))
			{
				--instance;
			}
		}
		else
		{
			file = TheLocalFileSystem->openFile( filename, access, bufferSize );

#if ENABLE_FILESYSTEM_EXISTENCE_CACHE
			if (file != nullptr && (file->getAccess() & File::CREATE))
			{
				FastCriticalSectionClass::LockClass lock(m_fileExistMutex);
				FileExistMap::iterator it = m_fileExist.find(FileExistMap::key_type::temporary(filename));
				if (it != m_fileExist.end())
				{
					++it->second.instanceExists;
					if (it->second.instanceDoesNotExist != ~FileInstance(0))
						++it->second.instanceDoesNotExist;
				}
				else
				{
					m_fileExist[filename];
				}
			}
#endif
		}
	}

	if ( (TheArchiveFileSystem != nullptr) && (file == nullptr) )
	{
		// TheSuperHackers @todo Pass 'access' here?
		file = TheArchiveFileSystem->openFile( filename, 0, instance );
	}

	return file;
}

//============================================================================
// FileSystem::doesFileExist
//============================================================================

Bool FileSystem::doesFileExist(const Char *filename, FileInstance instance) const
{
	USE_PERF_TIMER(FileSystem)

#if ENABLE_FILESYSTEM_EXISTENCE_CACHE
	{
		FastCriticalSectionClass::LockClass lock(m_fileExistMutex);
		FileExistMap::const_iterator it = m_fileExist.find(FileExistMap::key_type::temporary(filename));
		if (it != m_fileExist.end())
		{
			// Must test instanceDoesNotExist first!
			if (instance >= it->second.instanceDoesNotExist)
				return FALSE;
			if (instance <= it->second.instanceExists)
				return TRUE;
		}
	}
#endif

	if (TheLocalFileSystem->doesFileExist(filename))
	{
		if (instance == 0)
		{
#if ENABLE_FILESYSTEM_EXISTENCE_CACHE
			{
				FastCriticalSectionClass::LockClass lock(m_fileExistMutex);
				m_fileExist[filename];
			}
#endif
			return TRUE;
		}

		--instance;
	}

	if (TheArchiveFileSystem->doesFileExist(filename, instance))
	{
#if ENABLE_FILESYSTEM_EXISTENCE_CACHE
		{
			FastCriticalSectionClass::LockClass lock(m_fileExistMutex);
			FileExistMap::mapped_type& value = m_fileExist[filename];
			value.instanceExists = max(value.instanceExists, instance);
		}
#endif
		return TRUE;
	}

#if ENABLE_FILESYSTEM_EXISTENCE_CACHE
	{
		FastCriticalSectionClass::LockClass lock(m_fileExistMutex);
		FileExistMap::mapped_type& value = m_fileExist[filename];
		value.instanceDoesNotExist = min(value.instanceDoesNotExist, instance);
	}
#endif
	return FALSE;
}

//============================================================================
// FileSystem::getFileListInDirectory
//============================================================================
void FileSystem::getFileListInDirectory(const AsciiString& directory, const AsciiString& searchName, FilenameList &filenameList, Bool searchSubdirectories) const
{
	USE_PERF_TIMER(FileSystem)
	TheLocalFileSystem->getFileListInDirectory(AsciiString::TheEmptyString, directory, searchName, filenameList, searchSubdirectories);
	TheArchiveFileSystem->getFileListInDirectory(AsciiString::TheEmptyString, directory, searchName, filenameList, searchSubdirectories);

#ifndef _WIN32
	respellAsRequested(directory, filenameList);
#endif
}

//============================================================================
// FileSystem::getFileInfo
//============================================================================
Bool FileSystem::getFileInfo(const AsciiString& filename, FileInfo *fileInfo, FileInstance instance) const
{
	USE_PERF_TIMER(FileSystem)

	// TheSuperHackers @todo Add file info cache?

	if (fileInfo == nullptr) {
		return FALSE;
	}
	memset(fileInfo, 0, sizeof(*fileInfo));

	if (TheLocalFileSystem->getFileInfo(filename, fileInfo)) {
		if (instance == 0) {
			return TRUE;
		}

		--instance;
	}

	if (TheArchiveFileSystem->getFileInfo(filename, fileInfo, instance)) {
		return TRUE;
	}

	return FALSE;
}

//============================================================================
// FileSystem::createDirectory
//============================================================================
Bool FileSystem::createDirectory(AsciiString directory)
{
	USE_PERF_TIMER(FileSystem)
	if (TheLocalFileSystem != nullptr) {
		return TheLocalFileSystem->createDirectory(directory);
	}
	return FALSE;
}

//============================================================================
// FileSystem::normalizePath
//============================================================================
AsciiString FileSystem::normalizePath(const AsciiString& path)
{
	return TheLocalFileSystem->normalizePath(path);
}

//============================================================================
// FileSystem::isPathInDirectory
//============================================================================
Bool FileSystem::isPathInDirectory(const AsciiString& testPath, const AsciiString& basePath)
{
	AsciiString testPathNormalized = TheFileSystem->normalizePath(testPath);
	AsciiString basePathNormalized = TheFileSystem->normalizePath(basePath);

	if (basePathNormalized.isEmpty())
	{
		DEBUG_CRASH(("Unable to normalize base directory path '%s'.", basePath.str()));
		return false;
	}
	else if (testPathNormalized.isEmpty())
	{
		DEBUG_CRASH(("Unable to normalize file path '%s'.", testPath.str()));
		return false;
	}

#ifdef _WIN32
	const char* pathSep = "\\";
#else
	const char* pathSep = "/";
#endif

	if (!basePathNormalized.endsWith(pathSep))
	{
		basePathNormalized.concat(pathSep);
	}

	// Engine paths follow Windows case rules on every host: GameState lower-cases the real map path
	// it reconstructs from a save file, and the native path seam resolves names case-insensitively.
	if (!testPathNormalized.startsWithNoCase(basePathNormalized))
	{
		return false;
	}

	return true;
}

//============================================================================
// FileSystem::removeExtension - Ascii handling variant
//============================================================================
bool FileSystem::removeExtension(AsciiString& path)
{
	if (const Char* ext = getExtension(path.str()))
	{
		path.truncateTo(ext - path.str());
		return true;
	}

	return false;
}

//============================================================================
// FileSystem::removeExtension - Unicode handling variant
//============================================================================
bool FileSystem::removeExtension(UnicodeString& path)
{
	if (const WideChar* ext = getExtension(path.str()))
	{
		path.truncateTo(ext - path.str());
		return true;
	}

	return false;
}
