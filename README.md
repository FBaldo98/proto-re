# proto-re

Нативный порт **Prototype (2009, Radical Entertainment)** на Android через реверс-инжиниринг движка.

Не эмуляция: код движка (`prototypeenginef.dll`) переводится в C статическим перекомпилятором
x86 → C и вручную реконструируется по подсистемам. Затем он собирается под arm64 с собственным
платформенным слоем. В APK лежит только наш код, файлы игры пользователь копирует из своей копии.

Текущее состояние и план: [ROADMAP.md](ROADMAP.md).

## Условия

- Только легально купленная копия игры (PC, Steam app 10150, сборка **19788432**).
- Ассеты игры (`.rcf`, `.p3d`, `.bik`, …), исходный DLL, декомпилированный код и сгенерированный
  перекомпилятором C в репозиторий **не попадают** (`.gitignore`: `build/`, `re/decomp/`, `ghidra/`).
- DRM не обходим: папку `Crack/` из раздач не используем.
- Порт для личного использования, ассеты игры не распространяем.

## Как это устроено

```
prototypef.exe ──LoadLibrary──▶ prototypeenginef.dll ──импорт──▶ binkw32.dll  (наш прокси)
                                     ▲                               │
                                     │  jmp-хуки на входах функций   │
                                     └───────────────────────────────┘
          ├─ ручные реконструкции (src/core, src/math), проверенные дифтестами
          └─ перекомпилированный движок (build/recomp/*, мост src/recomp/bridge_x86.cpp)
```

1. **Хост-прокси.** `src/host` собирается как `binkw32.dll` и пересылает настоящие вызовы Bink в
   `binkw32_orig.dll`. Прокси загружается раньше `EngineInitialize`, сверяет сборку движка
   (TimeDateStamp `0x689E2026`) и ставит хуки.
2. **Ручная реконструкция (по методике re3/reVC).** Функция переписывается на C++ и ставится
   хуком поверх оригинала. Затем она проверяется дифференциальным тестом прямо внутри процесса:
   на одних и тех же входах вызываются оригинал и реконструкция, результаты сравниваются.
3. **Статический перекомпилятор.** `tools/recomp/recomp.py` (capstone) переводит каждую функцию
   движка в `void f_<va>(Ctx*)` с явным состоянием процессора:
   - точные флаги с анализом живости;
   - модели x87 и SSE;
   - таблицы переходов, setjmp/longjmp, атомарные операции.

   Весь движок: 73 595 функций, неподдерживаемых инструкций 0. Мост переключает стеки между
   нативным и перекомпилированным кодом, у каждого файбера свой стек.

## Структура

| Путь | Что там |
|---|---|
| `src/host/` | прокси `binkw32.dll`: установка хуков, реестр хуков, дифтесты, пробы, `memwatch`, `WorldStop` |
| `src/core/`, `src/math/` | реконструированный код движка (Report, хеш имён, Cement/RCF, память, файлы, запросы) |
| `src/tests/` | дифференциальные тесты реконструкций |
| `src/recomp/` | рантайм перекомпилированного кода (`runtime.h`, `runtime_fp.h`) и мост x86 |
| `tools/rcf.py`, `p3d.py`, `rcfdiff.py` | чтение архивов RCF 2.1, дерева чанков Pure3D, сравнение архивов |
| `tools/recomp/` | перекомпилятор и `bisect.sh` (поиск функции, ломающей игру) |
| `tools/hookgen.py`, `disfn.sh` | заготовки хуков и дизассемблирование функций по адресу |
| `tools/ghidra/` | скрипты экспорта функций и декомпиляции для `analyzeHeadless` |
| `re/subsystems.md` | все находки: архитектура, форматы, подсистемы |
| `re/issues.md` | открытые и закрытые проблемы |
| `re/functions.csv` | инвентарь функций из Ghidra (адрес, размер, thunk, имя) |
| `android/` | будущий Gradle/NDK-проект |

## Требования (Linux)

- `mingw-w64-gcc` (i686): сборка прокси;
- `wine` + `dxvk`: запуск оригинала;
- `python3` с `capstone` и `pefile`: перекомпилятор и инструменты;
- `llvm-objdump`: `disfn.sh`;
- `xdelta3`, `innoextract`: разбор русского патча;
- Ghidra (headless): анализ движка.

## Подготовка

```sh
# Тестовая папка игры: символические ссылки на оригинальную установку
mkdir -p ~/proto-re-data/game && cd ~/proto-re-data/game
ln -s "$HOME/Downloads/Prototype (2009)/Prototype/"* .
rm binkw32.dll
cp "$HOME/Downloads/Prototype (2009)/Prototype/binkw32.dll" binkw32_orig.dll

# Wine-префикс с DXVK
export WINEPREFIX=~/proto-re-data/wineprefix
wineboot -i && setup_dxvk install
```

