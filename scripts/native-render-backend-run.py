#!/usr/bin/env python3
"""Link and run the engine's own renderer path off Windows, and report where it stops.

`Core/Libraries/Source/WWVegas/WW3D2/tests/native_render_run.cpp` calls DX8Wrapper::Init,
Get_Render_Device_Count, Set_Render_Device and Begin_Scene/Clear/End_Scene -- the sequence
W3DDisplay::init() performs -- and prints the value the engine returned for each. This script gives
it an executable: it compiles the harness with the flags `scripts/native-build.py` compiled the
engine with (read out of the generated `compile_commands.json`, so the two cannot drift) and links
it against the same archives, with the game's own `main()` removed from a scratch copy of them.

Why not just start the game: the game needs a retail Zero Hour install, which the Linux CI box does
not have, and the null render backend #87 measured was reached only after 20 `.big` archives had
loaded. This runs the renderer half without the data half. Nothing here substitutes for the Apple
Silicon run -- MoltenVK, the CAMetalLayer and the 2.00 backing scale factor are only decidable
there; see docs/porting/renderer-integration.md.

On macOS this chooses Mach-O link flags, the Cocoa/Metal frameworks and MoltenVK's ICD instead of
ELF flags and lavapipe's, and the entry-point rename needs llvm-objcopy and llvm-nm
(`brew install llvm`), because Apple's cctools have no --redefine-sym. That path has now been run on
an Apple Silicon outpost (docs/porting/renderer-integration-arm64.md): the link half was right as
written, and the two things it got wrong were both runtime environment -- Homebrew's MoltenVK ICD
lives under the keg's `etc/vulkan/icd.d`, and the validation layer's dylib is named relatively so
DYLD_LIBRARY_PATH has to carry the keg's lib directory. Both are corrected here.

Usage:
    python3 scripts/native-build.py --level 1 --level 2 --level 3 --level 4 \\
        --with-shims --strict-link            # must run first: this uses its archives
    python3 scripts/native-render-backend-run.py [--keep] [--no-present] [--validation] [--lldb]

The harness's exit code is the number of engine stages that failed, and this script exits with it.
A stage that fails is a finding to be reported, not a reason to retry differently.
"""

import argparse
import importlib.util
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
BUILD_DIR = REPO_ROOT / "build" / "native"
HARNESS = REPO_ROOT / "Core/Libraries/Source/WWVegas/WW3D2/tests/native_render_run.cpp"
# The translation unit whose compile command is reused: same target, same include set, and it is
# the one file in the tree that includes both the D3D8-shaped seam and the spike's header.
FLAG_DONOR = "vulkanrenderbackend.cpp"
# The archive that carries the game's WinMain-equivalent `main()`.
GAME_MAIN_ARCHIVE = "libgeneralsmd_code_main.a"
# lavapipe's manifest is versioned on jammy; the renderer-spike skill documents the same path.
LAVAPIPE_ICD = "/usr/share/vulkan/icd.d/lvp_icd.x86_64.json"
MACOS = sys.platform == "darwin"
# MoltenVK's ICD. Measured on the outpost: `brew install molten-vk` puts it under the keg's
# `etc/vulkan/icd.d`, not the `share/vulkan/icd.d` the Vulkan SDK and every Linux distribution use,
# so both paths this script first guessed were absent and VK_ICD_FILENAMES went unset. The SDK
# layout stays in the list because a machine with the LunarG SDK installed has it there.
MOLTENVK_ICDS = ("/opt/homebrew/opt/molten-vk/etc/vulkan/icd.d/MoltenVK_icd.json",
                 "/usr/local/opt/molten-vk/etc/vulkan/icd.d/MoltenVK_icd.json",
                 "/opt/homebrew/share/vulkan/icd.d/MoltenVK_icd.json",
                 "/usr/local/share/vulkan/icd.d/MoltenVK_icd.json")
