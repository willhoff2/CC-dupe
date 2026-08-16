#!/usr/bin/env python3
"""Pin the two pieces of harness logic that decide what a number in docs/porting/ means.

Both are pure Python, both were wrong in a way no compile or link could reveal, and both are read
by humans as facts about the port rather than about the harness:

1. `native-build.py`'s symbol categorisation, which decides which pile an unresolved symbol lands
   in. It used to match a vendor NAME PATTERN before considering the EVIDENCE that a file in this
   tree defines the symbol, so `D3DXFilterTexture` -- defined in WW3D2, a level-4 library -- was
   filed at levels 1-3 as "Direct3D 8 / DirectX", whose pile is `no-definition-anywhere`, i.e.
   remaining port work. The definition was in the tree the whole time. Every symbol sharing a
   vendor prefix while living in an unbuilt layer was overstated the same way, so the fix is an
   ordering rule and these cases are what hold it in place.

2. `check-native-build-baseline.py`'s binary gate, the inverted ratchet: once a baseline records a
   produced executable, the assertion stops being "the unresolved count must not grow" and becomes
   "the link must not break". Its failure cases are the ones nobody exercises by accident -- a link
   that stops completing, a file that is not 64-bit, a `binary_produced: true` with no file
   description behind it -- so they are asserted here rather than waited for.

3. The symbol namespace and the entry-point rule, both of which #87's Darwin run broke without any
   compile or link noticing. Mach-O prefixes C symbols with an underscore, the scan looked for the
   ELF spelling of `main`, found no entry point, and a generated stub `main()` silently replaced the
   game's -- so the cases here are that exactly one prefix is removed (never `lstrip("_")`, which
   corrupts `__ZN...` and `___cxa_throw`), and that a missing entry point stops the run instead.
   Written on Linux: the Mach-O cases drive the prefix directly rather than needing a Mac.

Run: python3 scripts/native-build-categorise-test.py
"""
import importlib.util
import pathlib
import shutil
import sys
import tempfile

REPO_ROOT = pathlib.Path(__file__).resolve().parents[1]

CHECKS = 0
FAILURES = []


