# Установка TeleForge в Arch Linux

Три способа, от простого к «своими руками».

## 1. Из AUR (после публикации пакета)

```bash
yay -S teleforge-bin     # готовый бинарь из GitHub Releases
# или
yay -S teleforge-git     # сборка из исходников
```

`teleforge-bin` ставится за секунды, `teleforge-git` собирает всё локально (нужен полный тулчейн и ~30 ГБ на диске, время — часы).

Оба пакета конфликтуют с `telegram-desktop`, потому что предоставляют его же.

## 2. Из архива вручную

```bash
# скачать релиз
wget https://github.com/lavrentijav/TeleForge/releases/download/v6.9.0/TeleForge-6.9.0-x86_64.tar.xz

# распаковать во временный каталог
tar -xf TeleForge-6.9.0-x86_64.tar.xz -C /tmp/teleforge

# поставить файлы в систему
sudo cp -r /tmp/teleforge/usr/* /usr/

# обновить кэши ярлыков и иконок
sudo update-desktop-database /usr/share/applications
sudo gtk-update-icon-cache /usr/share/icons/hicolor 2>/dev/null || true
```

Запуск: `teleforge` или через меню приложений («TeleForge Desktop»).

Удаление — обратные команды по списку из `/tmp/teleforge`:
`/usr/bin/TeleForge`, `/usr/share/applications/com.teleforge.desktop.desktop`,
`/usr/share/icons/hicolor/*/apps/com.teleforge.desktop.png`,
`/usr/share/dbus-1/services/com.teleforge.desktop.service`,
`/usr/share/metainfo/com.teleforge.desktop.metainfo.xml`.

## 3. Из PKGBUILD локально

```bash
git clone https://github.com/lavrentijav/TeleForge.git
cd TeleForge/packaging/aur/teleforge-bin
makepkg -si
```

Полезно, если хочется поправить PKGBUILD под себя (например, убрать зависимости).

## Зависимости

Пакет тянет: `qt6-base`, `qt6-svg`, `qt6-imageformats`, `qt6-wayland`, `glib2`, `openssl`,
`zlib`, `hicolor-icon-theme`, `xdg-utils`, `gcc-libs`.

Для встроенного браузера (мини-приложения, оплата, Instant View) нужен `webkit2gtk-4.1`
из optdepends; без него эти функции не работают, но приложение запускается.

## Если приложение не стартует

```bash
teleforge 2>&1 | head -20        # посмотреть ошибку
ldd $(command -v TeleForge) | grep "not found"   # проверить зависимости
```

Частые причины:

- **`cannot open shared object file`** — не хватает пакета из списка зависимостей;
- **пустой экран при запуске на Wayland** — попробовать `QT_QPA_PLATFORM=xcb teleforge`;
- **нет иконки в трее на Wayland** — нужен пакет `gnome-shell-extension-appindicator`
  или расширение трея для используемого окружения.

## Ускорение и память (необязательно)

Если бинарь тяжёлый и хочется меньше потребления — включите zram, это системная
компрессия памяти, она помогает всему приложению сразу:

```bash
sudo pacman -S zram-generator
```

`/etc/systemd/zram-generator.conf`:

```ini
[zram0]
zram-size = ram / 2
compression-algorithm = zstd
```

После этого — перезагрузка или `sudo systemctl start systemd-zram-setup@zram0.service`.

## Проверка установки

```bash
teleforge &      # запустить
sleep 5
ps -o rss= -p $(pgrep -f TeleForge | head -1) | awk '{print $1/1024 " MiB RSS"}'
```

Ожидаемый ориентир после оптимизаций памяти: 150–250 МиБ, а не 800.
