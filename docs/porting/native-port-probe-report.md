# Native 64-bit clang probe — platform-independent libraries

Compiled 147 translation units with `clang++ -fsyntax-only -std=c++20 -m64 -ferror-limit=0 -fms-extensions -include Utility/CppMacros.h -DWIN32_LEAN_AND_MEAN -D_REENTRANT` (no Windows SDK, no Wine).

- Translation units that already compile clean: **106 / 147** (72%)
- Translation units with errors: **41**
- Total errors: **88**

## Errors by category

| Category | Errors | Files | Example |
|---|---:|---:|---|
| Non-conforming template/name lookup | 34 | 4 | `Core/Libraries/Source/WWVegas/WWLib/mpu.cpp: use of undeclared identifier 'GetCurrentProcess'` |
| Missing Win32 headers | 25 | 25 | `Core/Libraries/Source/WWVegas/WWMath/matrix3d.cpp: 'd3d8types.h' file not found` |
| Other | 15 | 10 | `Core/Libraries/Source/WWVegas/WWLib/mutex.cpp: expected expression` |
| Win32 types undeclared | 10 | 3 | `Core/Libraries/Source/WWVegas/WWLib/mpu.cpp: unknown type name 'LARGE_INTEGER'` |
| Missing project/vendor headers | 4 | 4 | `Core/Libraries/Source/Compression/LZHCompress/NoxCompress.cpp: 'CompLibHeader/lzhl.h' file not found` |

## Per-library breakdown

| Library | Clean | Total |
|---|---:|---:|
| Core/Libraries/Source/Compression | 10 | 11 |
| Core/Libraries/Source/WWVegas/WWDebug | 2 | 3 |
| Core/Libraries/Source/WWVegas/WWLib | 49 | 60 |
| Core/Libraries/Source/WWVegas/WWMath | 33 | 35 |
| Core/Libraries/Source/WWVegas/WWSaveLoad | 11 | 12 |
| Core/Libraries/Source/debug | 1 | 20 |
| Core/Libraries/Source/profile | 0 | 6 |

## Translation units already clean

