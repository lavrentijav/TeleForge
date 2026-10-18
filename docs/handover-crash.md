# Передача: падение Windows-сборки TeleForge (0xC0000005 / 0xC000041D)

Дата: 18 октября 2026. Всё ниже — проверенные факты, а не предположения, если не
помечено как «гипотеза».

## Симптом

Приложение падает после старта. Код выхода:

```
-1073741819 = 0xC0000005  access violation (чтение по адресу 0)
-1073740771 = 0xC000041D  необработанное исключение в колбэке
```

В диалоге Windows: «Инструкция по адресу 0x00007FF6… обратилась к памяти по
адресу 0x0000000000000000».

Падение **не** привязано к конкретным данным: воспроизводилось и на пустом
профиле, и на профиле с шестью аккаунтами.

## Что известно точно

1. **Сборка от 11:26 18.10 работала** (Windows, 262 743 552 байта, RC=0). Её
   exe был перезаписан последующими сборками.

2. **Пустой профиль доходит до конца инициализации** и падает уже в работающем
   приложении. Хвост лога успешного старта:

```
Language is not loaded
TeleForge: storage initialized OK | SchemaVersion=16
TeleForge crypto: stored secret cannot be decrypted
QDir::mkpath: Empty or null file name
TeleForge plugins: runtime disabled, 0 path(s) skipped
Application::run: primary window init complete; event loop continues
Application::run: returned after setup (Qt event loop still running in Sandbox)
Wallet Info: Windows Hello availability: ...
```

После `Wallet Info` лог обрывается — падение где-то в первые секунды работы
цикла событий.

3. **Старый профиль падает раньше**, на загрузке языка:

```
Lang Info: Loaded cached, keys: 12376
Lang Error: Unexpected tag (key 'ayu_AboutText1:api_link')
```

Это отдельный баг: кэш языковых строк хранится в `tdata/settingss` и применяется
к скомпилированным ключам **по индексу**. Добавление новых ключей в середину
`Telegram/Resources/langs/lang.strings` сдвигает индексы, кэш перестаёт
соответствовать, в ключ, не принимающий тег, попадает строка с `{api_link}`, и
парсер (`Lang::ValueParser`) падает. Пустой профиль не имеет кэша (`Language is
not loaded`) и этот баг не воспроизводит.

4. **Python-интерпретатор участвует.** Если убрать каталог `modules/` рядом с
exe, приложение завершается с:

```
Could not find platform independent libraries <prefix>
Fatal Python error: Failed to import encodings module
```

Все точки инициализации Python находятся в
`Telegram/SourceFiles/tfc/features/plugins/plugin_runner.cpp` (строки ~76, 98,
102, 105). Сейчас `PluginRunner::ensureStarted()` и `reloadEnabled()` принудительно
отключены (см. коммиты), но падение осталось — значит Python не единственная
причина, либо инициализация идёт ещё откуда-то.

5. **Аккаунты не переносятся копированием `tdata`.** Telegram Desktop привязывает
локальное хранилище к пути рабочего каталога. После смены `-workdir` создаются
новые каталоги аккаунтов и показывается экран ввода номера.

6. **Дампы падений есть** (см. пути ниже), но символы получить не удалось: PDB
весит 4.3 ГБ, `dbghelp` отказывается (`SymFromAddr` возвращает ошибку 87).
Разрешение по `.map` работает, но адреса съезжают между сборками.

## Проверено и НЕ является причиной

- Данные профиля (падает и с пустым).
- Язык интерфейса (`-lang en` и `-lang ru` — оба падают).
- Русский оверлей (`ApplyBundledRussianOverlay` отключён — падение осталось).
- Настройки форка (`ayu_settings.json` убран — падение осталось).
- Каталог `plugins/`, `modules/` (переименование не помогает от 0xC000041D).
- Откат правок слайдеров/настроек (`5323915c0b`) и языка (`731aa4fb09`).
- Полный откат `tfc/**` к известному рабочему состоянию `ec5f687f4a` — пустой
  профиль стал запускаться, но падение на живом профиле осталось.

## Состояние репозитория

`D:\TeleForge\tdesktop`, ветка `dev`, remote `lavrentijav/TeleForge`.

Последние коммиты (сверху новые):

```
a2b720b61d Revert "Let the archive password be set from the sync settings"
859f510ca8 Keep the archive passphrase encrypted on disk without decrypting it during load
db3a2bc9ca Restore the last known good runtime state   ← tfc/** возвращён к ec5f687f4a
eaee91082c Keep the plugin interpreter from taking the app down at startup
```

Важно: рабочее дерево содержит незакоммиченные изменения в
`Telegram/lib_ui`, `Telegram/ThirdParty/zxcvbn`, `cmake` (указатели сабмодулей) —
их не трогать.

Функциональность, которая была сделана и откачена (нужна позже, не сейчас):
шифрование архива на ключе аккаунта (Argon2id), самописывающийся бандл
`TFSYNC2`, поле пароля архива в настройках синхронизации. Тесты на крипту лежат
в `tests/sync-crypto/` и проходят в контейнере (15/15), их не удалять.

## Как собрать Windows

```bat
D:\TeleForge\win_build4.bat
```

Скрипт: `VSLANG=1033` (убирает «Примечание: включение файла»), `ninja -C
out-ninja -j 3 Telegram`, лог `D:\TeleForge\win_build4.log`.

