# proto-re

Русский | [English](README.md)

Нативный порт Prototype (2009) на Android. Не эмулятор и не Wine: `prototypeenginef.dll` статически
перекомпилирован из x86 в C, собран под arm64 и работает поверх своей прослойки Win32/D3D9 (SDL2, DXVK, FAudio).
В APK нет файлов игры, их надо скопировать из своей копии.

**v0.2.0 — последнее обновление.** Дальше над портом я не работаю, код остаётся здесь как есть.

Если порт понравился, можно поддержать меня на Patreon: **[patreon.com/VinTeRuS](https://www.patreon.com/cw/VinTeRuS/membership)** (от $3 в месяц).

## Что есть

На телефоне работают меню, звук, миссии, сохранения. Полное прохождение ещё не проверял. Русский текст работает (фанатский перевод).

- сенсорное управление, редактор раскладки, чувствительность камеры, инверсия Y, вибрация
- геймпад
- на Mali системный Vulkan, на Adreno Turnip (Mesa) через libadrenotools
- настройки в игре: разрешение рендера, текстуры, лимит FPS, экономия батареи
- ADPF, чтобы телефон не гонял процессор на максимуме
- ролики Bink 1 (вступление, катсцены, Web of Intrigue) декодируются через FFmpeg, звук идёт через SDL
- автоматические профили устройства: при первом запуске качество подбирается по видеочипу и памяти

Pixel 6 (Mali-G78): в меню 60 fps, в игре 38-46 в 720p и 33-42 в 1080p. Сейчас всё упирается в процессор.
Телефон с Adreno 735: в среднем около 40 fps в 720p.

## Известные проблемы

- Видеокарты с драйвером только Vulkan 1.1 или 1.2 (старые Mali вроде G52/G57/G76, PowerVR, часть Adreno 6xx/7xx
  без Turnip) не потянут: графическому слою (DXVK) нужен Vulkan 1.3.
- Adreno 8xx (Snapdragon 8 Elite и новее): родной драйвер Qualcomm не работает, игра берёт встроенный Turnip; если и
  он не подойдёт, положите более свежую сборку Turnip в `files/driver/` (см. ниже).
- В игре упор в процессор, на Pixel 6 около 35-45 fps.

## Установка

Android 10+, arm64 и купленная игра (PC, Steam app 10150, сборка 19788432).

1. Поставить `prototype.apk` и запустить. Без файлов игры он покажет, куда их класть, и сам создаст папку.
2. Скопировать `prototypeenginef.dll`, `proto_uv.dat`, все `.rcf` и `.p3d` и папки `art`, `audio`, `scripts`
   со всем содержимым (без `proto_uv.dat` игра закрывается сразу после запуска) либо в
   `/sdcard/Android/data/com.protore.game/files/game/` (на Android 11+ файловые менеджеры туда обычно не пускают,
   проще скопировать с ПК по USB), либо в `/sdcard/Prototype/` (любой файловый менеджер; на экране запуска нажать
   "Allow file access"). Для русского нужны `.rcf` с наложенным переводом.
3. По желанию положить `protore.ini` в `/sdcard/Android/data/com.protore.game/files/`
   (пример: [android/protore.ini.example](android/protore.ini.example)). Нужны права на чтение, `chmod 666`.

Логи там же (`proto-native.log`, `app_process64_d3d9.log`) и в `adb logcat -s protore`.

Ключи `protore.ini`:

| ключ | |
|---|---|
| `device_profile` | `auto` (по умолчанию), `off`, `low`, `medium`, `high`, `flagship` |
| `render_height` | 540, 720, 900, 1080, 1440, 0 = экран |
| `texture_skip_mips` | 0 оригинал, 1 вдвое меньше, 2 вчетверо |
| `fps_limit` | 30 / 45 / 60 / 0 = только vsync |
| `battery_saver` | 30 fps, без привязки к большим ядрам |
| `perf_hint` | ADPF вкл/выкл |
| `vulkan_driver` | `auto` / `system` / `turnip` |
| `audio_driver` | `aaudio` (по умолчанию) / `openslES`, если заикается звук |
| `bink_audio` | звук роликов вкл/выкл |
| `bink_swap_uv` | 1, если в роликах неправильные цвета (меняет местами плоскости цветности) |
| `present_stats` | статистика DXVK в лог |
| `import_profile`, `watchdog` | профилирование, отладка зависаний |

Любой ключ можно задать переменной окружения `PROTORE_<KEY>`.

`device_profile` задаёт значения по умолчанию для `render_height`, `texture_skip_mips` и `fps_limit`; явно заданный
ключ важнее. `auto` смотрит модель GPU (`Adreno840v2` на Oppo Find N6) и объём памяти:

| профиль | устройства | рендер | мипы | fps |
|---|---|---|---|---|
| flagship | Adreno 8xx, 8 ГиБ+ (Snapdragon 8 Elite и новее, например Find N6) | 1080 | 0 | 60 |
| high | Adreno 740+, Mali-G8x/Xclipse, 8 ГиБ+ | 900 | 1 | 60 |
| medium | остальные Adreno 6xx/7xx (710–735), Mali-G7x, 6 ГиБ+ | 720 | 1 | 0 (vsync) |
| low | 4 ГиБ и меньше | 540 | 1 | 0 (vsync) |

На Find N6 (Snapdragon 8 Elite Gen 5, Adreno 840, 16 ГиБ) включается профиль `flagship`: 1080p, текстуры в полном
разрешении и лимит 60 fps. Лимит не даёт процессору работать на максимуме на 120-герцовом экране; `fps_limit = 0`
его снимает, а `render_height = 1440` делает картинку резче (дороже для GPU). Тактовые частоты по-прежнему
регулирует ADPF, поэтому при запасе времени телефон сбрасывает частоты.

Драйверы на Adreno: игра пробует Turnip 26.2, потом Turnip 26.3 (для Adreno 710/720), потом драйвер телефона (нужен
Vulkan 1.3). На Adreno 8xx первым пробуется Turnip 26.3. Чтобы использовать свой драйвер (`.so` Vulkan-драйвера для Adreno,
например сборку Turnip для эмуляторов), положите его в `Android/data/com.protore.game/files/driver/`, игра попробует
его первым. Удалите оттуда, чтобы вернуть как было. На Android 16 с Adreno 840 проверена сборка Turnip Gen8 V37
(Mesa 26.3.0-R6) из
[StevenMXZ/Adreno-Tools-Drivers](https://github.com/StevenMXZ/Adreno-Tools-Drivers) в `files/driver/`;
сборки Turnip 27.0.0 давали вертикальные полосы.

## Если что-то не работает

**К каждому issue прикладывайте логи.** Без них краш не исправить, одно «вылетает» ничего не говорит.

Логи лежат в `Android/data/com.protore.game/files/` (на Android 11+ их удобнее забрать с ПК по USB или файловым
менеджером, который открывает Android/data). Прикладывайте сами файлы целиком, а не куски:

- `proto-native.log`: лог движка за последний запуск
- `proto-native.prev.log`: предыдущий запуск (в нём видно краш, который был при прошлом старте)
- `app_process64_d3d9.log`: лог DXVK (графика)
- `turnip_failed`, если такой файл есть

И напишите модель телефона, процессор (например Snapdragon 8 Gen 2 / Dimensity 9000 / Exynos 1380), версию Android,
версию APK и когда проблема появляется (при запуске, в меню, в игре).

## Сборка

Linux (или Windows, см. ниже), Android NDK, JDK, SDK build-tools, python3 с `capstone` и `pefile`, cmake, ninja
и meson >= 1.4 (для Turnip ещё mako, pyyaml, bison, flex, glslangValidator). `PROTORE_ENGINE` указывает на
оригинальный DLL
(по умолчанию `~/Downloads/Prototype (2009)/Prototype/prototypeenginef.dll`).

```sh
android/build-deps.sh arm64-v8a ~/proto-re-data/android-deps-src ~/proto-re-data/android-deps/arm64-v8a
android/build-turnip.sh ~/proto-re-data/android-deps-src ~/proto-re-data/android-turnip
python3 -I tools/recomp/data_entries.py
python3 -I tools/recomp/recomp.py --functions all --base 0x7c000000 --split 400 --out build/recomp/native
tools/cool_make.sh -C src/native PLATFORM=android ABI=arm64-v8a
# для телефонов на ARMv8.0 (Snapdragon 6xx и старше); APK сам выбирает нужную сборку при запуске
tools/cool_make.sh -C src/native PLATFORM=android ABI=arm64-v8a ARM_ARCH=armv8-a OUT=../../build/android/arm64-v8a-v80
android/build-apk.sh arm64-v8a
```

`android/build-deps.sh` собирает SDL2, FAudio, DXVK и статический FFmpeg (демуксер bink и декодеры bink/binkaudio).
`cool_make.sh` это make на всех ядрах, который ставит сборку на паузу при 93°C.

### Windows

Сборка работает и на Windows без WSL. Нужны Git Bash (в нём есть `sh`, `touch`, `zip` и `realpath`), Windows NDK
28.2.13676358, JDK, SDK build-tools, CMake, Ninja, Meson >= 1.4, нативный gcc для host-утилит FFmpeg и
`bison`/`flex` для Turnip.

- В cross-файлах Meson для DXVK/Turnip укажите `clang.exe`/`clang++.exe` с `--target=aarch64-linux-android26`
  (29 для Turnip) и `--sysroot=...`: Meson на Windows не может запускать скрипты-обёртки NDK без расширения.
- `src/native` собирайте с `TOOLCHAIN=.../toolchains/llvm/prebuilt/windows-x86_64/bin` и переменными `DEPS`,
  `DXVK`, `TURNIP`. Если итоговая команда линковки длиннее лимита Windows, передайте список объектов через
  response-файл (`@objs.rsp`).
- `android/build-apk.sh` рассчитан на Linux-пути; используйте Windows-версии `llvm-strip.exe`, `d8.bat` и
  `apksigner.bat` (или выполните шаги `aapt2`/`d8`/`zipalign`/`apksigner` вручную).

Проверено на телефоне Snapdragon 8 Elite Gen 5 / Adreno 840 под Android 16.

## Тестирование

Спасибо Copyright-1 за тесты на Adreno.

## Авторство

Если вы делитесь этим портом, выкладываете его сборки или используете его код, **обязательно укажите автора и дайте
ссылку на этот репозиторий:** https://github.com/VinTeRuS/proto-re

Русский текст: фанатский перевод от ElArchibald.

В репозитории нет файлов игры, оригинального DLL и сгенерированного из него кода, DRM порт не обходит: нужна своя
копия игры. Prototype принадлежит Activision / Radical Entertainment.