- `Core/Libraries/Source/Compression/CompressionManager.cpp`
- `Core/Libraries/Source/Compression/EAC/btreeabout.cpp`
- `Core/Libraries/Source/Compression/EAC/btreedecode.cpp`
- `Core/Libraries/Source/Compression/EAC/btreeencode.cpp`
- `Core/Libraries/Source/Compression/EAC/huffabout.cpp`
- `Core/Libraries/Source/Compression/EAC/huffdecode.cpp`
- `Core/Libraries/Source/Compression/EAC/huffencode.cpp`
- `Core/Libraries/Source/Compression/EAC/refabout.cpp`
- `Core/Libraries/Source/Compression/EAC/refdecode.cpp`
- `Core/Libraries/Source/Compression/EAC/refencode.cpp`
- `Core/Libraries/Source/WWVegas/WWMath/aabox.cpp`
- `Core/Libraries/Source/WWVegas/WWMath/aabtreecull.cpp`
- `Core/Libraries/Source/WWVegas/WWMath/cardinalspline.cpp`
- `Core/Libraries/Source/WWVegas/WWMath/catmullromspline.cpp`
- `Core/Libraries/Source/WWVegas/WWMath/colmath.cpp`
- `Core/Libraries/Source/WWVegas/WWMath/colmathaabox.cpp`
- `Core/Libraries/Source/WWVegas/WWMath/colmathaabtri.cpp`
- `Core/Libraries/Source/WWVegas/WWMath/colmathfrustum.cpp`
- `Core/Libraries/Source/WWVegas/WWMath/colmathline.cpp`
- `Core/Libraries/Source/WWVegas/WWMath/colmathobbobb.cpp`
- `Core/Libraries/Source/WWVegas/WWMath/colmathobbox.cpp`
- `Core/Libraries/Source/WWVegas/WWMath/colmathobbtri.cpp`
- `Core/Libraries/Source/WWVegas/WWMath/colmathplane.cpp`
- `Core/Libraries/Source/WWVegas/WWMath/colmathsphere.cpp`
- `Core/Libraries/Source/WWVegas/WWMath/cullsys.cpp`
- `Core/Libraries/Source/WWVegas/WWMath/curve.cpp`
- `Core/Libraries/Source/WWVegas/WWMath/euler.cpp`
- `Core/Libraries/Source/WWVegas/WWMath/frustum.cpp`
- `Core/Libraries/Source/WWVegas/WWMath/gridcull.cpp`
- `Core/Libraries/Source/WWVegas/WWMath/hermitespline.cpp`
- `Core/Libraries/Source/WWVegas/WWMath/lineseg.cpp`
- `Core/Libraries/Source/WWVegas/WWMath/lookuptable.cpp`
- `Core/Libraries/Source/WWVegas/WWMath/matrix3.cpp`
- `Core/Libraries/Source/WWVegas/WWMath/obbox.cpp`
- `Core/Libraries/Source/WWVegas/WWMath/ode.cpp`
- `Core/Libraries/Source/WWVegas/WWMath/pot.cpp`
- `Core/Libraries/Source/WWVegas/WWMath/quat.cpp`
- `Core/Libraries/Source/WWVegas/WWMath/tcbspline.cpp`
- `Core/Libraries/Source/WWVegas/WWMath/tri.cpp`
- `Core/Libraries/Source/WWVegas/WWMath/v3_rnd.cpp`
- `Core/Libraries/Source/WWVegas/WWMath/vehiclecurve.cpp`
- `Core/Libraries/Source/WWVegas/WWMath/vp.cpp`
- `Core/Libraries/Source/WWVegas/WWMath/wwmath.cpp`
- `Core/Libraries/Source/WWVegas/WWLib/Except.cpp`
- `Core/Libraries/Source/WWVegas/WWLib/FastAllocator.cpp`
- `Core/Libraries/Source/WWVegas/WWLib/TARGA.cpp`
- `Core/Libraries/Source/WWVegas/WWLib/argv.cpp`
- `Core/Libraries/Source/WWVegas/WWLib/b64pipe.cpp`
- `Core/Libraries/Source/WWVegas/WWLib/b64straw.cpp`
- `Core/Libraries/Source/WWVegas/WWLib/base64.cpp`
- `Core/Libraries/Source/WWVegas/WWLib/bfiofile.cpp`
- `Core/Libraries/Source/WWVegas/WWLib/buff.cpp`
- `Core/Libraries/Source/WWVegas/WWLib/bufffile.cpp`
- `Core/Libraries/Source/WWVegas/WWLib/chunkio.cpp`
- `Core/Libraries/Source/WWVegas/WWLib/cpudetect.cpp`
- `Core/Libraries/Source/WWVegas/WWLib/crc.cpp`
- `Core/Libraries/Source/WWVegas/WWLib/crcpipe.cpp`
- `Core/Libraries/Source/WWVegas/WWLib/crcstraw.cpp`
- `Core/Libraries/Source/WWVegas/WWLib/cstraw.cpp`
- `Core/Libraries/Source/WWVegas/WWLib/ffactory.cpp`
- `Core/Libraries/Source/WWVegas/WWLib/gcd_lcm.cpp`
- `Core/Libraries/Source/WWVegas/WWLib/hash.cpp`
- `Core/Libraries/Source/WWVegas/WWLib/ini.cpp`
- `Core/Libraries/Source/WWVegas/WWLib/jshell.cpp`
- `Core/Libraries/Source/WWVegas/WWLib/lzo.cpp`
- `Core/Libraries/Source/WWVegas/WWLib/lzo1x_c.cpp`
- `Core/Libraries/Source/WWVegas/WWLib/lzo1x_d.cpp`
- `Core/Libraries/Source/WWVegas/WWLib/lzopipe.cpp`
- `Core/Libraries/Source/WWVegas/WWLib/lzostraw.cpp`
- `Core/Libraries/Source/WWVegas/WWLib/md5.cpp`
- `Core/Libraries/Source/WWVegas/WWLib/multilist.cpp`
- `Core/Libraries/Source/WWVegas/WWLib/nstrdup.cpp`
- `Core/Libraries/Source/WWVegas/WWLib/pipe.cpp`
- `Core/Libraries/Source/WWVegas/WWLib/ramfile.cpp`
- `Core/Libraries/Source/WWVegas/WWLib/random.cpp`
- `Core/Libraries/Source/WWVegas/WWLib/rawfile.cpp`
- `Core/Libraries/Source/WWVegas/WWLib/readline.cpp`
- `Core/Libraries/Source/WWVegas/WWLib/realcrc.cpp`
- `Core/Libraries/Source/WWVegas/WWLib/refcount.cpp`
- `Core/Libraries/Source/WWVegas/WWLib/slnode.cpp`
- `Core/Libraries/Source/WWVegas/WWLib/stimer.cpp`
- `Core/Libraries/Source/WWVegas/WWLib/straw.cpp`
- `Core/Libraries/Source/WWVegas/WWLib/systimer.cpp`
- `Core/Libraries/Source/WWVegas/WWLib/textfile.cpp`
- `Core/Libraries/Source/WWVegas/WWLib/tgatodxt.cpp`
- `Core/Libraries/Source/WWVegas/WWLib/trim.cpp`
- `Core/Libraries/Source/WWVegas/WWLib/vector.cpp`
- `Core/Libraries/Source/WWVegas/WWLib/widestring.cpp`
- `Core/Libraries/Source/WWVegas/WWLib/wwfile.cpp`
- `Core/Libraries/Source/WWVegas/WWLib/wwstring.cpp`
- `Core/Libraries/Source/WWVegas/WWLib/xpipe.cpp`
- `Core/Libraries/Source/WWVegas/WWLib/xstraw.cpp`
- `Core/Libraries/Source/WWVegas/WWDebug/wwmemlog.cpp`
- `Core/Libraries/Source/WWVegas/WWDebug/wwprofile.cpp`
- `Core/Libraries/Source/WWVegas/WWSaveLoad/definition.cpp`
- `Core/Libraries/Source/WWVegas/WWSaveLoad/definitionfactory.cpp`
- `Core/Libraries/Source/WWVegas/WWSaveLoad/definitionfactorymgr.cpp`
- `Core/Libraries/Source/WWVegas/WWSaveLoad/definitionmgr.cpp`
- `Core/Libraries/Source/WWVegas/WWSaveLoad/parameter.cpp`
- `Core/Libraries/Source/WWVegas/WWSaveLoad/persistfactory.cpp`
- `Core/Libraries/Source/WWVegas/WWSaveLoad/pointerremap.cpp`
- `Core/Libraries/Source/WWVegas/WWSaveLoad/saveloadstatus.cpp`
- `Core/Libraries/Source/WWVegas/WWSaveLoad/saveloadsubsystem.cpp`
- `Core/Libraries/Source/WWVegas/WWSaveLoad/twiddler.cpp`
- `Core/Libraries/Source/WWVegas/WWSaveLoad/wwsaveload.cpp`
- `Core/Libraries/Source/debug/debug_getdefaultcommands.cpp`
