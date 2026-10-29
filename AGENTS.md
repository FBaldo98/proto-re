# AGENTS.md

Guidance for AI coding agents working on `proto-re`, the native Android port of Prototype (2009).

## What this is

The port statically recompiles the game's x86 `prototypeenginef.dll` into C, builds it for arm64, and runs it on a
from-scratch Win32/D3D9 layer: SDL2 (window/input/audio), DXVK (D3D9 on Vulkan), FAudio (XAudio2). There is no
emulator and no Wine on the phone, and the APK contains no game data.

## Ground rules

- **Never commit game data or engine code.** The repository must not contain `.rcf`, `.p3d`, `.bik`, `.gfx`, `.dds`
  files, the original DLL, save data, decompiled dumps, or generated recompiled engine code. They are in
  `.gitignore` (`build/`, `re/decomp/`, `ghidra/`, `extracted/`, `__pycache__/`); keep it that way.
- **No DRM circumvention.** The port requires a purchased copy of the game; do not add code that extracts keys or
  bypasses activation.
- **Do not build the engine in CI.** GitHub Actions builds only third-party dependencies. The engine and generated
  code are built locally from the user's own DLL.
- Keep `README.md`, `README.ru.md` and `android/protore.ini.example` in sync when adding or changing a setting.
- Do not hand-edit generated files: `src/native/d3d9_gen.inc` comes from `tools/gen_d3d9_bridge.py`, and everything
  under `build/recomp/` comes from `tools/recomp/recomp.py`.
- Do not reformat untouched code or add unrelated changes; keep diffs focused.

## Layout

| path | contents |
|---|---|
| `src/recomp/` | guest runtime (`runtime.h`, `runtime_fp.h`), x86 bridge; `-DRECOMP_FLAT_MEMORY`, TSO |
| `src/native/` | the phone host: Win32/D3D9/XAudio2/Bink import implementations, DXVK bridge, loader, Android glue |
| `src/core/`, `src/math/` | game-independent helpers shared by the native port and the Windows hook host |
| `src/host/` | 32-bit Windows `binkw32.dll` proxy that hooks and differential-tests the original engine |
| `src/tests/` | differential tests linked into the hook host; they run inside the game |
| `android/` | `build-deps.sh`, `build-turnip.sh`, `build-apk.sh`, Java activity, patches, `protore.ini.example` |
| `tools/` | recompiler, D3D9 bridge generator, RCF/P3D tools, thermal-guarded `cool_make.sh` |
| `re/` | research notes: `subsystems.md`, `issues.md`, `functions.csv` (read before engine-facing work) |
| `build/` | all generated/build output (ignored) |

## Toolchain

Linux (or WSL) with: Android NDK (the Makefile defaults to `~/Android/Sdk/ndk/28.2.13676358`), JDK, Android SDK
build-tools 36 and platform 35, `python3` with `capstone` and `pefile`, `cmake`, `ninja`, and `meson >= 1.4`; Turnip
additionally needs `mako`, `pyyaml`, `bison`, `flex`, `glslangValidator`. The hook host needs `i686-w64-mingw32-g++`.

`PROTORE_ENGINE` points at the original `prototypeenginef.dll` (default
`~/Downloads/Prototype (2009)/Prototype/prototypeenginef.dll`).

## Build

Full Android build (see the README for the current commands):

```sh
android/build-deps.sh arm64-v8a ~/proto-re-data/android-deps-src ~/proto-re-data/android-deps/arm64-v8a
android/build-turnip.sh ~/proto-re-data/android-deps-src ~/proto-re-data/android-turnip
python3 -I tools/recomp/data_entries.py
python3 -I tools/recomp/recomp.py --functions all --base 0x7c000000 --split 400 --out build/recomp/native
tools/cool_make.sh -C src/native PLATFORM=android ABI=arm64-v8a
android/build-apk.sh arm64-v8a
```

- The recompile step must run before `src/native` builds: it produces `build/recomp/native` (`RECOMP_DIR`).
- Dependencies are installed under the deps prefix (`DEPS` in `src/native/Makefile`, `DEPS_ROOT` in
  `build-apk.sh`). Changing `android/build-deps.sh` invalidates the CI cache key, so deps rebuild.
- ARMv8.0 phones use a second native build: add `ARM_ARCH=armv8-a OUT=../../build/android/arm64-v8a-v80`;
  `ProtoActivity` picks the right one at runtime.
- `tools/cool_make.sh` is make at low priority that pauses above a CPU temperature (93 °C by default).
- Desktop native build (optional): `make -C src/native` needs DXVK/FAudio/SDL2 in `~/proto-re-data`; the 32-bit hook
  host is `make -C src/host` (mingw-w64).

## Tests and verification

There is no standalone test binary for the Android port. The available checks are:

- `bash src/native/tests/run_hot_test.sh` — builds and runs the native-replacement difftest for the hot functions on
  the host; needs `PROTORE_ENGINE` and `python3` with `pefile`.
- Differential tests in `src/host` run inside the real game on Windows/Wine through the `binkw32.dll` proxy:
  build `src/host`, then set `PROTORE_DIFFTEST=1` (run after `EngineInitialize`, keep playing) or
  `PROTORE_DIFFTEST=exit` (exit code 0 = all passed).
- CI (`.github/workflows/android-deps.yml`) only builds and uploads deps for `arm64-v8a` and `x86_64`; it must not
  touch game files.
- Always compile what you change. The Android build is the real check:
  `tools/cool_make.sh -C src/native PLATFORM=android ABI=arm64-v8a`.
- Runtime logs: `proto-native.log`, `app_process64_d3d9.log`, `adb logcat -s protore`, plus
  `Android/data/com.protore.game/files/`.

## Code conventions

- C++17 for `src/native` and `src/core`; C11 for generated recompiled code. Four-space indent, braces on the same
  line, `//` comments.
- **Guest memory is not host memory.** Access it only through `Ptr<T>`, `R8/R16/R32`, `W8/W16/W32`, `Arg(c, n)` and
  `Return(c, value, argc)`. Never cast a guest address, and never pass a host pointer to the engine. Guest structs
  must match the 32-bit Windows layout exactly (see the Bink structs in `src/native/bink.cpp`).
- Each Win32 module lives in one file and registers its imports at the bottom:
  `const ImportDef kX[] = {...}; ImportRegistrar g_x(kX);` — follow the existing argument counts and stdcall names.
- Use `Log(...)` (printf-style) for anything worth having in `proto-native.log`; no `printf`, and never log secrets.
- Settings go through `Setting`/`SettingInt` with a default; every key is overridable as `PROTORE_<KEY>`. Add new
  keys to `android/protore.ini.example` and both READMEs. Device-dependent defaults belong in `Device()`
  (`src/native/device.cpp`), not hardcoded in call sites.
- Android-only code is under `#ifdef __ANDROID__`; desktop/Wine code paths must keep building.
- When the engine needs a file (`.rcf`, `.bik`, ...), go through its own virtual file system / callbacks instead of
  bypassing it; the Bink movie path is the reference.
- Save states and the engine's memory are 32-bit and shared with recompiled code; do not hold guest pointers across
  calls that can reallocate or free guest memory.

## Commits

- Short, descriptive subject in sentence case; add a short body for multi-part changes. The history uses no
  conventional-commit prefixes.
- Stage only intended files; never commit `build/`, generated code, game assets, logs, or APKs.
- Check `git diff --check` and `git status` before committing.
