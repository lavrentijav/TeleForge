# TeleForge

![TeleForge Logo](Telegram/Resources/art/logo_256.png)

[ English | [Русский](README-RU.md) ]

## Overview

**TeleForge** is an unofficial Telegram client, inspired by AyuGram but enhanced with unique features.

The project is based on Telegram Desktop and keeps the familiar desktop experience while extending it with additional customization, utility, privacy-oriented behavior, and experimental client features that are not part of the official app.

## Project Direction

TeleForge is built as a separate client identity, not as a simple rebrand. The current direction of the project includes:

- deeper customization of interface and behavior
- privacy-oriented tooling such as anti-delete and message history features
- per-chat automation and policy controls
- synchronization of client-side personality and assistant state
- AI and local inference integrations such as LM Studio
- experimental interaction features that go beyond stock Telegram Desktop

## Core Features

- Ghost Mode
- Anti-delete and message history tooling
- Extended appearance and font customization
- Streamer Mode
- Local Telegram Premium options
- Translator
- Advanced UI customization
- Experimental AI and automation integrations

## Official Resources

- Channel: [t.me/teleforge_official](https://t.me/teleforge_official)
- Chat: [t.me/teleforgechat](https://t.me/teleforgechat)
- Website: [tele-forge.ru](https://tele-forge.ru)

## Logos And Branding

TeleForge uses its own branding assets stored in the repository.

- Main project logo: [Telegram/Resources/art/logo_256.png](Telegram/Resources/art/logo_256.png)
- Primary application icons: [Telegram/Resources/art/icon128.png](Telegram/Resources/art/icon128.png), [Telegram/Resources/art/icon256.png](Telegram/Resources/art/icon256.png), [Telegram/Resources/art/icon512.png](Telegram/Resources/art/icon512.png)
- Windows icon bundle: [Telegram/Resources/art/icon256.ico](Telegram/Resources/art/icon256.ico)
- Alternative logo variant without margins: [Telegram/Resources/art/logo_256_no_margin.png](Telegram/Resources/art/logo_256_no_margin.png)

These assets define the current visual identity of the client across the app window, installer resources, repository presentation, and packaged builds.

## Preview

<h3>
  <details>
    <summary>Screenshots</summary>
    <table>
      <tr>
        <td><img src='.github/demos/demo1.png' width='268' alt='Preferences'></td>
        <td><img src='.github/demos/demo2.png' width='268' alt='Options'></td>
        <td><img src='.github/demos/demo3.png' width='268' alt='Filters'></td>
      </tr>
      <tr>
        <td><img src='.github/demos/demo4.png' width='268' alt='Appearance'></td>
        <td><img src='.github/demos/demo5.png' width='268' alt='Chats'></td>
      </tr>
    </table>
  </details>
</h3>

## Downloads

Prebuilt binaries may be distributed through GitHub Releases, GitHub Actions artifacts, or official TeleForge resources when available.

If you need a local build, use the build documentation from this repository:

- Windows x64: [docs/building-win-x64.md](docs/building-win-x64.md)
- Windows ARM: [docs/building-win-arm.md](docs/building-win-arm.md)
- Linux: [docs/building-linux.md](docs/building-linux.md)
- macOS: [docs/building-mac.md](docs/building-mac.md)

## Windows Build Notes

Make sure your Visual Studio Build Tools installation includes:

- C++ MFC latest for x86 and x64
- C++ ATL latest for x86 and x64
- recent Windows 11 SDK

## Credits

### Base clients and inspirations

- [Telegram Desktop](https://github.com/telegramdesktop/tdesktop)
- [AyuGram](https://github.com/AyuGram/AyuGramDesktop)
- [Kotatogram](https://github.com/kotatogram/kotatogram-desktop)
- [64Gram](https://github.com/TDesktop-x64/tdesktop)
- [Forkgram](https://github.com/forkgram/tdesktop)

### Libraries

- [JSON for Modern C++](https://github.com/nlohmann/json)
- [SQLite](https://github.com/sqlite/sqlite)
- [sqlite_orm](https://github.com/fnc12/sqlite_orm)
- [androidx sources](https://github.com/androidx/androidx)

### Icons

- [Solar Icon Set](https://www.figma.com/community/file/1166831539721848736)

### Utilities

- [TelegramDB](https://t.me/tgdatabase) for username lookup by ID
