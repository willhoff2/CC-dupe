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

// sim_probe.cpp
//
// A measurement harness, not part of the game. It links the engine archives the native build
// produces and drives individual simulation-adjacent subsystems directly, so that each one's first
// genuine off-Windows failure is observable without the retail `.big` archives, without a renderer
// and without GameEngine::init().
//
// Nothing here is stubbed or reimplemented: every mode calls the engine's own code and prints what
// the engine returned. When a mode fails, the failure is the result.
//
// Modes:
//   chunks   <file>              walk a DataChunk file's chunk table and structure
//   mapcache <dir>               MapCache::updateCache -- the engine's own .map header parse
//   mapcachekeys <dir> [name..]  MapCache.ini read back, then looked up by the given map names
//   filecrc  <file> [runs]       the engine CRC class over a file, repeatedly
//   xfercrc  <file> [runs]       XferCRC (the desync check's CRC) over the same bytes
//   replayhdr <file>             RecorderClass::readReplayHeader over a retail replay

#include "PreRTS.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>

#include "Common/ArchiveFileSystem.h"
#include "Common/CriticalSection.h"
#include "Common/crc.h"
#include "Common/DataChunk.h"
#include "Common/Dict.h"
#include "Common/FileSystem.h"
#include "Common/GameMemory.h"
#include "Common/GlobalData.h"
#include "Common/INI.h"
#include "Common/LocalFileSystem.h"
#include "Common/NameKeyGenerator.h"
#include "Common/Recorder.h"
#include "Common/Xfer.h"
#include "Common/XferCRC.h"
#include "Common/ThingFactory.h"
#include "GameClient/GameText.h"
#include "GameClient/MapUtil.h"
#include "StdDevice/Common/StdBIGFileSystem.h"
#include "StdDevice/Common/StdLocalFileSystem.h"
#include "Win32Device/Common/Win32LocalFileSystem.h"

static CriticalSection critSec1, critSec2, critSec3, critSec4, critSec5;

// The engine expects its entry point to supply these; the game's entry-point archive is deliberately
// not linked here, so the harness repeats the spellings PlatformMain.cpp uses.
const Char *g_strFile = "data\\Generals.str";
const Char *g_csfFile = "data\\%s\\Generals.csf";
// Only referenced in the debug configuration, where DebugInit() names the log file with it.
const char *gAppPrefix = "sim_";

// Which LocalFileSystem implementation the harness brings up. The game's own factory,
// Win32GameEngine::createLocalFileSystem(), returns Win32LocalFileSystem on every platform, so
// "win32" is what the game actually runs and "std" is what this harness used to assume
// unconditionally. Both are compiled into the native build and they do different things with a
// Windows spelled path, so being able to run the same mode through either is how that difference
// gets measured instead of argued about. See docs/porting/path-separator-seam.md.
static LocalFileSystem *createSelectedLocalFileSystem(void)
{
	const char *choice = getenv("SIM_PROBE_LOCALFS");

	if (choice != nullptr && strcmp(choice, "win32") == 0)
	{
		printf("localFileSystem=win32 (the implementation the game's factory returns)\n");
		return MSGNEW("SimProbe") Win32LocalFileSystem;
	}

	printf("localFileSystem=std\n");
	return MSGNEW("SimProbe") StdLocalFileSystem;
}

// The engine's own file system, in the order GameEngine::init brings it up. The archive file system
// finds whatever `.big` archives are in the working directory and is content with none, so a missing
// retail archive is not a reason a mode here cannot run.
static void bringUpFileSystem(void)
{
	TheAsciiStringCriticalSection = &critSec1;
	TheUnicodeStringCriticalSection = &critSec2;
	TheDmaCriticalSection = &critSec3;
	TheMemoryPoolCriticalSection = &critSec4;
	TheDebugLogCriticalSection = &critSec5;

	initMemoryManager();

	TheFileSystem = MSGNEW("SimProbe") FileSystem;
	TheNameKeyGenerator = MSGNEW("SimProbe") NameKeyGenerator;
	TheNameKeyGenerator->init();
	TheLocalFileSystem = createSelectedLocalFileSystem();
	TheLocalFileSystem->init();
	TheArchiveFileSystem = MSGNEW("SimProbe") StdBIGFileSystem;
	TheArchiveFileSystem->init();
	TheFileSystem->init();
}

