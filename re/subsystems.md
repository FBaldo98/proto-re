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

## Pure3D (проверено `tools/p3d.py`)
- Файл: `{0xFF443350, 12, file_size}`; чанк: `{id, data_size, total_size}`, дочерние чанки идут после
  `data_size`. Все 520 файлов `cells.rcf` разобраны без ошибок.
- Самые объёмные чанки в `cells.rcf`: `0x00010012` (~611 МБ, предположительно вершинные данные),
  `0x00019002` (~569 МБ, данные текстур), `0x07021007` (~52 МБ).
- `0x07f00000`/`0x07f00001`: метаданные объектов (`proto::StatsManager(1)`, `engine::SkuInfo(1)`)
  с блоком `META`, то есть сериализация через метатипы `content\src\meta`.
- **`0x20000702`: сериализованные объекты формата `fig0` (версия 5).** В 86 из 1542 таких чанков в
  `art.rcf` внутри объекта лежит **Lua 5.1-байткод** (миссии `art\missions\*\*_fig.p3d`, NIS-катсцены
  `art\nis\*_fig.p3d`, пакеты `art\packages\missions\*`). Скрипты миссий — это поля
  fig-объектов, а не отдельные файлы. Для порта нужен загрузчик `fig0` + Lua 5.1 (float, size_t=4).

## Ghidra: полный анализ (12.1.4, 1147 с, проект `ghidra/prototype`)
- **102 736 функций**, из них 30 658 thunk'ов → **~72 000 настоящих функций**, 7.9 МБ кода.
- По размеру: <16 байт — 44 407 (геттеры, шаблонные инстанциации), 16–127 — 45 696,
  128–1023 — 11 773, ≥1024 — 860. Основная работа приходится на ~12 600 функций размером от 128 байт.
- Имена есть только у 115 функций (экспорты/импорты: Scaleform `G*`, `Engine*`); остальные `FUN_*`.
- Только 211 функций ссылаются на строку с путём `.cpp` (ассерты): ravenphysics 101, choreo 29, pure3d 28, engine 30…
  → для привязки остальных функций к подсистемам нужен разбор RTTI/vtable (следующий шаг).
- Полный список функций: `re/functions.csv` (entry, size, thunk, namespace, name, source_files).

## Этап 7: русская локализация (ElArchibald, 02.10.2025): что она меняет (проверено `tools/rcfdiff.py`)
Установщик Inno Setup 5.5.7 распакован через `innoextract` без запуска. Внутри только xdelta3-патчи к 8 архивам
(без контрольных сумм, только для сборки 19788432). Патчи применены к копиям в `~/proto-re-data/ru_rcf/`.
- **Озвучки нет.** В `00audio`/`01audio` у 14 318 звуковых записей меняется только чанк
  `0xFE000000` (метаданные реплики): английский текст субтитров заменён на **UTF-8 кириллицу**,
  длина записи меняется на десятки байт. Сами аудиоданные не тронуты. + 1 новая запись в `01audio` (`audio/english/audiofile`).
- `0Xwoi.rcf` / `movies.rcf`: в `.p3d`, сопровождающих Bink-ролики, меняются чанки `0x00018201`/`0x00018202`
  (вероятно, строковые таблицы субтитров FMV); 1 `.bik` заменён, 1 файл добавлен.
- `art.rcf`: 115 файлов изменено (`art/nis` 46, пешеходы 34, `art/hud` 21 вкл. 7 `.gfx`): чанки
  `0x00018200–02` (текст), `0x00019002` (текстуры: шрифты/надписи), `0x0001100c`.
  **223 файла удалены**: отладочные/тестовые миссии и локации (proptest, autotester, test_*); переводчик
  ужал архив. Для порта это неважно.
- Вывод для порта: рендер текста обязан поддерживать **UTF-8**, шрифтовые атласы берутся из пропатченного
  `art.rcf`. Оверлей = пропатченные пользователем архивы (или извлечённые из них записи) поверх оригинала.

## Восстановление классов по RTTI (Ghidra `RecoverClassesFromRTTIScript`)
- Конструкторов 5 949, деструкторов 3 471, виртуальных функций 68 548.
- Из 73 595 настоящих функций **39 512 (2.8 МБ кода) привязаны к классам**, 34 083 (5.3 МБ) остаются глобальными
  (невиртуальные методы, свободные функции, CRT, встроенный Lua/zlib/libpng).
- Крупнейшие пространства имён (функций / байт): `fight` 8 843 / 225 КБ, `proto::fightitems` 6 720 / 558 КБ,
  `proto::*` 3 633 / 462 КБ, `content` 3 499 / 197 КБ, `om` 3 342 / 145 КБ, `engine::*` 2 892 / 285 КБ,
  `mgraph` 2 496 / 147 КБ, `pure3d` 1 576 / 234 КБ, `core` 812, `ravenphysics` 281, `audio` 239, `music` 151.
- **Не переносим (сторонний код):** Scaleform GFx: 2 213 функций / 222 КБ → замена на AS2-плеер;
  `MassiveAdClient3` (сервис внутриигровой рекламы Massive, давно закрыт): 197 функций → заглушки.

## Файловая система: хеш имён (восстановлено и проверено)
- `core::HashFileName` (rva 0x00643ed0) → `core::HashString(path без ведущего '\', 0)` (rva 0x0063a5d0).
  Алгоритм: `h = h*31 + c` по знаковым символам, к каждому символу < 'a' прибавляется 0x20;
  после вычисления вызывается наблюдатель по указателю rva 0x010bc59c с (hash, string).
- Совпадает со всеми 18 044 записями всех архивов (оригинальных и русских). `tools/rcf.py` теперь
  сопоставляет имена по хешу.
- Путь поиска: `core::FileOpenRequest` → `FUN_10650cf0` → `FUN_10650ab0` → двоичный поиск по каталогу
  `FUN_106502c0(base, count, key, stride)` (найден наблюдателем за памятью `PROTORE_PROBE=watchdir`).
- `CementLibrary::vfunction8` (открытие по готовому ключу) до главного меню не вызывается.
