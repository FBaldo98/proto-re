# Prototype (2009) PC: результаты этапа 1 (инвентаризация)

Цель: Steam-сборка 19788432.
- `prototypeenginef.dll`: SHA-256 `d331be205bdde99897952ee88261ab7e63c5c65e36a4660e2a367886a3537071`
- `prototypef.exe`: тонкий хост

Источники: импорты/экспорты (`winedump`), строки (`strings`), MSVC RTTI.
Ghidra-анализ через REA ещё не завершён.

## Архитектура запуска (наблюдение)
`prototypef.exe` только создаёт окно Win32 и цикл сообщений, затем делает `LoadLibraryA` на движок
и вызывает экспортируемые функции:
`EngineInitialize`, `EngineService`, `EngineTerminate`, `EngineKeyboardService`,
`EngineMouseService`, `EngineWindowFocus` (адреса экспорта: 0x0085EFB0, 0x0085EB20, 0x0085E340, …).
→ Хост-слой для Android = собственная реализация этих 6 точек входа поверх SDL.
→ Для hook-and-replace удобно: свой exe-хост может грузить оригинальную DLL и ставить хуки до `EngineInitialize`.

## Компилятор
MSVC 2005 (MSVCR80), x86, внутренний путь сборки `atg\engine\code\...`.

## Импорты движка (что нужно заменить на Android)
| DLL | Функций | Назначение | Замена |
|---|---|---|---|
| d3d9 | 1 (`Direct3DCreate9`) | рендер через COM-интерфейсы | Vulkan / GLES-рендер |
| DSOUND | 1 | звук (DirectSound, не XACT) | miniaudio / AAudio |
| binkw32 | 17 | FMV-ролики | FFmpeg bink decoder |
| XINPUT1_3 | 3 | геймпад | SDL gamepad |
| WINMM | 11 | таймеры/мультимедиа | std::chrono / SDL |
| WS2_32 + iphlpapi + RPCRT4 + SETUPAPI | 29+4+2+6 | сеть/онлайн-телеметрия, перечисление устройств | заглушки |
| KERNEL32/USER32/ADVAPI32/SHELL32/SHLWAPI/ole32/OLEAUT32 | ~150 | ОС, реестр, пути | POSIX/SDL |
| MSVCR80 | 140 | CRT | bionic libc/libc++ |

## Встроенные сторонние библиотеки (наблюдение по строкам)
- **Scaleform GFx** (`G*`-классы в экспорте, `GFx*` RTTI, обёртка `scaleformp3d`): все меню и HUD
  (`art\hud\proto*.gfx`: protomainmenu, protohud, protooptions, protosaveload…).
  Проприетарная → **заменять**. Вариант: open-source Flash-плеер с поддержкой AS2
  (например Ruffle, Rust, MIT/Apache) + реализация расширений Scaleform и callback'ов
  `CustomCallbackHandler`/`TextBibleInterfaceHandler`. Проверить версию GFx и AS2/AS3 по самим .gfx.
- **zlib 1.2.3** (`Decompressor_Zlib`) → системный zlib.
- **libpng 1.2.8** → libpng/stb_image.
- **Lua 5.1**, скрипты — **байткод** (`scripts/frevents.lua` начинается с `\x1bLuaQ`):
  заголовок: little-endian, int=4, **size_t=4**, instruction=4, **lua_Number=4 байта, float**
  (нестандартная сборка `LUA_NUMBER=float`). Для arm64: свой загрузчик чанков с 32-битным size_t,
  либо декомпиляция байткода и перекомпиляция под 64 бит.
- Физика — **собственная** `ravenphysics` (Havok не найден) → декомпилируем.
- Bink — внешняя DLL, не внутри движка.

## Подсистемы (по RTTI, 11 808 классов)
Намеспейсы по числу классов: proto 1079, motion 699, engine 570, ai 322, pure3d 264,
core 96, fight 90, content 65, ravenphysics 56, mgraph 53, alert 52, audio 49, om 48,
scenario 38, music 32, animation 25, vehicle 22, render 21, scaleformp3d 19, controller 18,
camera 17, framebufferfx 14, choreo 14, online 13, driving 13, movie 7, crypto 7, compress 2.

Деревья исходников: `code\core`, `code\math`, `code\content` (метатипы, загрузка `.tod`),
`code\pure3d`, `code\choreo` (анимация/позы), `code\ravenphysics`,
`code\engine\{render,physics,fight,animation,frontend,fmv,gameobject,system}`,
`code\proto\{ai,scenario,fightitems,freeroamevents,architecture}`.

## Форматы ассетов, упомянутые в коде
`.rcf` (Cement-архивы, `core\file\common\cementer.cpp`), `.p3d` (Pure3D), `.gfx` (Scaleform),
`.tod`, `.sls`, Lua 5.1-байткод, Bink.

## Открытые вопросы (unknowns)
- Точное число функций (ждёт Ghidra `binary_overview`).
- Версия Scaleform GFx и версия ActionScript в `.gfx`.
- Формат звука внутри `0Xaudio.rcf` (DirectSound получает PCM, значит декодер внутри движка).
- Назначение сетевого кода (`online`, `crypto`): телеметрия/античит/DLC-коды.

## Этап 2: форматы (проверено `tools/rcf.py`)
- **RCF 2.1** разобран полностью: все 11 архивов читаются. Имена сопоставляются с записями каталога
  по порядку смещений: `scripts\frevents.lua` из архива побайтно совпал с копией на диске.
  Хеш имени пока не установлен (CRC-32 от имени в нижнем регистре не подошёл).
- **`.rz`** = заголовок 16 байт `{"RZ\0\0", 0, unpacked_size, 0}` + один zlib-поток; на 80 выборочных файлах
  из всех архивов ошибок нет, внутри во всех случаях Pure3D (`P3D\xff`).
- Содержимое: `0Xaudio.rcf`: 14 614 `.p3d`, звук упакован в Pure3D-чанки; `0Xwoi.rcf`/`movies.rcf`: Bink 1
  (`BIKi`, FFmpeg поддерживает) + `.p3d`; `art.rcf`: 363 `.gfx` (**`GFX\x08` = Scaleform на базе SWF 8,
  ActionScript 2** → совместимо с AS2-рантаймами вроде Ruffle), 2219 `.p3d`, 19 `.dds`;
  `cells.rcf`: 520 ячеек мира `.p3d`.
- В `scripts.rcf` только 2 Lua-файла, значит скрипты миссий лежат внутри Pure3D-чанков: следующая цель — парсер P3D.