//----------------------------------------------------------------------------------------------
// chunks: the structural half of the DataChunk reader.
//
// DataChunkTableOfContents::read parses the file's label table, then every chunk header is walked
// with the engine's own openDataChunk/closeDataChunk. closeDataChunk seeks by the recorded chunk
// size, so a wrong header layout or a wrong string read shows up immediately as a bogus label, a
// bogus size or a walk that does not land on the next chunk. No field inside a chunk is read, so
// this isolates the container from the per-field types.
//----------------------------------------------------------------------------------------------
static Int walkChunks(DataChunkInput &file, Int depth, Int *count)
{
	while (!file.atEndOfFile())
	{
		DataChunkVersionType ver = 0;
		AsciiString label = file.openDataChunk(&ver);
		if (label.isEmpty())
			break;

		++*count;
		printf("%*s%-28s version=%-4u dataSize=%u\n", depth * 2, "", label.str(),
			(UnsignedInt)ver, file.getChunkDataSize());

		// Only recurse where the format nests; a chunk whose payload is fields, not chunks, would
		// be misread as chunks. The map format's top-level chunks are the interesting ones, and
		// their sizes are what the walk verifies.
		file.closeDataChunk();
	}
	return depth;
}

static int modeChunks(const char *path)
{
	CachedFileInputStream stream;
	if (!stream.open(AsciiString(path)))
	{
		printf("RESULT chunks open=FAIL path=%s\n", path);
		return 2;
	}
	printf("stream opened\n");

	ChunkInputStream *pStrm = &stream;
	DataChunkInput file(pStrm);
	printf("isValidFileType=%s\n", file.isValidFileType() ? "TRUE" : "FALSE");

	Int count = 0;
	walkChunks(file, 0, &count);
	printf("RESULT chunks topLevelChunks=%d\n", count);
	return count > 0 ? 0 : 3;
}

//----------------------------------------------------------------------------------------------
// mapcache: the engine's own map header parse, through the path -buildmapcache uses.
//
// MapCache::updateCache() reads `Maps/<name>/<name>.map` relative to the working directory with
// DataChunkInput, then keeps each map's metadata: the player count derived from the waypoints, the
// file CRC, the extent, the display-name tag out of the world Dict. Every one of those is a value
// the parse has to get right, so a silently wrong parse shows up as a wrong number, not a crash.
//----------------------------------------------------------------------------------------------
static int modeMapCache(const char *dir)
{
	if (chdir(dir) != 0)
	{
		printf("RESULT mapcache chdir=FAIL dir=%s\n", dir);
		return 2;
	}

	TheWritableGlobalData = MSGNEW("SimProbe") GlobalData;
	TheWritableGlobalData->m_buildMapCache = TRUE;

	// The cache reads a localized display name per map, and GameTextManager::fetch asserts if the
	// string manager was never brought up, so it is brought up here for real. Its string table lives
	// in the retail archives; when those are absent the engine reports that as an init failure, which
	// is printed and not swallowed -- init() has already marked itself initialized by then, so the
	// maps still parse and every field except the localized display name is still measured.
	TheGameText = CreateGameTextInterface();
	try
	{
		TheGameText->init();
		printf("gameText init=ok\n");
	}
	catch (...)
	{
		printf("gameText init=FAILED (no string table; display names will be missing)\n");
	}

	// The map's object chunk asks the thing factory for each object's template, so the factory has
	// to exist even though no template INI is loaded here: findTemplate then answers "no such
	// template", which is what the cache parse does with an unknown object anyway.
	TheThingFactory = MSGNEW("SimProbe") ThingFactory;
	TheThingFactory->init();

	MapCache cache;
	// INI::parseMapCacheDefinition files what it reads in TheMapCache, so without this the harness
	// could not reach the cache *hit* path at all: every MapCache.ini entry would be parsed into a
	// null cache and every map on disk would look like one the cache had never seen.
	TheMapCache = &cache;
	cache.updateCache();
	printf("cache entries=%u\n", (UnsignedInt)cache.size());

	Int parsed = 0;
	for (MapCache::const_iterator it = cache.begin(); it != cache.end(); ++it)
	{
		const MapMetaData &md = it->second;
		++parsed;
		printf("RESULT map name=%s players=%d multiplayer=%s crc=%08X filesize=%d "
			"extent=(%.2f,%.2f)-(%.2f,%.2f) waypoints=%u nameTag=%s displayName='%ls'\n",
			it->first.str(), md.m_numPlayers, md.m_isMultiplayer ? "yes" : "no", md.m_CRC,
			md.m_filesize, md.m_extent.lo.x, md.m_extent.lo.y, md.m_extent.hi.x,
			md.m_extent.hi.y, (UnsignedInt)md.m_waypoints.size(), md.m_nameLookupTag.str(),
			md.m_displayName.str());
	}
	printf("RESULT mapcache maps=%d\n", parsed);
	TheMapCache = nullptr;
	return parsed > 0 ? 0 : 3;
}

