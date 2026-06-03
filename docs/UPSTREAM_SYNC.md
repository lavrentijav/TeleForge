# Синхронизация с Telegram Desktop (upstream)

Репозиторий: [lavrentijav/TeleForge](https://github.com/lavrentijav/TeleForge)  
Upstream: [telegramdesktop/tdesktop](https://github.com/telegramdesktop/tdesktop) (`upstream/dev`)

## Текущее состояние

| Параметр | Значение |
|----------|----------|
| Версия (`Telegram/build/version`) | **6.8.4** (`6008004`) |
| Merge upstream `dev` | выполнен в `dev` (коммит merge `telegramdesktop/tdesktop` 6.8.4) |
| Брендинг | TeleForge (имя, иконки, `AppFile` = TeleForge) |
| Патчи AyuGram/TeleForge | повторно наложены поверх upstream через `git apply` |

## Что даёт merge upstream

Актуальный API/TL, исправления Telegram Desktop 6.7–6.8, новый UI (боты, админ-лог, фильтры, акцент системы, RHI и т.д.). Без merge клиент остаётся на ветке 6.6.x и может ломаться на новых серверных возможностях.

## Что нужно сохранить в TeleForge

При разрешении конфликтов **не затирайте** без проверки:

- `Telegram/SourceFiles/ayu/` — Ghost Mode, anti-delete, настройки TeleForge
- `Telegram/cmake/teleforge_*`, `qrc/teleforge/`, `art/teleforge/`
- `TeleForgeAssets`, `official_resources.h`, `plugin_catalog.h`
- Патчи в `history/`, `data_session.cpp`, `api_updates.cpp`, `core/application.cpp` (ссылки на `lavrentijav/TeleForge`)
- `Telegram/build/version` — после merge выставить **6.8.4** и прогнать `Telegram/build/set_version.py` при необходимости

## Как выполнить merge (Windows)

1. Закройте **TeleForge.exe**, сборку Ninja/MSBuild и лишние процессы, держащие файлы в `Telegram/SourceFiles/`.
2. Сохраните незакоммиченные правки: `git stash push -u -m "before upstream"`
3. Уберите мешающие неотслеживаемые файлы от сорванного merge: `git clean -fd` (только если понимаете, что удаляете временный мусор).
4. Обновите upstream: `git fetch upstream dev`
5. Merge: `git merge upstream/dev`
6. Разрешите конфликты (часто: `lang.strings`, `CMakeLists.txt`, файлы с `// AyuGram` / `// TeleForge`).
7. Обновите подмодули: `git submodule update --init --recursive`
8. `git stash pop` и снова разрешите конфликты с вашими WIP.
9. Полная пересборка: `teleforge_ninja_build.bat` или скрипт из `docs/building-win-x64.md`.

Если merge падает с `unable to unlink old ... Invalid argument` — файл занят (IDE, антивирус, запущенный exe). Повторите после закрытия процессов.

## Ссылки в проекте

| Назначение | URL |
|------------|-----|
| GitHub / Releases | https://github.com/lavrentijav/TeleForge/releases |
| GitHub Pages (каталог плагинов) | https://lavrentijav.github.io/TeleForge/ |
| Сайт | https://tele-forge.ru |

Старые URL `TeleForgeDesktop/TeleForge` и `teleforgedesktop.github.io` в коде заменены на актуальные.

## Ветка `origin/codex/integrate-origin-dev-2026-04-01`

Содержит в основном обновление подмодулей для CI, **не** полный merge 6.8.4. Основная работа — merge `upstream/dev` в `dev` по инструкции выше.