# Homebrew's validation-layer manifest names its dylib relatively (`library_path` is
# "libVkLayer_khronos_validation.dylib"), so the loader can only open it if the keg's lib directory
# is on DYLD_LIBRARY_PATH. Without this --validation fails in vkCreateInstance with
# VK_ERROR_LAYER_NOT_PRESENT (-6), which is not a renderer result.
MACOS_LOADER_DIRS = ("/opt/homebrew/lib", "/usr/local/lib")
# The window backend off Linux is platform_window_cocoa.mm and the swapchain surface is a
# CAMetalLayer, so the harness links what those two need.
MACOS_FRAMEWORKS = ("Cocoa", "QuartzCore", "Metal", "IOKit")
# Where Common/GameMemory.h and Common/CriticalSection.h live; PlatformMain.cpp's prologue needs
# them and the WW3D2 target's include set stops below them.
HARNESS_INCLUDES = ("Core/GameEngine/Include", "GeneralsMD/Code/GameEngine/Include")
# The archive carrying GameMemory.cpp's replacement of the global operator new/delete, and the
# mangled names of those replacements. --stdlib-new renames them in the scratch copy so libstdc++'s
# implementations win instead; see the alignment finding in docs/porting/renderer-integration.md.
GAME_MEMORY_ARCHIVE = "libcore_gameengine.a"
GAME_MEMORY_MEMBER = "GameMemory.cpp.o"
GLOBAL_NEW_SYMBOLS = ("_Znwm", "_Znam", "_ZdlPv", "_ZdaPv")


def donor_command():
    """The compile command native-build.py used, as an argument list."""
    database = BUILD_DIR / "compile_commands.json"
    if not database.is_file():
        sys.exit(f"{database} is missing: run scripts/native-build.py --level 1 --level 2 "
                 "--level 3 --level 4 --with-shims --strict-link first.")
    entries = json.loads(database.read_text())
    for entry in entries:
        if entry["file"].endswith(FLAG_DONOR):
            return entry["command"].split(), pathlib.Path(entry["directory"])
    sys.exit(f"no compile command for {FLAG_DONOR} in {database}")


def compile_harness(out_object):
    """Compile the harness with the donor's flags, only the source and -o replaced."""
    command, directory = donor_command()
    # Everything except the donor's own output, its `-c source`, and the source path itself. The
    # `-include Utility/CppMacros.h` and `-isystem DIR` pairs must survive intact, so this drops
    # exactly the two arguments it knows about rather than filtering on a leading dash.
    rebuilt = []
    skip_next = False
    for argument in command:
        if skip_next:
            skip_next = False
            continue
        if argument in ("-o", "-c"):
            skip_next = True
            continue
        if argument.endswith(FLAG_DONOR):
            continue
        rebuilt.append(argument)
    # The harness performs main()'s prologue (the string critical sections and the memory
    # manager), which lives above WW3D2, so it gets the GameEngine include roots the WW3D2 target
    # does not have. It calls nothing else from there.
    rebuilt += [f"-isystem{REPO_ROOT / directory_}" for directory_ in HARNESS_INCLUDES]
    rebuilt += ["-g", "-o", str(out_object), "-c", str(HARNESS)]
    proc = subprocess.run(rebuilt, cwd=directory, capture_output=True, text=True)
    if proc.returncode != 0:
        sys.stdout.write(proc.stdout + proc.stderr)
        sys.exit("the harness did not compile")
    return proc.stdout + proc.stderr


def rewrite_member(archive, member, redefinitions):
    """Rename symbols in one member of a scratch archive copy."""
    scratch = archive.parent
    subprocess.run(["ar", "x", str(archive), member], cwd=scratch, check=True)
    arguments = []
    for old, new in redefinitions:
        arguments += ["--redefine-sym", f"{old}={new}"]
    subprocess.run([objcopy_tool(), *arguments, str(scratch / member)], check=True)
    subprocess.run(["ar", "r", str(archive), member], cwd=scratch, check=True)
    (scratch / member).unlink()


def llvm_tool(name):
    """GNU binutils on Linux; LLVM's equivalents on macOS, where cctools cannot rename a symbol.

    Named rather than guessed: Apple's `nm` has no --defined-only and its `objcopy` does not exist,
    so a run that fell back to them would fail in the middle of rewriting an archive.
    """
    for candidate in ((name,) if not MACOS else (f"llvm-{name}", name)):
        found = shutil.which(candidate)
        if found:
            return found
    for prefix in ("/opt/homebrew/opt/llvm/bin", "/usr/local/opt/llvm/bin"):
        candidate = pathlib.Path(prefix) / f"llvm-{name}"
        if candidate.is_file():
            return str(candidate)
    sys.exit(f"no {name} found: on macOS the entry-point rename needs `brew install llvm` "
             f"(llvm-{name}); Apple's cctools cannot redefine a symbol.")


def objcopy_tool():
    return llvm_tool("objcopy")