//----------------------------------------------------------------------------------------------
// mapcachekeys: the map cache as a *key* store, without needing a `.map` on disk.
//
// A MapCache.ini is read with the engine's own INI reader and then queried through
// MapCache::findMap with whatever spellings are named on the command line. That isolates the half of
// the path-separator seam that is not an open() failure: a cache key is an identifier, and a lookup
// that misses does not fail loudly, it re-derives metadata or reports the map as absent. Nothing
// retail is required, so this runs anywhere -- see scripts/ci/check-path-separator-keys.py.
//----------------------------------------------------------------------------------------------
static int modeMapCacheKeys(const char *dir, char **names, int nameCount)
{
	if (chdir(dir) != 0)
	{
		printf("RESULT mapcachekeys chdir=FAIL dir=%s\n", dir);
		return 2;
	}

	TheWritableGlobalData = MSGNEW("SimProbe") GlobalData;

	// A user map key begins with this, and canonicalization has to leave that part alone, so the
	// gate needs to see which prefix the run actually resolved.
	printf("userDataDir=%s\n", TheGlobalData->getPath_UserData().str());

	// Reached for any entry that carries a localized name tag, and it asserts if the string manager
	// was never brought up. Its table lives in the retail archives, which this mode does not need,
	// so a failure to read one is reported and the run continues with the tag unresolved.
	TheGameText = CreateGameTextInterface();
	try
	{
		TheGameText->init();
		printf("gameText init=ok\n");
	}
	catch (...)
	{
		printf("gameText init=FAILED (no string table; display names will be missing)\n");
	}

	MapCache cache;
	TheMapCache = &cache;

	INI ini;
	ini.load(AsciiString("Maps\\MapCache.ini"), INI_LOAD_OVERWRITE, nullptr);

	printf("cache entries=%u\n", (UnsignedInt)cache.size());
	for (MapCache::const_iterator it = cache.begin(); it != cache.end(); ++it)
	{
		printf("RESULT entry key=%s multiplayer=%s players=%d\n", it->first.str(),
			it->second.m_isMultiplayer ? "yes" : "no", it->second.m_numPlayers);
	}

	Int missed = 0;
	for (int i = 0; i < nameCount; ++i)
	{
		const MapMetaData *md = cache.findMap(AsciiString(names[i]));
		if (md == nullptr)
		{
			++missed;
			printf("RESULT lookup name=%s found=no\n", names[i]);
			continue;
		}
		printf("RESULT lookup name=%s found=yes multiplayer=%s players=%d official=%s\n",
			names[i], md->m_isMultiplayer ? "yes" : "no", md->m_numPlayers,
			md->m_isOfficial ? "yes" : "no");
	}

	printf("RESULT mapcachekeys entries=%u lookups=%d missed=%d\n", (UnsignedInt)cache.size(),
		nameCount, missed);
	TheMapCache = nullptr;
	return missed == 0 ? 0 : 3;
}