Важные ловушки:

1. **После каждой переконфигурации CMake** (любая правка `CMakeLists.txt`)
   нужно выполнить:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File D:\TeleForge\tf_fix_rc_sccache.ps1
```

Иначе сборка падает на `Telegram.rc.res`: `sccache: error: failed to execute
compile … cmcldeps`.

2. **Приложение должно быть закрыто** во время линковки, иначе
   `LINK : fatal error LNK1104: не удается открыть файл "bin\TeleForge.exe"`.

3. **Линковка требует много ОЗУ:** при занятой памяти `LNK1102: недостаточно
   памяти`. Перед сборкой остановить WSL (`wsl --shutdown`) — виртуальная машина
   держит 4–6 ГБ. Пользователь просил собирать в 4–5 потоков.

4. Линковка статического бинаря 260 МБ занимает 20–25 минут, PDB ~4.3 ГБ.

## Как собрать Linux (проверка переносимости)

```bash
docker kill $(docker ps -q)
docker run --rm --cpus=4 -u $(id -u) -e HOME=/tmp \
  -e CCACHE_SLOPPINESS=pch_defines,time_macros \
  -v ~/tf:/usr/src/teleforge -v ~/tf/.ccache:/ccache -e CCACHE_DIR=/ccache \
  -w /usr/src/teleforge/out teleforge/centos_env:full \
  bash -c "ninja -j 3 Telegram > /usr/src/teleforge/build.log 2>&1; echo EXIT=\$?"
```

Дерево синхронизируется копированием (`rsync` в WSL нет):

```powershell
Copy-Item 'D:\TeleForge\tdesktop\Telegram\SourceFiles\...' '\\wsl.localhost\Debian\home\hltv\tf\Telegram\SourceFiles\...' -Force
```

Изолированная проверка одного файла (быстро, без полной сборки): скрипты
`~/tf/one*.sh` в WSL — они парсят `build.ninja` и вызывают
`g++ -fsyntax-only` с точными `DEFINES/INCLUDES/FLAGS` в контейнере.

Ограничение памяти: если в WSL `free -m` показывает меньше 1500 МиБ
available — сборка падает по OOM. Держать 2–3 потока.

## Инструменты, которые уже написаны

| Файл | Назначение |
|---|---|
`D:\TeleForge\tools\dumpinfo.py` | читает minidump: адрес исключения и модуль |
`D:\TeleForge\tools\stackwalk.py` | проходит стек упавшего потока и сопоставляет с `.map` |
`D:\TeleForge\tools\resolve-offset.ps1` | пытается резолвить адрес через PDB (dbghelp) |
`D:\TeleForge\tf_fix_rc_sccache.ps1` | убирает sccache с RC-правила |
`tests/sync-crypto/run.sh` | тест крипты архива (15 проверок) |

Пример: `python3 /mnt/d/TeleForge/tools/stackwalk.py <dump> <TeleForge.map> <tid>`
— так был найден кадр `Lang::ParseKeyValue` в дампе от 15:12.

## Где логи, дампы и карты

```
C:\Users\lavre\AppData\Roaming\TeleForge\log.txt          лог установленного профиля
D:\TeleForge\tdesktop\out-ninja\bin\log.txt               лог запусков из bin
D:\TeleForge\tdesktop\out-ninja\bin\log_start0..9.txt     пустые, мешают создавать новый лог
C:\Users\lavre\AppData\Local\CrashDumps\TeleForge.exe.*.dmp   дампы падений
D:\TeleForge\tdesktop\out-ninja\bin\TeleForge.map         карта символов (2.2 ГБ)
D:\TeleForge\win_build4.log                               лог последней сборки
```

## План для чистого агента (предлагаемый порядок)

1. **Собрать без `/DEBUG`** — убрать из `LINK_FLAGS` (edit `build.ninja`
   напрямую или через CMake) `/DEBUG`, и добавить `/MAP`. Линковка станет
   втрое быстрее, PDB не будет, `.map` останется, память перестанет упираться.
2. **Поймать падение** на пустом профиле и снять дамп, затем прочитать стек
   `tools/stackwalk.py`. Адреса в `.map` и в exe будут от одной сборки —
   сопоставление честное.
3. **По стеку найти функцию** и править точечно. Не перебирать гипотезы: каждая
   сборка стоит 20–25 минут.
4. **Отдельно починить язык:** перенести новые ключи в конец
   `Telegram/Resources/langs/lang.strings` (не в середину), чтобы не сдвигать
   индексы. Плюс, при желании, добавить в `Lang::ValueParser` защиту от
   `Unexpected tag` (не падать, а логировать и подставлять пустую строку).
5. **Только после стабильного запуска** возвращать функциональность: пароль
   архива (UI + `EncryptBundleEx` в писателе) и Python-плагины.

## Что нельзя делать

- Не убивать `Telegram.lib`/объектники: полная пересборка — 1.5–2 часа.
- Не запускать `ninja` с числом потоков больше 5 при 16 ГБ ОЗУ: линковщик
  падает с `LNK1102`.
- Не запускать сборку, пока приложение открыто (файл занят).
- Не коммитить сабмодули (`lib_ui`, `zxcvbn`, `cmake`).
- Не трогать `tdata` пользователя без копии: `D:\TeleForge\tdesktop\out-ninja\bin\tdata`
  содержит архив 32.5 МБ и ключи шести аккаунтов.
