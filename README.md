# proto-re

[Русский](README.ru.md) | English

Native Android port of Prototype (2009). Not an emulator and not Wine: `prototypeenginef.dll` is statically
recompiled from x86 to C, built for arm64 and run on top of my own Win32/D3D9 layer (SDL2, DXVK, FAudio).
The APK has no game data in it, you copy the files from your own copy of the game.

**v0.2.0 is the last update.** I'm not planning more work on the port; the code stays here as it is.

If you like the port, you can support me on Patreon: **[patreon.com/VinTeRuS](https://www.patreon.com/cw/VinTeRuS/membership)** (from $3 a month).

## Status

Runs on the phone: menus, sound, missions, saves. A full playthrough is not tested yet. Russian text works (fan translation).

- touch controls with a layout editor, camera sensitivity, invert Y, vibration
- gamepad
- Mali uses the system Vulkan driver, Adreno uses Turnip (Mesa) through libadrenotools
- settings in game: render resolution, textures, FPS limit, battery saver
- ADPF hints so the phone doesn't run the CPU at max all the time
- Bink 1 movies (intro, cutscenes, Web of Intrigue) decode with FFmpeg; the sound goes through SDL
- automatic device profiles: the first start picks quality defaults from the GPU and RAM

Pixel 6 (Mali-G78): menus 60 fps, gameplay 38-46 at 720p and 33-42 at 1080p. Gameplay is CPU bound right now.
A phone with Adreno 735: about 40 fps on average at 720p.

## Known issues

- GPUs whose driver only has Vulkan 1.1 or 1.2 (older Mali like G52/G57/G76, PowerVR, some Adreno 6xx/7xx without
  Turnip) can't run it: the graphics layer (DXVK) needs Vulkan 1.3.
- Adreno 8xx (Snapdragon 8 Elite and newer): Qualcomm's own driver fails, the game uses the bundled Turnip; if that
  doesn't work on your phone, put a newer Turnip build in `files/driver/` (see below).
- Gameplay is CPU bound, about 35-45 fps on a Pixel 6.

## Installing

Android 10+, arm64, and a bought copy of the game (PC, Steam app 10150, build 19788432).

1. Install `prototype.apk` and start it. Without the game files it shows where to put them and creates the folder.
2. Copy `prototypeenginef.dll`, `proto_uv.dat`, all `.rcf` and `.p3d` files and the `art`, `audio`, `scripts`
   folders with their contents (the game closes right after starting without `proto_uv.dat`) to either
   `/sdcard/Android/data/com.protore.game/files/game/` (file managers often can't open it on Android 11+, copy from a
   PC over USB) or `/sdcard/Prototype/` (any file manager; tap "Allow file access" on the setup screen).
   For Russian, use the `.rcf` files with the translation applied.
3. Optionally put `protore.ini` in `/sdcard/Android/data/com.protore.game/files/`
   (see [android/protore.ini.example](android/protore.ini.example)). It has to be readable, `chmod 666`.

Logs go to the same folder (`proto-native.log`, `app_process64_d3d9.log`) and to `adb logcat -s protore`.

`protore.ini` keys:

| key | |
|---|---|
| `device_profile` | `auto` (default), `off`, `low`, `medium`, `high`, `flagship` |
| `render_height` | 540, 720, 900, 1080, 1440, 0 = screen |
| `texture_skip_mips` | 0 original, 1 half, 2 quarter |
| `fps_limit` | 30 / 45 / 60 / 0 = vsync only |
| `battery_saver` | 30 fps, no big core pinning |
| `perf_hint` | ADPF on/off |
| `vulkan_driver` | `auto` / `system` / `turnip` |
| `audio_driver` | `aaudio` (default) / `openslES`, try it if the sound stutters |
| `bink_audio` | movie sound on/off |
| `bink_swap_uv` | 1 if movie colours look wrong (swaps the chroma planes) |
| `present_stats` | DXVK stats in the log |
| `import_profile`, `watchdog` | profiling, hang diagnostics |

Every key also works as an env var `PROTORE_<KEY>`.

`device_profile` sets the defaults of `render_height`, `texture_skip_mips` and `fps_limit`; any key set explicitly
wins. `auto` reads the GPU model (`Adreno840v2` on the Oppo Find N6) and the RAM:

| profile | devices | render | mip skip | fps |
|---|---|---|---|---|
| flagship | Adreno 8xx, 8 GiB+ (Snapdragon 8 Elite and newer, e.g. Find N6) | 1080 | 0 | 60 |
| high | Adreno 740+, Mali-G8x/Xclipse, 8 GiB+ | 900 | 1 | 60 |
| medium | other Adreno 6xx/7xx (710-735), Mali-G7x, 6 GiB+ | 720 | 1 | 0 (vsync) |
| low | 4 GiB or less | 540 | 1 | 0 (vsync) |

On the Find N6 (Snapdragon 8 Elite Gen 5, Adreno 840, 16 GiB) the profile is `flagship`: 1080p internal resolution,
full-resolution textures and a 60 fps cap. The 60 fps cap keeps the 120 Hz panel from running the CPU at full tilt;
`fps_limit = 0` removes it, and `render_height = 1440` sharpens the upscaled picture at some GPU cost. The clock
and power hints still go through ADPF, so the phone drops its clocks when a frame has headroom.

GPU drivers on Adreno: the game tries Turnip 26.2, then Turnip 26.3 (for Adreno 710/720), then the phone's own driver
(needs Vulkan 1.3). On Adreno 8xx it tries Turnip 26.3 first. To use your own driver (an Adreno Vulkan driver `.so`,
for example a Turnip build made for emulators), put it in `Android/data/com.protore.game/files/driver/`; the game
tries it first. Delete it from there to go back. On Android 16 with Adreno 840, a Turnip Gen8 V37 (Mesa 26.3.0-R6)
build from [StevenMXZ/Adreno-Tools-Drivers](https://github.com/StevenMXZ/Adreno-Tools-Drivers) in `files/driver/` was
verified; Turnip 27.0.0 builds rendered vertical-line artifacts there.

## Reporting a problem

**Attach the logs to every issue.** Without them a crash can't be fixed, "it crashes" alone tells nothing.

The logs are in `Android/data/com.protore.game/files/` (on Android 11+ copy them with a PC over USB or a file
manager that can open Android/data). Attach these files as they are, don't paste parts of them:

- `proto-native.log`: the engine log of the last run
- `proto-native.prev.log`: the run before it (it shows a crash that happened on the previous start)
- `app_process64_d3d9.log`: the DXVK (graphics) log
- `turnip_failed`, if that file exists

Also write the phone model, the chip (for example Snapdragon 8 Gen 2 / Dimensity 9000 / Exynos 1380), the Android
version, the version of the APK and when the problem happens (at start, in the menu, in the game).

## Building

Linux (or Windows, see below) with the Android NDK, JDK, SDK build-tools, python3 with `capstone` and `pefile`,
cmake, ninja and meson >= 1.4 (Turnip also needs mako, pyyaml, bison, flex, glslangValidator). `PROTORE_ENGINE`
points to the original DLL (default `~/Downloads/Prototype (2009)/Prototype/prototypeenginef.dll`).

```sh
android/build-deps.sh arm64-v8a ~/proto-re-data/android-deps-src ~/proto-re-data/android-deps/arm64-v8a
android/build-turnip.sh ~/proto-re-data/android-deps-src ~/proto-re-data/android-turnip
python3 -I tools/recomp/data_entries.py
python3 -I tools/recomp/recomp.py --functions all --base 0x7c000000 --split 400 --out build/recomp/native
tools/cool_make.sh -C src/native PLATFORM=android ABI=arm64-v8a
# for ARMv8.0 phones (Snapdragon 6xx and older); the APK picks the right build at launch
tools/cool_make.sh -C src/native PLATFORM=android ABI=arm64-v8a ARM_ARCH=armv8-a OUT=../../build/android/arm64-v8a-v80
android/build-apk.sh arm64-v8a
```

`android/build-deps.sh` builds SDL2, FAudio, DXVK and a static FFmpeg (the Bink demuxer and the bink/binkaudio
decoders). `cool_make.sh` is make on all cores that pauses at 93°C.

### Windows

The build also works on Windows without WSL. Use Git Bash as the shell (it provides `sh`, `touch`, `zip` and
`realpath`), the Windows NDK 28.2.13676358, JDK, SDK build-tools, CMake, Ninja, Meson >= 1.4, a native gcc for
FFmpeg's host tools, and `bison`/`flex` for Turnip.

- Point the DXVK/Turnip Meson cross files at `clang.exe`/`clang++.exe` with `--target=aarch64-linux-android26`
  (29 for Turnip) and `--sysroot=...`. Meson on Windows cannot execute the NDK's extensionless wrapper scripts.
- Build `src/native` with `TOOLCHAIN=.../toolchains/llvm/prebuilt/windows-x86_64/bin` and `DEPS`, `DXVK`, `TURNIP`
  set. If the final link exceeds the Windows command-line limit, pass the object list through a response file
  (`@objs.rsp`).
- `android/build-apk.sh` assumes Linux tool paths; use the Windows `llvm-strip.exe`, `d8.bat` and `apksigner.bat`
  (or run the equivalent `aapt2`/`d8`/`zipalign`/`apksigner` steps directly).

This was verified on a Snapdragon 8 Elite Gen 5 / Adreno 840 phone running Android 16.

## Testing

Thanks to Copyright-1 for testing on Adreno.

## Credits

If you share this port, publish a build of it, or use its code, **credit the author and link this repository:**
https://github.com/VinTeRuS/proto-re

Russian text: ElArchibald's fan translation.

The repository has no game files, original DLL or code generated from it, and the port does not bypass any DRM:
you need your own copy of the game. Prototype belongs to Activision / Radical Entertainment.