def scratch_archives(scratch, stdlib_new=False):
    """Copy the archives and delete the game's `main()` from the copy.

    The copy exists so the build tree native-build.py measured is not mutated: `ar d` on the real
    archive would change what the next strict link sees.
    """
    archives = sorted(BUILD_DIR.glob("*.a"))
    if not archives:
        sys.exit(f"no archives in {BUILD_DIR}: run scripts/native-build.py first")
    copies = []
    for archive in archives:
        copy = scratch / archive.name
        shutil.copy2(archive, copy)
        copies.append(copy)
        if archive.name == GAME_MEMORY_ARCHIVE and stdlib_new:
            # Mach-O prefixes every symbol with an underscore, Itanium-mangled names included.
            prefix = "_" if MACOS else ""
            rewrite_member(copy, GAME_MEMORY_MEMBER,
                           [(f"{prefix}{s}", f"{prefix}zh_pool_{s}") for s in GLOBAL_NEW_SYMBOLS])
            print(f"renamed the global operator new/delete in {GAME_MEMORY_MEMBER}: the engine and "
                  "the Vulkan driver both use the standard library's for this run")
        if archive.name != GAME_MAIN_ARCHIVE:
            continue
        listing = subprocess.run([llvm_tool("nm"), "-A", "--defined-only", "--extern-only",
                                  str(copy)], capture_output=True, text=True).stdout
        # Mach-O prefixes every C symbol with an underscore, so the game's entry point is `_main`.
        pattern = re.compile(r"^.*?:([^:]+\.o):\s*\S+\s+\S+\s+_?main$")
        for line in listing.splitlines():
            found = pattern.match(line)
            if not found:
                continue
            # Rename the game's entry point rather than deleting the object: the same translation
            # unit defines globals the rest of the engine references (TheWin32Mouse, g_csfFile),
            # so dropping the member trades one duplicate `main` for four unresolved symbols.
            underscore = "_" if MACOS else ""
            rewrite_member(copy, found.group(1),
                           [(f"{underscore}main", f"{underscore}zh_game_main_unused")])
            print(f"renamed main() in {found.group(1)} ({GAME_MAIN_ARCHIVE}) so the harness's is "
                  "the entry point")
    return copies