//----------------------------------------------------------------------------------------------
// filecrc / xfercrc: the two CRCs the lock-step desync check is built out of, over bytes whose
// Windows-computed value is recoverable from a retail replay header.
//----------------------------------------------------------------------------------------------
static Bool readWholeFile(const char *path, char **buf, Int *len)
{
	File *fp = TheFileSystem->openFile(path, File::READ | File::BINARY);
	if (fp == nullptr)
		return FALSE;
	*len = fp->size();
	*buf = (char *)malloc(*len);
	Int got = fp->read(*buf, *len);
	fp->close();
	return got == *len;
}

static int modeFileCRC(const char *path, Int runs)
{
	char *buf = nullptr;
	Int len = 0;
	if (!readWholeFile(path, &buf, &len))
	{
		printf("RESULT filecrc read=FAIL path=%s\n", path);
		return 2;
	}

	UnsignedInt first = 0;
	Bool stable = TRUE;
	for (Int i = 0; i < runs; ++i)
	{
		CRC crc;
		crc.clear();
		crc.computeCRC(buf, len);
		UnsignedInt value = crc.get();
		printf("run %d: CRC=%08X\n", i, value);
		if (i == 0)
			first = value;
		else if (value != first)
			stable = FALSE;
	}
	printf("RESULT filecrc bytes=%d runs=%d crc=%08X stable=%s\n", len, runs, first,
		stable ? "yes" : "no");
	free(buf);
	return stable ? 0 : 3;
}

static int modeXferCRC(const char *path, Int runs)
{
	char *buf = nullptr;
	Int len = 0;
	if (!readWholeFile(path, &buf, &len))
	{
		printf("RESULT xfercrc read=FAIL path=%s\n", path);
		return 2;
	}

	UnsignedInt first = 0;
	Bool stable = TRUE;
	for (Int i = 0; i < runs; ++i)
	{
		XferCRC xfer;
		xfer.open("simprobe");
		xfer.xferUser(buf, len);
		UnsignedInt value = xfer.getCRC();
		xfer.close();
		printf("run %d: XferCRC=%08X\n", i, value);
		if (i == 0)
			first = value;
		else if (value != first)
			stable = FALSE;
	}
	printf("RESULT xfercrc bytes=%d runs=%d crc=%08X stable=%s\n", len, runs, first,
		stable ? "yes" : "no");
	free(buf);
	return stable ? 0 : 3;
}

//----------------------------------------------------------------------------------------------
// replayhdr: RecorderClass::readReplayHeader over a retail .rep.
//
// The replay header is the one retail-authored binary layout in the repository, so it is a real
// cross-platform layout oracle: every field in it was written by the 32-bit Windows build.
//----------------------------------------------------------------------------------------------
static Bool stageIntoReplayDir(const char *src, AsciiString *leafOut)
{
	AsciiString dir = TheGlobalData->getPath_UserData();
	AsciiString unix_dir = dir;
	char *p = (char *)unix_dir.str();
	for (; *p; ++p)
		if (*p == '\\')
			*p = '/';

	// Build the directory chain; the engine's createDirectory only makes one level.
	AsciiString accum;
	const char *s = unix_dir.str();
	while (*s)
	{
		const char *slash = strchr(s, '/');
		if (slash == nullptr)
			break;
		accum.concat(AsciiString(std::string(s, slash - s + 1).c_str()));
		mkdir(accum.str(), 0755);
		s = slash + 1;
	}
	AsciiString replays = unix_dir;
	replays.concat("Replays");
	mkdir(replays.str(), 0755);

	const char *leaf = strrchr(src, '/');
	leaf = (leaf != nullptr) ? leaf + 1 : src;
	AsciiString dest = replays;
	dest.concat("/");
	dest.concat(leaf);

	FILE *in = fopen(src, "rb");
	if (in == nullptr)
		return FALSE;
	FILE *out = fopen(dest.str(), "wb");
	if (out == nullptr)
	{
		fclose(in);
		return FALSE;
	}
	char buf[65536];
	size_t got;
	while ((got = fread(buf, 1, sizeof(buf), in)) > 0)
		fwrite(buf, 1, got, out);
	fclose(in);
	fclose(out);
	printf("staged %s -> %s\n", src, dest.str());
	*leafOut = AsciiString(leaf);
	return TRUE;
}

