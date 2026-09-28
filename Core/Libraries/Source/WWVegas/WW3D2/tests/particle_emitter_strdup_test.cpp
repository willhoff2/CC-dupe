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
 *                                                                                              *
 *  Copying a particle emitter, or its definition, must not duplicate a null string.            *
 *                                                                                              *
 *  WHY THIS EXISTS. WW3D2 duplicates its owned C strings with `::_strdup`, and six of those    *
 *  call sites -- in three classes -- passed a pointer that is routinely null, one of them      *
 *  always. `_strdup(nullptr)` reaches `strlen`, which reads address zero: on Apple's libc and  *
 *  on glibc that is a SIGSEGV. The 32-bit MSVC CRT the retail build used returned nullptr      *
 *  instead, so the defect was invisible on Windows and has been latent since the source        *
 *  release (3d0ee53a0). It is an upstream bug, not a port regression. The measured crash was:  *
 *                                                                                              *
 *      _platform_strlen <- strdup <- ParticleEmitterClass::ParticleEmitterClass(const &)       *
 *      <- ParticleEmitterClass::Clone() <- W3DRenderObjectSnapshot::update                     *
 *      <- W3DGhostObject::snapShot <- Object::getShroudedStatus <- GameClient::update          *
 *                                                                                              *
 *  -- the fog-of-war ghost path cloning a dying unit's render objects. See                     *
 *  docs/porting/particle-emitter-strdup.md.                                                    *
 *                                                                                              *
 *  WHAT THIS MEASURES. `ParticleEmitterClass` cannot be built standalone: its constructor      *
 *  needs a `ParticleBufferClass`, and both it and `RenderObjClass` would have to be stubbed    *
 *  vtable and all -- 89 undefined symbols including two full render-object vtables. Its        *
 *  definition class carries the identical defect and is a plain class with no base, so THAT is *
 *  what this file drives, for real, against the real part_ldr.cpp:                             *
 *                                                                                              *
 *    * a default-constructed `ParticleEmitterDefClass` has m_pName and m_pUserString null      *
 *      (part_ldr.cpp:71, :73), and its copy constructor runs `operator=`, which feeds both     *
 *      straight back into `Set_Name` and `Set_User_String` (part_ldr.cpp:161-162). Before the  *
 *      fix that copy is an unconditional segfault; this file runs it in a forked child so the  *
 *      crash is a legible failure rather than a dead harness.                                  *
 *    * `_strdup(nullptr)` really does fault here, and `_strdup("x")` really does not -- the    *
 *      platform fact that made Windows and macOS disagree, measured rather than assumed.       *
 *    * the guarded form yields nullptr for nullptr and an independently allocated, byte-exact  *
 *      copy otherwise. Nothing about the non-null case changes, which is what keeps the        *
 *      Windows replay gate valid for code in the render path.                                  *
 *                                                                                              *
 *  `ParticleEmitterClass`'s own two call sites, and `AggregateDefClass::Set_Name` (which has   *
 *  the same defect by the same route -- agg_def.h:108), are covered by the source scan in      *
 *  scripts/native-particle-emitter-strdup-test.py, the regression gate for the sites no        *
 *  standalone link can reach.                                                                  *
 *                                                                                              *
 *  Run through scripts/native-particle-emitter-strdup-test.py.                                 *
 *                                                                                              *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#include "part_ldr.h"

#include "WWLib/chunkio.h"
#include "assetmgr.h"
#include "texture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#endif

/*
**	LINK STUBS. part_ldr.cpp references chunk IO, the W3D memory pool glue, two preset shaders,
**	the asset manager singleton and `ParticleEmitterClass::Create_From_Definition`. None of them
**	is on the path this file drives -- constructing and copying a definition touches no file, no
**	pool-allocated class and no emitter -- so these exist only so the emitted references resolve.
**
**	They are defined here rather than by compiling chunkio.cpp, assetmgr.cpp, texture.cpp and
**	part_emt.cpp beside the test because those four drag in the whole renderer, and because the
**	two toolchains disagree about which unused references survive to the link: ld64 discards
**	them, GNU ld on the clang-14 CI runner does not. Defining them keeps the test standalone on
**	both. The three `lstr*` entry points are the same story -- part_ldr.cpp calls them only from
**	Set_Texture_Filename and Save_W3D.
*/
void *createW3DMemPool(const char *, int)
{
	return NULL;
}

void *allocateFromW3DMemPool(void *, int size)
{
	return ::malloc(size);
}

void freeFromW3DMemPool(void *, void *block)
{
	::free(block);
}

bool ChunkLoadClass::Open_Chunk()
{
	return false;
}

bool ChunkLoadClass::Close_Chunk()
{
	return false;
}

unsigned int ChunkLoadClass::Cur_Chunk_ID()
{
	return 0;
}

unsigned int ChunkLoadClass::Read(void *, unsigned int)
{
	return 0;
}