def link_libraries():
    """The by-path libraries native-build.py puts on the link line, found the same way."""
    spec = importlib.util.spec_from_file_location(
        "native_build", REPO_ROOT / "scripts" / "native-build.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    deps = REPO_ROOT / "build" / "docker" / "_deps"
    found = [module.openal_library(), module.vulkan_library(), module.sdl2_library()]
    return [p for p in found if p is not None] + list(module.ffmpeg_libraries(deps))


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--keep", action="store_true", help="keep the scratch link directory")
    parser.add_argument("--no-present", action="store_true",
                        help="submit frames without asking DX8Wrapper for a flip")
    parser.add_argument("--validation", action="store_true",
                        help="ask the backend for the Vulkan validation layer")
    parser.add_argument("--stop-after-init", action="store_true",
                        help="stop after DX8Wrapper::Init and the device enumeration, before the "
                             "D3DX texture wall, so the unimplemented-call ledger is printed")
    parser.add_argument("--stdlib-new", action="store_true",
                        help="link the standard library's operator new instead of GameMemory's "
                             "4-byte-aligned pool allocator, which lavapipe's LLVM JIT cannot "
                             "survive on x86-64; MoltenVK does survive it, so this is a control")
    parser.add_argument("--lldb", action="store_true",
                        help="run the harness under lldb and print the backtrace. Needed on macOS "
                             "rather than invoking lldb by hand: it is SIP-protected, so dyld "
                             "strips DYLD_LIBRARY_PATH from the environment it passes on and the "
                             "validation layer cannot be loaded; this sets target.env-vars instead")
    args = parser.parse_args()

    scratch = BUILD_DIR / "render-run"
    if scratch.exists():
        shutil.rmtree(scratch)
    scratch.mkdir(parents=True)

    print("== compiling the harness with the engine's own flags")
    compile_harness(scratch / "native_render_run.o")

    print("== copying the archives")
    archives = scratch_archives(scratch, stdlib_new=args.stdlib_new)
    support = [a for a in archives if a.name.startswith("libsupport_")
               or a.name == "libthirdparty_lzhl.a"]
    engine = [a for a in archives if a not in support]

    print("== linking")
    binary = scratch / "native_render_run"
    # The engine archives are loaded whole, as the strict link loads them: the harness references a
    # handful of DX8Wrapper entry points and everything else is reached through the engine's own
    # static initialisers and vtables. ld64 spells that per-archive (-force_load) rather than as a
    # mode with an off switch, and has no -ldl (dlopen is in libSystem).
    if MACOS:
        whole = [flag for a in engine for flag in ("-Wl,-force_load", str(a))]
        platform_libs = ["-lc++", "-lm", "-lpthread", "-lz",
                         *[flag for f in MACOS_FRAMEWORKS for flag in ("-framework", f)]]
    else:
        whole = ["-Wl,--whole-archive", *[str(a) for a in engine], "-Wl,--no-whole-archive"]
        platform_libs = ["-lstdc++", "-lm", "-lpthread", "-ldl", "-lz"]
    command = [os.environ.get("CLANGXX", "clang++"), "-std=c++20", "-o", str(binary),
               str(scratch / "native_render_run.o"), *whole,
               *[str(a) for a in support], *platform_libs,
               *[str(p) for p in link_libraries()]]
    proc = subprocess.run(command, capture_output=True, text=True)
    if proc.returncode != 0 or not binary.exists():
        sys.stdout.write(proc.stdout + proc.stderr)
        sys.exit("the harness did not link")
    print(f"   {binary.relative_to(REPO_ROOT)}")

    print("== running")
    environment = dict(os.environ)
    # FFmpeg is linked by path from the deps tree, which has no entry in the loader cache.
    ffmpeg_dir = REPO_ROOT / "build" / "docker" / "_deps" / "ffmpeg-lib" / "lib"
    loader_path = "DYLD_LIBRARY_PATH" if MACOS else "LD_LIBRARY_PATH"
    if ffmpeg_dir.is_dir():
        existing = environment.get(loader_path, "")
        environment[loader_path] = f"{ffmpeg_dir}:{existing}" if existing else str(ffmpeg_dir)
    if MACOS:
        # The loader opens the layer dylib by leaf name; see MACOS_LOADER_DIRS.
        for directory in MACOS_LOADER_DIRS:
            if pathlib.Path(directory).is_dir():
                existing = environment.get(loader_path, "")
                environment[loader_path] = f"{directory}:{existing}" if existing else directory
    if "VK_ICD_FILENAMES" not in environment:
        # Naming the ICD rather than trusting the loader's search is what the renderer-spike skill
        # does, and on macOS it is what keeps a stray SDK loader from picking a different driver.
        icds = MOLTENVK_ICDS if MACOS else (LAVAPIPE_ICD,)
        for icd in icds:
            if pathlib.Path(icd).is_file():
                environment["VK_ICD_FILENAMES"] = icd
                break
    if args.validation:
        environment["ZH_VULKAN_VALIDATION"] = "1"
    run = [str(binary)]
    if args.no_present:
        run.append("--no-present")
    if args.stop_after_init:
        run.append("--stop-after-init")
    if args.lldb:
        # Every DYLD_* variable is stripped when a SIP-protected binary is executed, so lldb cannot
        # inherit the loader path the layer needs; target.env-vars is set inside the session, which
        # is where the launched process reads it from.
        exported = [f"{name}={environment[name]}" for name in
                    (loader_path, "VK_ICD_FILENAMES", "ZH_VULKAN_VALIDATION")
                    if name in environment]
        # A command file, not a series of -o: with -o, lldb stops executing the remaining commands
        # once the process stops on a signal, so `bt` never runs and the crash is reported without
        # a backtrace. Measured on the outpost, where the first run of this option printed none.
        commands = scratch / "lldb-commands.txt"
        commands.write_text("settings set target.env-vars " + " ".join(exported) + "\n"
                            "run\nbt\nframe variable\nquit\n")
        run = ["lldb", "-b", "-s", str(commands), "--", *run]
    result = subprocess.run(run, env=environment, cwd=REPO_ROOT)
    if args.lldb:
        # lldb's own exit status, not the harness's: it reports the debugger session, and a process
        # that died on a signal still leaves lldb exiting 0.
        print("(--lldb: the status below is lldb's, not the harness's)")
    if not args.keep:
        for archive in archives:
            archive.unlink()
    print(f"\nharness exit code: {result.returncode}")
    return result.returncode


if __name__ == "__main__":
    sys.exit(main())
