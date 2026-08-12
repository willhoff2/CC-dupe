"""Import shim for `native-port-probe.py`, whose file name is not a Python identifier.

The probe owns the definition of what a native build is: which translation units belong to each
library, which include paths they need, and which compiler flags reproduce the MSVC build's
assumptions. `native-build.py` must use exactly the same definitions, or its numbers cannot be
compared against the probe's -- which is the whole point of running it.
"""

import importlib.util
import pathlib

_PROBE_PATH = pathlib.Path(__file__).resolve().parent / "native-port-probe.py"

_spec = importlib.util.spec_from_file_location("native_port_probe", _PROBE_PATH)
probe = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(probe)

REPO_ROOT = probe.REPO_ROOT
COMMON_INCLUDES = probe.COMMON_INCLUDES
TARGETS = probe.TARGETS
RENDERER_TARGETS = probe.RENDERER_TARGETS
DEFAULT_DEPS_DIR = probe.DEFAULT_DEPS_DIR


def target_sources(target):
    """The translation units the real build compiles for a probe target."""
    if target.cmake_lists:
        return probe.cmake_sources(target)
    sources = []
    for directory in target.source_dirs:
        sources.extend(sorted((REPO_ROOT / directory).rglob("*.cpp")))
    return sources


def target_includes(target, deps_dir):
    """Include directories for a probe target, in the probe's order."""
    includes = list(probe.dep_includes(deps_dir))
    includes.extend(str(REPO_ROOT / d) for d in COMMON_INCLUDES)
    includes.extend(str(REPO_ROOT / d) for d in target.includes)
    return includes