static int modeReplayHeader(const char *path)
{
	TheWritableGlobalData = MSGNEW("SimProbe") GlobalData;
	printf("user data dir = '%s'\n", TheGlobalData->getPath_UserData().str());

	AsciiString leaf;
	if (!stageIntoReplayDir(path, &leaf))
	{
		printf("RESULT replayhdr stage=FAIL path=%s\n", path);
		return 2;
	}

	RecorderClass *recorder = MSGNEW("SimProbe") RecorderClass;
	RecorderClass::ReplayHeader header;
	header.forPlayback = FALSE;
	header.filename = leaf;

	Bool ok = recorder->readReplayHeader(header);
	printf("readReplayHeader returned %s\n", ok ? "TRUE" : "FALSE");
	printf("  frameCount   = %u\n", header.frameCount);
	printf("  versionNumber= %u\n", header.versionNumber);
	printf("  exeCRC       = %08X\n", header.exeCRC);
	printf("  iniCRC       = %08X\n", header.iniCRC);
	printf("  desync=%d quitEarly=%d localPlayerIndex=%d\n", (int)header.desyncGame,
		(int)header.quitEarly, header.localPlayerIndex);
	printf("  replayName   = '%ls' (len %d)\n", header.replayName.str(),
		header.replayName.getLength());
	printf("  versionString= '%ls' (len %d)\n", header.versionString.str(),
		header.versionString.getLength());
	printf("  gameOptions  = '%s'\n", header.gameOptions.str());
	printf("RESULT replayhdr parsed=%s frames=%u options=%s\n", ok ? "yes" : "no",
		header.frameCount, header.gameOptions.isEmpty() ? "empty" : "present");
	return ok ? 0 : 3;
}

//----------------------------------------------------------------------------------------------

static void usage(void)
{
	fprintf(stderr,
		"usage: sim_probe <mode> [args]\n"
		"  chunks    <file>\n"
		"  mapcache  <dir>\n"
		"  mapcachekeys <dir> [map name ...]\n"
		"  filecrc   <file> [runs]\n"
		"  xfercrc   <file> [runs]\n"
		"  replayhdr <file>\n");
}

int main(int argc, char **argv)
{
	if (argc < 3)
	{
		usage();
		return 1;
	}

	setvbuf(stdout, nullptr, _IONBF, 0);
	bringUpFileSystem();

	const char *mode = argv[1];
	if (strcmp(mode, "chunks") == 0)
		return modeChunks(argv[2]);
	if (strcmp(mode, "mapcache") == 0)
		return modeMapCache(argv[2]);
	if (strcmp(mode, "mapcachekeys") == 0)
		return modeMapCacheKeys(argv[2], argv + 3, argc - 3);
	if (strcmp(mode, "filecrc") == 0)
		return modeFileCRC(argv[2], argc > 3 ? atoi(argv[3]) : 3);
	if (strcmp(mode, "xfercrc") == 0)
		return modeXferCRC(argv[2], argc > 3 ? atoi(argv[3]) : 3);
	if (strcmp(mode, "replayhdr") == 0)
		return modeReplayHeader(argv[2]);

	usage();
	return 1;
}