def load(path, name):
    """Import a hyphenated script by path, the way no `import` statement can."""
    spec = importlib.util.spec_from_file_location(name, REPO_ROOT / path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


def check(label, got, want):
    global CHECKS
    CHECKS += 1
    if got != want:
        FAILURES.append(f"{label}: got {got!r}, want {want!r}")
        print(f"FAIL {label}\n     got  {got!r}\n     want {want!r}")
    else:
        print(f"ok   {label}")


def check_contains(label, haystack, needle):
    global CHECKS
    CHECKS += 1
    if not any(needle in item for item in haystack):
        FAILURES.append(f"{label}: no failure mentioning {needle!r} in {haystack!r}")
        print(f"FAIL {label}\n     nothing mentioning {needle!r} in {haystack!r}")
    else:
        print(f"ok   {label}")


def check_raises(label, call, needle):
    global CHECKS
    CHECKS += 1
    try:
        call()
    except SystemExit as exit_:
        if needle in str(exit_):
            print(f"ok   {label}")
            return
        FAILURES.append(f"{label}: raised without {needle!r}: {exit_}")
        print(f"FAIL {label}\n     raised without {needle!r}: {exit_}")
        return
    FAILURES.append(f"{label}: did not raise")
    print(f"FAIL {label}\n     did not raise")


class symbol_prefix_of:
    """Run a case in another platform's symbol namespace, without needing that platform."""

    def __init__(self, build, prefix):
        self.build, self.prefix = build, prefix

    def __enter__(self):
        self.saved = self.build.symbol_prefix
        self.build.symbol_prefix = lambda: self.prefix
        return self

    def __exit__(self, *exc):
        self.build.symbol_prefix = self.saved
        return False


def attribution_with(build, **kwargs):
    """An Attribution whose fields are all empty except the ones a case is about."""
    empty = frozenset()
    fields = dict(win32_names=empty, gamespy_names=empty, uncompiled_names=empty,
                  unbuilt_layer_names=empty, built_definition_names=empty,
                  excluded_backend_names=empty, uncompiled_classes=empty,
                  unbuilt_layer_classes=empty,
                  well_known_keys_category="Other / unclassified")
    fields.update({key: frozenset(value) if isinstance(value, (set, frozenset, list, tuple))
                   else value for key, value in kwargs.items()})
    return build.Attribution(**fields)


def test_categorisation(build):
    print("== evidence outranks the vendor name pattern")

    # The reported bug, both halves of it: WW3D2 defines these and WW3D2 is level 4.
    for symbol in ("D3DXFilterTexture", "D3DXAssembleShader"):
        attribution = attribution_with(build, unbuilt_layer_names={symbol})
        check(f"{symbol} defined in an unbuilt layer",
              build.categorise_symbol(symbol, symbol, attribution),
              build.UNBUILT_LAYER_CATEGORY)

    # With no evidence anywhere the name pattern is exactly right, and must still apply: this is a
    # precedence fix, not the removal of the patterns.
    check("D3DXFilterTexture with no definition in the tree",
          build.categorise_symbol("D3DXFilterTexture", "D3DXFilterTexture",
                                  attribution_with(build)),
          "Direct3D 8 / DirectX")

    # The same ordering for the other three kinds of evidence, on names the patterns also match, so
    # a regression in the ordering fails here rather than in a count nobody re-derives.
    check("a D3DX name a failed translation unit defines",
          build.categorise_symbol("D3DXCreateTexture", "D3DXCreateTexture",
                                  attribution_with(build, uncompiled_names={"D3DXCreateTexture"})),
          "Defined in a translation unit that failed to compile")
    check("a D3DX name a built translation unit defines behind a disabled #if",
          build.categorise_symbol("D3DXCreateTexture", "D3DXCreateTexture",
                                  attribution_with(build,
                                                   built_definition_names={"D3DXCreateTexture"})),
          build.DISABLED_IF_CATEGORY)
    # The window backends define `Window_Create` inside `namespace WWPlatform`; the source scan sees
    # it unqualified, so the excluded-backend lookup is the one case that matches unqualified.
    check("a namespaced backend entry point the source scan saw unqualified",
          build.categorise_symbol("_ZN10WWPlatform13Window_CreateEv",
                                  "WWPlatform::Window_Create()",
                                  attribution_with(build,
                                                   excluded_backend_names={"Window_Create"})),
          build.EXCLUDED_BACKEND_CATEGORY)

    # Evidence about a *class* is weaker than evidence about the name -- it is how the
    # symbols are attributed -- so it stays below the patterns, or every `IDirect3D*` vtable entry
    # would be filed by whichever layer happens to mention the class.
    check("a Win32 name with no evidence at all",
          build.categorise_symbol("GetWindowRect", "GetWindowRect",
                                  attribution_with(build, win32_names={"GetWindowRect"})),
          "Win32 API")


def test_binary_gate(gate):
    print()
    print("== the inverted ratchet: the link must not break")

    linux_binary = {"path": "build/native/native_strict_link", "bytes": 88_000_000,
                    "format": "ELF", "word_size": 64, "machine": "x86-64", "elf_type": "DYN"}
    good_baseline = {"attempted": True, "clean": True, "binary_produced": True,
                     "unresolved_total": 0, "binary": linux_binary}
    good_result = dict(good_baseline)

    check("a clean link against a baseline that had one",
          gate.check_binary_gate(good_baseline, good_result), [])

    # Before the first binary the gate is silent: the count ratchet is what applies then, and this
    # function must not start failing runs measured against a pre-binary baseline.
    check("no executable in the baseline: the gate does not apply",
          gate.check_binary_gate({"attempted": True, "clean": False, "binary_produced": False,
                                  "unresolved_total": 73},
                                 {"attempted": True, "clean": False, "binary_produced": False,
                                  "unresolved_total": 77}),
          [])

    broke = gate.check_binary_gate(good_baseline,
                                   {"attempted": True, "clean": False, "binary_produced": False,
                                    "unresolved_total": 4})
    check_contains("a link that stopped producing an executable fails", broke,
                   "produced an executable in the baseline and produces none now")
    check_contains("...and says how many symbols it is short by", broke, "4 unresolved symbol(s)")

    # Four unresolved symbols is not "the count grew by four": it is no executable. The message says
    # so, because the whole point of the inversion is that those are different events.
    check_contains("...and names it as a broken build rather than a bigger number", broke,
                   "broken build, not a bigger number")

    thirty_two_bit = dict(good_result,
                          binary=dict(linux_binary, word_size=32, machine="i386"))
    check_contains("a 32-bit file at the executable's path fails",
                   gate.check_binary_gate(good_baseline, thirty_two_bit), "32-bit")

    described_nothing = {"attempted": True, "clean": True, "binary_produced": True,
                         "unresolved_total": 0}
    check_contains("binary_produced with no description of the file fails",
                   gate.check_binary_gate(good_baseline, described_nothing),
                   "describes no file")

    macho = dict(good_result, binary=dict(linux_binary, format="Mach-O", machine="arm64"))
    check_contains("a baseline from another OS is reported as that, not as a comparison",
                   gate.check_binary_gate(good_baseline, macho), "another OS")

    # A link that "succeeded" while reporting unresolved symbols would mean the harness and the
    # linker disagree; the gate does not let that pass as an executable.
    check_contains("a clean flag with unresolved symbols still fails",
                   gate.check_binary_gate(good_baseline,
                                          dict(good_result, unresolved_total=2)),
                   "2 unresolved")

    # The arm64 questions. A universal file, or two tools disagreeing about the architecture, would
    # satisfy every check above while invalidating the conclusions drawn from the run -- an x86-64
    # build on an Apple Silicon Mac is the case that matters, and it is silent by nature.
    mac_baseline = {"attempted": True, "clean": True, "binary_produced": True,
                    "unresolved_total": 0,
                    "binary": {"path": "build/native/native_strict_link", "bytes": 27_900_000,
                               "format": "Mach-O", "word_size": 64, "machine": "arm64",
                               "lipo_archs": ["arm64"]}}
    check("a thin arm64 Mach-O against an arm64 baseline",
          gate.check_binary_gate(mac_baseline, dict(mac_baseline)), [])
    fat = dict(mac_baseline, binary={"path": "x", "bytes": 1, "format": "Mach-O universal",
                                     "architectures": 2, "word_size": None, "machine": None,
                                     "lipo_archs": ["arm64", "x86-64"]})
    check_contains("a universal binary is not the thin native one being measured",
                   gate.check_binary_gate(mac_baseline, fat), "universal Mach-O")
    disagree = dict(mac_baseline,
                    binary=dict(mac_baseline["binary"], lipo_archs=["x86-64"]))
    check_contains("lipo and the header disagreeing about the architecture fails",
                   gate.check_binary_gate(mac_baseline, disagree), "lipo -archs")


def test_describe_binary(build):
    print()
    print("== the executable is described from its own header")
    # This interpreter is an ELF or Mach-O 64-bit executable on every platform this runs on, so it
    # is a real file to decode without building one.
    info = build.describe_binary(sys.executable)
    check("the running interpreter is 64-bit", info["word_size"], 64)
    check("...and its container is named", info["format"] in ("ELF", "Mach-O"), True)
    check("...and its size is read from the filesystem", info["bytes"] > 0, True)
    check("a path with no file there describes nothing",
          build.describe_binary(REPO_ROOT / "build" / "native" / "no-such-file"), None)


def test_symbol_namespace(build):
    print()
    print("== one symbol namespace, one prefix removed")

    with symbol_prefix_of(build, "_"):
        check("Mach-O `_main` is the entry point", build.canonical_symbol("_main"), "main")
        # The two names greedy stripping destroys. `__ZN...` is Mach-O for the mangled `_ZN...`, and
        # a corrupted name cannot match the same symbol read from anywhere else.
        check("a mangled C++ name keeps its own leading underscore",
              build.canonical_symbol("__ZN9DX8Wrapper4InitEv"), "_ZN9DX8Wrapper4InitEv")
        check("an ABI name keeps its own two",
              build.canonical_symbol("___cxa_throw"), "__cxa_throw")
        check("a name without the prefix is left alone",
              build.canonical_symbol("dyld_stub_binder"), "dyld_stub_binder")
        check("the entry point is spelled for the platform",
              build.platform_symbol(build.ENTRY_SYMBOL), "_main")
        # The names the toolchain supplies rather than any library, in this namespace.
        for supplied in ("__cxa_throw", "_mh_execute_header", "dyld_stub_binder"):
            check(f"{supplied} is a toolchain symbol, not a blocker",
                  bool(build.TOOLCHAIN_SYMBOL_RE.match(supplied)), True)

    with symbol_prefix_of(build, ""):
        for name in ("main", "_ZN9DX8Wrapper4InitEv", "__cxa_throw"):
            check(f"ELF leaves {name} untouched", build.canonical_symbol(name), name)

    # And the prefix itself is measured from the toolchain rather than taken from sys.platform, so
    # the round trip has to hold on whatever this box is. Only where there is a compiler to measure
    # it with: this file is in the cheap CI tier, which is not required to have one.
    if shutil.which(build.CXX):
        check("the measured prefix round-trips",
              build.canonical_symbol(build.platform_symbol(build.ENTRY_SYMBOL)),
              build.ENTRY_SYMBOL)

    # macOS keeps its system libraries in the dyld shared cache, so the discount list is read from
    # the SDK's text-based stubs. An empty list would report every libc symbol as port work.
    with symbol_prefix_of(build, "_"), tempfile.TemporaryDirectory() as tmp:
        stub = pathlib.Path(tmp) / "libSystem.tbd"
        stub.write_text(
            "--- !tapi-tbd\ntbd-version: 4\nexports:\n"
            "  - targets: [ arm64-macos ]\n"
            "    symbols: [ _malloc, _free,\n"
            "               _dyld_stub_binder ]\n")
        check("a .tbd stub's exports are read in this namespace",
              build.tbd_symbols(stub), {"malloc", "free", "dyld_stub_binder"})


def test_entry_point_anchor(build):
    print()
    print("== a missing entry point stops the run; it is never replaced by a stub")

    # In Mach-O's namespace throughout, because that is the platform the substitution happened on,
    # and so that these cases need no compiler to measure a prefix with.
    with symbol_prefix_of(build, "_"):
        entry_point_anchor_cases(build)


def entry_point_anchor_cases(build):
    text, stub_used = build.entry_point_anchor(["libgeneralsmd_code_main"], True)
    check("the game's own main() anchors the link", stub_used, False)
    check("...and nothing is generated to compete with it", "int main" in text, False)

    text, stub_used = build.entry_point_anchor([], False)
    check("a levels 1-2 build has no entry point of its own to link", stub_used, True)
    check("...so a stub anchors it, and says why", "int main" in text, True)

    # The case that made #87's Darwin run measure a stub: the entry-point target was built, its
    # `main()` was in the archive under a name the scan did not know, and the harness quietly linked
    # three lines of generated code instead of the game.
    check_raises("the entry-point target built with no entry point found stops the run",
                 lambda: build.entry_point_anchor([], True),
                 "Refusing to substitute a generated stub main()")


def test_entry_point_gate(gate):
    print()
    print("== and a baseline can never record having had one")

    anchored = {"game_entry_target_built": True,
                "link_entry_point_archives": ["libgeneralsmd_code_main"],
                "link_entry_point_stub": False}
    check("the game's entry point, before and after",
          gate.check_entry_point(anchored, anchored), [])

    check_contains("a stub while the entry-point target is built fails",
                   gate.check_entry_point(anchored, {"game_entry_target_built": True,
                                                     "link_entry_point_archives": [],
                                                     "link_entry_point_stub": True}),
                   "stub main()")
    check_contains("losing the game's entry point fails even without a stub",
                   gate.check_entry_point(anchored, {"game_entry_target_built": False,
                                                     "link_entry_point_archives": []}),
                   "baseline's link was anchored by the game's own entry point")
    # A levels 1-2 baseline has no entry point either side, and must not start failing.
    check("no entry point in either, below level 3",
          gate.check_entry_point({"link_entry_point_archives": []},
                                 {"game_entry_target_built": False,
                                  "link_entry_point_archives": [], "link_entry_point_stub": True}),
          [])
    check_contains("a run measured under Rosetta 2 fails",
                   gate.check_entry_point(anchored, dict(anchored, host_translated=True)),
                   "Rosetta 2")


def main():
    build = load("scripts/native-build.py", "native_build_under_test")
    gate = load("scripts/ci/check-native-build-baseline.py", "native_build_gate_under_test")
    test_categorisation(build)
    test_binary_gate(gate)
    test_describe_binary(build)
    test_symbol_namespace(build)
    test_entry_point_anchor(build)
    test_entry_point_gate(gate)
    print()
    print(f"{CHECKS} checks, {len(FAILURES)} failures")
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
