# TeleForge

![Логотип TeleForge](Telegram/Resources/art/logo_256.png)

[ [English](README.md) | Русский ]

## Общее описание

**TeleForge** это неофициальный Telegram-клиент, вдохновлённый AyuGram, но расширенный собственными уникальными возможностями.

Проект основан на Telegram Desktop и сохраняет привычный десктопный UX, одновременно добавляя больше кастомизации, утилитарных возможностей, privacy-oriented поведения и экспериментальных функций, которых нет в официальном клиенте.

## Направление проекта

TeleForge развивается как отдельный клиент, а не как простой ребрендинг. Текущий вектор проекта включает:

- глубокую настройку интерфейса и поведения клиента
- privacy-инструменты, включая anti-delete и историю сообщений
- per-chat политики и управление доступом к функциям
- синхронизацию personality и локального состояния ассистента
- AI и локальные inference-интеграции, включая LM Studio
- экспериментальные возможности поверх стандартного Telegram Desktop

## Основные возможности

- Ghost Mode
- Anti-delete и история сообщений
- Расширенная настройка внешнего вида и шрифтов
- Streamer Mode
- Локальные опции Telegram Premium
- Переводчик
- Расширенная кастомизация интерфейса
- Экспериментальные AI и automation-возможности

## Официальные ресурсы

- Репозиторий: [github.com/lavrentijav/TeleForge](https://github.com/lavrentijav/TeleForge)
- Канал: [t.me/teleforge_official](https://t.me/teleforge_official)
- Чат: [t.me/teleforgechat](https://t.me/teleforgechat)
- Сайт: [tele-forge.ru](https://tele-forge.ru)

## Логотипы и айдентика

TeleForge использует собственные branding assets, которые лежат прямо в репозитории.

- Основной логотип проекта: [Telegram/Resources/art/logo_256.png](Telegram/Resources/art/logo_256.png)
- Основные иконки приложения: [Telegram/Resources/art/icon128.png](Telegram/Resources/art/icon128.png), [Telegram/Resources/art/icon256.png](Telegram/Resources/art/icon256.png), [Telegram/Resources/art/icon512.png](Telegram/Resources/art/icon512.png)
- Windows icon bundle: [Telegram/Resources/art/icon256.ico](Telegram/Resources/art/icon256.ico)
- Вариант логотипа без полей: [Telegram/Resources/art/logo_256_no_margin.png](Telegram/Resources/art/logo_256_no_margin.png)

Эти файлы формируют текущую визуальную идентичность клиента в окне приложения, ресурсах установщика, репозитории и готовых сборках.

## Загрузка и сборка

Готовые бинарники могут распространяться через GitHub Releases, GitHub Actions artifacts или через официальные ресурсы TeleForge, когда они доступны.

Синхронизация с официальным Telegram Desktop (upstream **6.8.x**, в этой ветке **6.6.x**): [docs/UPSTREAM_SYNC.md](docs/UPSTREAM_SYNC.md).

Если нужна локальная сборка, используйте инструкции из этого репозитория:

- Windows x64: [docs/building-win-x64.md](docs/building-win-x64.md)
- Windows ARM: [docs/building-win-arm.md](docs/building-win-arm.md)
- Linux: [docs/building-linux.md](docs/building-linux.md)
- macOS: [docs/building-mac.md](docs/building-mac.md)

## Примечания по сборке Windows

Убедитесь, что в Visual Studio Build Tools установлены:

- C++ MFC latest для x86 и x64
- C++ ATL latest для x86 и x64
- актуальный Windows 11 SDK

## Credits

### Базовые клиенты и источники вдохновения

- [Telegram Desktop](https://github.com/telegramdesktop/tdesktop)
- [AyuGram](https://github.com/AyuGram/AyuGramDesktop)
- [Kotatogram](https://github.com/kotatogram/kotatogram-desktop)
- [64Gram](https://github.com/TDesktop-x64/tdesktop)
- [Forkgram](https://github.com/forkgram/tdesktop)

### Библиотеки

- [JSON for Modern C++](https://github.com/nlohmann/json)
- [SQLite](https://github.com/sqlite/sqlite)
- [sqlite_orm](https://github.com/fnc12/sqlite_orm)
- [androidx sources](https://github.com/androidx/androidx)

### Иконки

- [Solar Icon Set](https://www.figma.com/community/file/1166831539721848736)

### Утилиты

- [TelegramDB](https://t.me/tgdatabase) для поиска username по ID