bool ChunkSaveClass::Begin_Chunk(unsigned int)
{
	return false;
}

bool ChunkSaveClass::End_Chunk()
{
	return false;
}

unsigned int ChunkSaveClass::Write(const void *, unsigned int)
{
	return 0;
}

ShaderClass ShaderClass::_PresetAdditiveSpriteShader;
ShaderClass ShaderClass::_PresetAlphaSpriteShader;

WW3DAssetManager *WW3DAssetManager::TheInstance = NULL;

void W3dUtilityClass::Convert_Shader(const ShaderClass &, W3dShaderStruct *)
{
}

ParticleEmitterClass *ParticleEmitterClass::Create_From_Definition(const ParticleEmitterDefClass &)
{
	return NULL;
}

extern "C" {
char *lstrcpyA(char *dest, const char *src)
{
	return ::strcpy(dest, src);
}

char *lstrcpynA(char *dest, const char *src, int count)
{
	if (count > 0) {
		::strncpy(dest, src, (size_t)count - 1);
		dest[count - 1] = '\0';
	}
	return dest;
}

int lstrlenA(const char *text)
{
	return (text != NULL) ? (int)::strlen(text) : 0;
}
}


static int _Failures = 0;
static int _Checks = 0;

static void Check(bool condition, const char *what)
{
	_Checks++;
	if (!condition) {
		_Failures++;
		printf("FAIL: %s\n", what);
	}
}


#ifndef _WIN32