Путь к оригинальному DLL для инструментов задаётся через `PROTORE_ENGINE`
(по умолчанию `~/Downloads/Prototype (2009)/Prototype/prototypeenginef.dll`).

## Сборка и запуск

### Нативная версия (без Wine)

Зависимости в `~/proto-re-data/deps`: DXVK 2.5.3 (собран с `-Denable_d3d9=true -Ddxvk_native_wsi=sdl2` в
`build-native`), FAudio 25.10 (`build-native`); SDL2 из системы.

```sh
python3 -I tools/recomp/data_entries.py                                                 # точки входа вне Ghidra
python3 -I tools/recomp/recomp.py --functions all --out build/recomp/all --split 400     # ~3 мин
make -C src/native -j4                                   # первый раз ~11 мин, потом секунды
cd ~/proto-re-data/native_run && PROTORE_WINDOWED=1 ~/proto-re/build/native/proto-native ~/proto-re-data/game
```

Лог пишется в `proto-native.log` в текущей папке.

### Android

```sh
android/build-deps.sh arm64-v8a ~/proto-re-data/android-deps-src ~/proto-re-data/android-deps/arm64-v8a
make -C src/native -j4 PLATFORM=android ABI=arm64-v8a     # build/android/arm64-v8a/libmain.so
android/build-apk.sh arm64-v8a                           # build/apk/prototype.apk
adb install -r build/apk/prototype.apk
```

Файлы игры (`prototypeenginef.dll`, все `.rcf`, `.p3d`, папки `art`, `audio`, `scripts`) копируются в
`/sdcard/Android/data/com.protore.game/files/game/`. Лог: `adb logcat -s protore`.
Зависимости (SDL2, FAudio, DXVK) также собираются на GitHub Actions (`android-deps`, артефакты на 30 дней);
код игры и сгенерированный из неё код на GitHub не попадают.

### Через Wine (прокси с хуками)

Прокси с ручными хуками и прототипным набором перекомпилированных функций:

```sh
make -C src/host -j4
cp build/host/binkw32.dll ~/proto-re-data/game/
cd ~/proto-re-data/game && wine prototypef.exe     # лог: proto-re.log
```

Весь движок через перекомпилятор (генерация ~8 мин, сборка ~15 мин, DLL ~65 МБ):

```sh
python3 -I tools/recomp/recomp.py --functions all --out build/recomp/all --split 400
make -C src/host -j4 OUT=../../build/host_all RECOMP_DIR=../../build/recomp/all
cp build/host_all/binkw32.dll ~/proto-re-data/game/
PROTORE_RECOMP=all wine prototypef.exe
```

## Переключатели (переменные окружения)

| Переменная | Действие |
|---|---|
| `PROTORE_NOHOOKS=1` | оригинальный движок через тот же прокси (A/B-сравнение) |
| `PROTORE_NULLPATCH=1` | записать на место хуков исходные байты (проверка самой установки) |
| `PROTORE_ONLY=a,b` / `PROTORE_SKIP=a,b` | включить или исключить хуки по префиксам имён |
| `PROTORE_DIFFTEST=1` / `=exit` | прогнать дифтесты в процессе (и выйти) |
| `PROTORE_DIFFTEST_RECOMP=1` | дифтесты «перекомпилированное против оригинала» |
| `PROTORE_RECOMP=1` / `=all` | прототипный набор / весь движок через перекомпилятор |
| `PROTORE_RECOMP_RANGE=lo-hi` | перекомпилированы только функции в диапазоне адресов (hex, hi не включается) |
| `PROTORE_PROBE=openbykey\|watchdir` | диагностические пробы файловой системы |
| `PROTORE_TRACE_FPU=1` | трассировка вызовов CRT, зависящих от состояния FPU |
| `PROTORE_WINDOWED=1` | нативная версия: окно вместо полного экрана |
| `PROTORE_SHOT=300,2000` | нативная версия: сохранить кадры с этими номерами в `shot_<кадр>.ppm` |
| `PROTORE_INPUT=2100:Return,...` | нативная версия: нажать клавишу (имя SDL) на заданном кадре |
| `PROTORE_TRACE_FILES=1` | нативная версия: писать в лог каждое открытие файла |

Поиск функции, ломающей игру:
`MIN_FRAMES=64 RUN_SECONDS=70 [START_LO=N] tools/recomp/bisect.sh`.
Шаг засчитывается, только если движок прошёл не меньше `MIN_FRAMES` кадров главного цикла. Во время поиска окно
игры само открывается и закрывается, в части запусков чёрный экран, так и задумано.

## Русский язык

Фанатский текстовый перевод (ElArchibald) — это xdelta-патчи к 8 архивам RCF: субтитры,
текстовые таблицы, HUD `.gfx` и шрифты (подробности в `re/subsystems.md`). Порт подключит его как
языковой оверлей поверх оригинальных архивов. Русской озвучки в исходных файлах нет.