/***********************************************************************************************
 *  Running a body that may segfault, without taking the harness down with it. The child's      *
 *  outcome is the measurement: a signal means the body faulted, and which signal it was is     *
 *  worth printing, because "died on SIGSEGV" is the whole finding.                             *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
struct ChildOutcome
{
	bool	Faulted;			// killed by a signal
	int	Signal;			// which one, when Faulted
	int	ExitCode;		// when it exited normally
};

static ChildOutcome Run_In_Child(void (*body)())
{
	ChildOutcome outcome = { false, 0, -1 };

	::fflush(stdout);
	const pid_t child = ::fork();
	if (child == 0) {
		body();
		::_exit(0);
	}
	if (child < 0) {
		::perror("fork");
		outcome.ExitCode = -1;
		return outcome;
	}

	int status = 0;
	while (::waitpid(child, &status, 0) < 0) {
		// EINTR only; nothing else can happen to a child we just forked.
	}
	if (WIFSIGNALED(status)) {
		outcome.Faulted = true;
		outcome.Signal = WTERMSIG(status);
	} else if (WIFEXITED(status)) {
		outcome.ExitCode = WEXITSTATUS(status);
	}
	return outcome;
}


/***********************************************************************************************
 *  The platform fact. `_strdup(nullptr)` is what the MSVC CRT tolerated and a POSIX libc does  *
 *  not; every other check in this file only matters because of it. Measured, not assumed --    *
 *  this is the one assertion that would read differently on Windows, which is why the whole    *
 *  suite is behind #ifndef _WIN32.                                                             *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
static void Strdup_Null_Body()
{
	// Through a volatile pointer so the compiler cannot fold the call away as UB.
	const char *volatile nothing = NULL;
	char *result = ::_strdup(nothing);
	// Not reached on a POSIX libc. Kept so the call is live, and so a libc that DID return null
	// shows up as "exited 7" rather than as a silent pass.
	::_exit(result == NULL ? 7 : 8);
}

static void Test_Platform_Strdup()
{
	const ChildOutcome outcome = Run_In_Child(Strdup_Null_Body);
	if (outcome.Faulted) {
		printf("measured: ::_strdup(nullptr) died on signal %d (%s)\n",
			outcome.Signal, ::strsignal(outcome.Signal));
	} else {
		printf("measured: ::_strdup(nullptr) returned, child exit %d\n", outcome.ExitCode);
	}
	Check(outcome.Faulted,
		"::_strdup(nullptr) faults on this libc, so an unguarded call site is a crash");

	// The other half of the same fact: duplicating a real string is fine, so the guard is the
	// only thing that needed to change.
	char *copy = ::_strdup("ParticleEmitter");
	Check(copy != NULL && ::strcmp(copy, "ParticleEmitter") == 0,
		"::_strdup of a non-null string still copies it");
	::free(copy);
}


/***********************************************************************************************
 *  The guard's own behaviour, stated as the invariant the four fixed call sites now hold:      *
 *  null in, null out; anything else duplicated exactly into fresh storage. The second half is  *
 *  what keeps Windows behaviour unchanged -- this code is in the render path and the replay    *
 *  gate compares against Windows, so the fix had to be null-safety and nothing else.           *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
static char *Guarded_Strdup(const char *text)
{
	return (text != NULL) ? ::_strdup(text) : NULL;
}

static void Test_Guarded_Duplication()
{
	Check(Guarded_Strdup(NULL) == NULL, "the guarded form yields null for null");

	const char *original = "ParticleEmitter";
	char *copy = Guarded_Strdup(original);
	Check(copy != NULL, "the guarded form duplicates a non-null string");
	if (copy != NULL) {
		Check(::strcmp(copy, original) == 0, "the duplicate is byte-exact");
		Check(copy != original, "the duplicate is independently allocated, so ::free owns it");
		::free(copy);
	}

	// The empty string is not null, and must still round-trip: Set_Name("") has to keep meaning
	// "a name that is empty" rather than becoming "no name".
	char *empty = Guarded_Strdup("");
	Check(empty != NULL && empty[0] == '\0', "the guarded form keeps the empty string non-null");
	::free(empty);
}


/***********************************************************************************************
 *  The real defect, in real engine code. A default-constructed definition has both owned       *
 *  strings null; copying it runs part_ldr.cpp's operator=, which hands each of them back to    *
 *  Set_Name / Set_User_String. Before the fix this child dies on SIGSEGV inside strlen.        *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
static void Copy_Default_Definition_Body()
{
	ParticleEmitterDefClass source;
	ParticleEmitterDefClass copy(source);		// operator= -> Set_Name(nullptr), Set_User_String(nullptr)

	// A wrong answer must not look like a crash, so distinguish the two.
	if (copy.Get_Name() != NULL || copy.Get_User_String() != NULL) {
		::_exit(9);
	}
}

static void Set_Null_On_Definition_Body()
{
	// The two setters on their own, with the argument operator= passes them. Set_User_String is
	// the one with no other caller that can supply a non-null value: ParticleEmitterClass's
	// Get_User_String() is `return nullptr` (part_emt.h:221) and there is no setter for it.
	ParticleEmitterDefClass definition;
	definition.Set_Name(NULL);
	definition.Set_User_String(NULL);
	if (definition.Get_Name() != NULL || definition.Get_User_String() != NULL) {
		::_exit(9);
	}
}

static void Report_Child(const ChildOutcome &outcome, const char *what)
{
	if (outcome.Faulted) {
		printf("FAIL: %s -- died on signal %d (%s)\n", what, outcome.Signal,
			::strsignal(outcome.Signal));
		_Checks++;
		_Failures++;
		return;
	}
	if (outcome.ExitCode == 9) {
		printf("FAIL: %s -- survived but left a non-null string behind\n", what);
		_Checks++;
		_Failures++;
		return;
	}
	Check(outcome.ExitCode == 0, what);
}

static void Test_Definition_Copy()
{
	Report_Child(Run_In_Child(Copy_Default_Definition_Body),
		"copying a default-constructed ParticleEmitterDefClass does not fault");
	Report_Child(Run_In_Child(Set_Null_On_Definition_Body),
		"Set_Name(nullptr) and Set_User_String(nullptr) do not fault");

	// And in-process, so a regression is a crash of this binary and not only of a child: the
	// same copy, with the result inspected.
	ParticleEmitterDefClass source;
	Check(source.Get_Name() == NULL, "a fresh definition has no name");
	Check(source.Get_User_String() == NULL, "a fresh definition has no user string");

	ParticleEmitterDefClass copy(source);
	Check(copy.Get_Name() == NULL, "the copy of a nameless definition is still nameless");
	Check(copy.Get_User_String() == NULL, "the copy has no user string either");

	// The non-null case, which must be unchanged: the copy owns its own storage.
	ParticleEmitterDefClass named;
	named.Set_Name("FireSmall");
	named.Set_User_String("user");
	ParticleEmitterDefClass named_copy(named);
	Check(named_copy.Get_Name() != NULL && ::strcmp(named_copy.Get_Name(), "FireSmall") == 0,
		"a name survives the copy");
	Check(named_copy.Get_User_String() != NULL
			&& ::strcmp(named_copy.Get_User_String(), "user") == 0,
		"a user string survives the copy");
	Check(named_copy.Get_Name() != named.Get_Name(),
		"the copy's name is its own allocation, not the source's pointer");

	// Overwriting a name frees the old one and takes the new; passing null clears it. Both
	// halves of Set_Name have to keep working, since the destructor frees whatever is left.
	named.Set_Name("FireLarge");
	Check(named.Get_Name() != NULL && ::strcmp(named.Get_Name(), "FireLarge") == 0,
		"Set_Name replaces an existing name");
	named.Set_Name(NULL);
	Check(named.Get_Name() == NULL, "Set_Name(nullptr) clears the name");
}

#endif	// !_WIN32


int main()
{
#ifdef _WIN32
	// The defect is invisible here by construction: the CRT's _strdup(nullptr) returns nullptr.
	printf("skipped on Windows: _strdup(nullptr) does not fault on the MSVC CRT\n");
	return 0;
#else
	Test_Platform_Strdup();
	Test_Guarded_Duplication();
	Test_Definition_Copy();

	printf("%d checks, %d failure(s)\n", _Checks, _Failures);
	return (_Failures == 0) ? 0 : 1;
#endif
}
