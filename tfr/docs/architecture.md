# TeleForge Agent — архитектура

Статус: дизайн зафиксирован; шаг 1a реализован — пассивный журнал, примитивы
`crypto/**`, хранилище секретов и `--self-test` (см. `docs/migration.md` и
`docs/passive-module.md`). Транспорт, SSH и TDLib — по-прежнему заглушки.
Область: новый серверный компонент `rust/teleforge-agent`.
Дата: 2026-02-24.
Что прочитано перед написанием: `ayu/features/sync/**` (все 10 пар `.cpp/.h`),
`ayu/features/teleforge/teleforge_storage.*`, `teleforge_core.*`,
`teleforge_unified_db.h`, `tf_peer_archive_entities.h`, `ayu/features/spy/**`,
`docs/audits/passive-sync-audit.md`, `docs/PLUGIN_TRUST.md`,
`.github/workflows/teleforge_release.yml`.
В существующем коде ничего не менялось.

---

## 1. Задача и границы

### 1.1 Что должен делать агент

1. Жить на сервере пользователя (VPS) и принимать доступ от десктопного клиента
   одним из способов: логин/пароль пользователя сервера, `root`, имя хоста из
   `~/.ssh/config`, путь к приватному ключу, либо явные параметры SSH-туннеля.
2. Держать SSH-туннель до БД (PostgreSQL/MySQL) — сегодня это
   `TeleForge::Ssh::EnsureTunnelBlocking` из `teleforge_ssh_tunnel.cpp`.
3. Хранить сессии Telegram (TDLib/MTProto auth keys) **зашифрованными**.
4. Быть серверной стороной синхронизации того, что сейчас делает
   `ayu/features/sync/**`: снапшоты, строки, мерж.
5. Распространяться как бинарник из ветки `agent-bin` и GitHub Releases, с
   проверкой подписи и версии, которое клиент **сам заливает на сервер**.

### 1.2 Чего агент не делает

* Не открывает порт наружу. Ни при каких настройках «чтобы клиент достучался
  напрямую». Единственный путь — SSH-канал, который клиент и так имеет.
* Не хранит пароль пользователя Telegram. Вход в аккаунт делает клиент (или
  TDLib по QR/коду), агент получает готовую зашифрованную сессию.
* Не является облаком. Он на машине пользователя. «Сервер» в аудите — это
  PostgreSQL/MySQL/Telegram-канал, и именно оттуда уезжали секреты (C1, C2).
* Не заменяет `lib_storage` и `tdata` клиента. Он не читает `tdata`.

### 1.3 Главный принцип дизайна

> Всё, что сегодня выводится из публично известных значений, должно выводиться из
> секрета, который вводит человек. Всё, что сегодня пишется «как получится»,
> должно писаться одной функцией с одним набором прав.

Практическое следствие: агент — это первое место в проекте, где появляется
**настоящая граница доверия**: пароль/ключ, который знает только пользователь, и
файл на диске, который этим паролем закрыт.

---

## 2. Что делает текущий код и почему это не переносится

Перенос «как есть» невозможен не из-за языка, а из-за четырёх свойств текущей
схемы. Ниже — по файлам.

### 2.1 Ключ шифрования

`teleforge_sync.cpp`:

```cpp
[[nodiscard]] QByteArray MasterMaterial(not_null<Main::Session*> session) {
    return QByteArray::number(session->user()->id.value)
        + session->user()->phone().toUtf8();
}
[[nodiscard]] QByteArray SyncKey(not_null<Main::Session*> session) {
    return DeriveSyncKey(MasterMaterial(session), g_exportKey);
}
```

`teleforge_sync_crypto.cpp`:

```cpp
[[nodiscard]] QByteArray HkdfSha256(ikm, salt, info) {
    auto data = salt + ikm + info;
    return QCryptographicHash::hash(data, QCryptographicHash::Sha256);
}
```

`HkdfSha256` — это один `SHA-256` над конкатенацией. Ни extract, ни expand, ни
итераций, ни соли, которую нельзя вычислить. `g_exportKey` пуст, потому что
`setExportKey` не имеет ни одного вызова в дереве. Итог: `SyncKey` =
`SHA-256("TeleForge-Sync-v1" + userId + phone + "account")`.

Тот же материал (`SHA-256(userId:phone)`) даёт `PgTenantId` в
`teleforge_sync_pg.cpp`, и он же используется как ключ для `HmacId` в
`BuildRow` (`teleforge_sync_rows.cpp`). То есть **один и тот же публично
выводимый ключ** и шифрует данные, и «слепит» идентификаторы.

### 2.2 Что уезжает в облако

`BuildEncryptedDatabaseShards` (`teleforge_sync_merger.cpp`) делает `VACUUM INTO`
всей `data.tforge`, `CompressSnapshot`, `EncryptSnapshot` и заливает документом в
канал через `UploadEncryptedDocument`. В снимке — `PersonalityCore`
(`teleforge_unified_db.h`), а в нём `openAiApiKey`, `pgConnString` с паролем БД,
`sshTunnelTarget`, `sshTunnelIdentityFile`. Плюс `SerializePersonalityCore` пишет
`openai_api_key=…` в файл по **относительному** пути
`./tdata/personality_core.tforge`.

### 2.3 Как решаются конфликты

`MergeDataDb` (`teleforge_sync_merger.cpp`): `INSERT OR REPLACE … WHERE
r.updatedAt >= l.updatedAt`, включая `MemoryItem` по `id`, где `id` —
автоинкремент **на устройстве**. Плюс `>=` отдаёт победу удалённой стороне при
равных секундах. Это H7.

### 2.4 Транзакции и потоки

`addEditedMessage`/`addDeletedMessage` в `ayu/data/ayu_database.cpp` делают
`begin_transaction` → `insert` → `commit` и в `catch` только `LOG`, без
`rollback` (C4). Единственное SQLite-соединение читается из `crl::async` через
`LoadPersonalityCore` внутри `ApplyTunnelIfConfigured` (C5).

### 2.5 Вывод

Переносится **функциональность**, а не реализация. Агент берёт:

* идею локального-как-источника-истины;
* набор сущностей (сообщения/правки, присутствие, архив пользователей, память,
  настройки);
* семантику `tunnel.up` (`RewriteConnStringHostPort`, `ExtractConnHostPort`).

и не берёт: вывод ключа, снимок всей БД как единицу репликации, мерж по
`updatedAt`/`id`, отсутствие `rollback`.

---

## 3. Модель развёртывания: клиент ставит агента сам

Это ключевое отличие от «серверного демона, который админ ставит руками».
Пользователь вводит доступ в десктопном клиенте, и дальше всё делает клиент.

### 3.1 Последовательность установки

```
[Клиент]                                            [VPS]
   │ 1. пользователь ввёл: user/password | root | ssh-config alias |
   │    путь к ключу | host/port/user/identity
   │
   ├─ 2. SSH-соединение ─────────────────────────────► sshd
   │
   ├─ 3. определить цель:
   │      uname -s                  → linux | darwin
   │      uname -m                  → x86_64 | aarch64 | arm64 | amd64
   │      getconf GNU_LIBC_VERSION  → есть: gnu, нет: musl
   │      id -u                     → 0: root, иначе имя пользователя
   │
   ├─ 4. GET /agent-bin/channels/stable/manifest.json (+ .sig)
   │    проверить подпись Ed25519 корневым ключом (вшит в клиент)
   │    select_artifact(triple, libc) → нужный артефакт
   │
   ├─ 5. скачать артефакт из Releases (у клиента есть интернет)
   │    sha256 == manifest, затем проверить подпись артефакта
   │
   ├─ 6. залить на сервер ───────────────────────────► ~/.local/lib/teleforge-agent/
   │      основной путь: SFTP put                        releases/<version>/teleforge-agent
   │      fallback A:   ssh 'cat > path'   (raw stream)
   │      fallback B:   ssh 'base64 -d > path'
   │    затем `sha256sum` на сервере и сверка с манифестом
   │
   ├─ 7. создать каталоги (0700) и agent.toml (0640)
   │
   ├─ 8. запустить `teleforge-agent self-test` ───────► exit 0 + JSON на stdout
   │      (exit 7 = SELF_TEST_FAILED, клиент не продолжает)
   │
   ├─ 9. поставить unit:
   │      не-root: ~/.config/systemd/user/teleforge-agent.service
   │               + loginctl enable-linger <user>
   │      root (явный выбор): /etc/systemd/system/teleforge-agent.service
   │      нет systemd: nohup-обёртка + запись в crontab @reboot
   │
   ├─ 10. systemctl --user enable --now teleforge-agent
   │      (или sudo systemctl … если выбран root)
   │
   ├─ 11. bootstrap по SSH-каналу:
   │      pair(label) → одноразовый код
   │      init(tenant, passphrase) → tenant_id, device_token,
   │                                recovery_code (показать один раз),
   │                                listener_fingerprint, high_water=0
   │
   └─ 12. status → healthcheck → отобразить в UI «агент 0.1.0, healthy»
```

### 3.2 Что клиент передаёт при первом bootstrap

| Параметр | Куда | Права | Почему так |
|---|---|---|---|
| passphrase | в `init`/`unlock` по каналу, `--passphrase-fd` если локально | не пишется | это единственный секрет, который не должен попасть на диск сервера |
| параметры туннеля к БД (host, port, user, identity file) | `agent.toml` | 0640 | не секрет; identity file остаётся там, где пользователь его положил |
| реквизиты БД (user/password) | **в vault**, не в `agent.toml` | 0600 | прямое закрытие M14: `pgConnString` больше не хранится в БД вместе с архивом |
| `known_hosts` / fingerprint хоста БД | `~/.ssh/known_hosts` агента | 0600 | закрывает `StrictHostKeyChecking=accept-new` (M12) |
| стоимость KDF (m_cost/t_cost) | `agent.toml` | 0640 | оператор вправе выбрать под свой VPS |

### 3.3 Что агент генерирует сам и возвращает клиенту

| Сгенерировано | Где хранится | Что возвращается клиенту |
|---|---|---|
| `tenant_id` — 16 случайных байт | `tenants/<hex>/meta.json` | hex, для адресации |
| соль Argon2id — 32 случайных байта | там же (cleartext, это не секрет) | не возвращается |
| DEK — 32 случайных байта | только в памяти + `keys/dek.<id>.wrap` | не возвращается **никогда** |
| recovery code — 24 случайных байта | только `keys/reco.<id>.wrap` (обёртка) | один раз, в UI «сохраните» |
| device token — 32 случайных байта | `auth/clients.json` хранит `sha256(token)` | один раз, в OS keychain клиента |
| ключ агентского CA (Ed25519) | `identity/ca.key.pem` 0600 | публичный сертификат CA |
| сертификат listener'а | `identity/agent.cert.pem` | fingerprint для пиннинга |

**Клиент не выбирает ни один ключ.** Это и есть содержательная разница с
`MasterMaterial()`: там ключ вычислял клиент из публичных данных, здесь ключ
рождается на сервере из `OsRng` и оборачивается паролем.

### 3.4 Права: что именно требует root

Агент **не требует root**. Базовый режим — выделенный непривилегированный
пользователь.

| Действие | root нужен? | Почему |
|---|---|---|
| создать пользователя `teleforge` | да, один раз | `useradd -r` |
| поставить **user**-unit | нет | `~/.config/systemd/user/` принадлежит пользователю |
| `loginctl enable-linger` | да (или polkit-подтверждение) | иначе user-сервис умрёт после закрытия сессии |
| поставить **system**-unit | да | `/etc/systemd/system/` |
| `ss -L` на порт ≥ 1024 | нет | агент слушает 8137 или unix-socket |
| `mlock` до 16 МиБ | нет | `LimitMEMLOCK=` в unit поднимает лимит для своего же процесса |
| прочитать свой identity file | нет | файл принадлежит тому же пользователю |
| `sudo` для установки | только если пользователь сам выбрал «работать под root» | это опция, не необходимость |

Рекомендация по умолчанию, которую клиент показывает в UI:

```
[✓] Создать отдельного пользователя teleforge (рекомендуется)
    требует sudo один раз: useradd + loginctl enable-linger
[ ] Установить как system-сервис под root
    не рекомендуется: агент получит доступ ко всей машине
```

Если у пользователя вообще нет sudo, клиент ставит агента под его собственным
аккаунтом и пишет в UI, что изоляция слабее (агент видит файлы этого
пользователя).

### 3.5 Идемпотентность установки и атомарная подмена

Раскладка (общая для агента и клиента, см. `updater::InstallPlan`):

```
~/.local/lib/teleforge-agent/            или /var/lib/teleforge-agent
  updater/releases/0.1.0/teleforge-agent    0700
  updater/releases/0.2.0/teleforge-agent    0700
  updater/current  -> releases/0.2.0
  updater/previous -> releases/0.1.0
  updater/state.json                        last_generation, current, previous
```

Правила:

1. **Повторный деплой той же версии — no-op.** Клиент сравнивает
   `manifest.version` с версией из `status` и `generation` из `state.json`.
   Совпало и healthy → выходит, ничего не трогая.
2. **Никогда не перезаписывается работающий файл.** Новая версия кладётся в
   собственный `releases/<version>/`, проверяется `sha256`, проходит
   `--self-test`, и только потом `ln -sfn releases/<v> current`. `rename` поверх
   симлинка атомарен: либо старый `current`, либо новый.
3. **Предыдущий слот сохраняется** до успешного healthcheck нового. Это то, чего
   нет в текущем апдейтере tdesktop и что делает «новая версия не стартует»
   двухсекундной проблемой.
4. **Откат** = `ln -sfn $(readlink previous) current` + restart + проверка
   `status`. Шаги возвращаются данными (`InstallPlan::rollback_steps`), чтобы UI
   показывал, на каком именно шаге всё встало.
5. **`systemctl restart`, а не `start`.** `start` идемпотентен, но не перечитывает
   `current`-симлинк, если unit ссылается на путь.

### 3.6 Установка без права записи (fallback'и)

| Ограничение сервера | Что делает клиент |
|---|---|
| нет SFTP, есть shell | `ssh 'cat > ~/.local/lib/.../agent'` — raw-поток, бинарник передаётся как есть |
| shell без `cat` (rbash, busybox без cat) | `ssh 'base64 -d > path'`, поток base64 |
| нет `sha256sum` | `sha256sum`, затем `openssl dgst -sha256`, затем `busybox sha256sum`; если ничего нет — считать `wc -c` + `cmp` на клиенте после повторного `cat` |
| нет `systemctl --user` | `nohup` + `@reboot` в `crontab`; в UI прямо сказать, что автозапуск слабее |
| нет `/run` для сокета | сокет в `$XDG_RUNTIME_DIR` или в `~/.teleforge-agent/run` |
| только `sudo` без `systemctl` | `sudo -n` проверяется заранее; если недоступно — установка не начинается, а не падает на середине |

Все три варианта передачи бинарника пишут в **временный файл** рядом с целевым и
делают `mv` (атомарно в пределах ФС), поэтому оборванный `scp` не оставляет
полуфайл на месте рабочего бинарника.

### 3.7 Unit-файл (генерируется клиентом)

```ini
[Unit]
Description=TeleForge Agent
After=network-online.target
Wants=network-online.target
StartLimitIntervalSec=300
StartLimitBurst=5

[Service]
Type=simple
# 127.0.0.1 + unix-socket: наружу ничего не открыто
ExecStartPre=%h/.local/lib/teleforge-agent/updater/current/teleforge-agent self-test
ExecStart=%h/.local/lib/teleforge-agent/updater/current/teleforge-agent serve --foreground
Restart=on-failure
RestartSec=5s
TimeoutStopSec=20

# --- изоляция ---
NoNewPrivileges=yes
PrivateTmp=yes
PrivateDevices=yes
ProtectSystem=strict
ProtectHome=read-only
ProtectKernelTunables=yes
ProtectKernelModules=yes
ProtectControlGroups=yes
ProtectClock=yes
ProtectHostname=yes
RestrictNamespaces=yes
RestrictRealtime=yes
RestrictSUIDSGID=yes
LockPersonality=yes
MemoryDenyWriteExecute=no
SystemCallArchitectures=native
SystemCallFilter=@system-service
RestrictAddressFamilies=AF_UNIX AF_INET AF_INET6
CapabilityBoundingSet=
AmbientCapabilities=
KeyringMode=private
RemoveIPC=yes
UMask=0077

# --- секреты в памяти ---
LimitCORE=0
LimitMEMLOCK=16M

# --- состояние ---
StateDirectory=teleforge-agent
StateDirectoryMode=0700
RuntimeDirectory=teleforge-agent
RuntimeDirectoryMode=0700
ReadWritePaths=%h/.local/lib/teleforge-agent

[Install]
WantedBy=default.target
```

Сниппет выше — **непривилегированный** вариант: `~/.config/systemd/user/`,
пути в `%h/.local/lib/teleforge-agent`, `WantedBy=default.target`. Для
system-режима (только если пользователь явно выбрал «под root») клиент меняет
четыре вещи: `StateDirectory` остаётся, но `data_dir = /var/lib/teleforge-agent`;
`ExecStart` — `/opt/teleforge-agent/updater/current/teleforge-agent`; `User=`/
`Group=teleforge` добавляются явно; `WantedBy=multi-user.target`. Всё остальное
(хардненинг, `LimitCORE`, `LimitMEMLOCK`) идентично — изоляция не должна зависеть
от того, есть ли у пользователя sudo.

Два неочевидных пункта:

* `LimitCORE=0` + `PR_SET_DUMPABLE=0` в коде — иначе ключ в памяти попадает в
  core dump, который потом лежит в `/var/lib/systemd/coredump` с правами,
  достаточными для чтения админу;
* `LimitMEMLOCK=16M` — без этого `mlock` падает по дефолтному лимиту 64 КиБ, и
  `secret::lock_pages` вернёт ошибку (которая в `self-test` показывается
  предупреждением, а не фатальной, потому что это деградация, а не отказ).

### 3.8 Каналы и healthcheck

Клиент узнаёт состояние агента тремя способами, все — через тот же SSH-туннель:

| Что | Как | Для чего в UI |
|---|---|---|
| версия | `status.agent_version` | «агент 0.1.0 · обновление до 0.2.0 доступно» |
| протокол | `status.protocol` | несовместимость → «обновите клиент» |
| компоненты | `status.components.{api,vault,tunnel,tdlib,sync,updater}` | каждый со своим бейджем |
| TDLib | `status.tdlib_version` | «требуется ≥ 1.8.0» |
| курсор | `sync.stamp.high_water` | «N операций не синхронизировано» |
| последняя ошибка | `components.<x>.last_error` | текст на карточке компонента |

`status` — единственный вызов, разрешённый при запертом vault (вместе с `hello`,
`pair`, `unlock`, `update.check`). Это значит, что клиент всегда может показать
«агент жив, но заблокирован», а не таймаут.

После установки клиент делает healthcheck по той же логике, что
`updater::healthcheck`:

1. `status` отвечает;
2. `agent_version` == версия из манифеста;
3. `protocol` == ожидаемый;
4. в `components` нет `failed(`.

Любой провал → откат на `previous` и показ ошибки.

### 3.9 Портируемость бинарника

Требование «клиент заливает этот файл на незнакомый сервер» означает, что бинарник
должен запускаться там, где мы никогда не были. Отсюда жёсткие правила.

**Linux — только musl-статика.**

```
x86_64-unknown-linux-musl
aarch64-unknown-linux-musl
```

Почему не glibc:

* glibc-бинарник требует конкретную версию символов (`GLIBC_2.34` и т.п.).
  Собранный на Ubuntu 22.04 на Debian 11 не запустится — а какой у пользователя
  дистрибутив, клиент не знает и не должен спрашивать;
* glibc тянет `libnss_*` динамически через `dlopen` во время DNS-резолва. Это
  значит, что «статическая» glibc-сборка всё равно ломается на резолве в
  контейнере с урезанным `/etc/nsswitch.conf`. `musl` резолвит внутри процесса, и
  это ровно то, что нужно для `tunnel.up` к хосту БД;
* для musl `RUSTFLAGS="-C target-feature=+crt-static"` даёт реально один файл.
  CI это проверяет `file … | grep 'statically linked'` и падает, если нет.

**Никакого glibc-специфичного кода.** Практически это означает:

| Нельзя | Почему | Как в скелете |
|---|---|---|
| `backtrace()`/`backtrace_symbols()` из glibc | символы не разрешатся в musl | паники печатают сообщение и место, без символов |
| `iconv`/`gettext` через glibc | другое поведение/отсутствие | строки — константы, локали не используются |
| `__libc_start_main`-хуки, `LD_PRELOAD`-ожидания | нет | нет |
| `getauxval` для определения возможностей CPU | есть в musl, но не нужно | не используется |
| `dlopen` для TDLib в musl-сборке | musl `dlopen` работает, но `.so` рядом не будет | TDLib статически (§ 11.1) |
| `/proc/self/exe`, `readlink` на себя | обычно есть, но в chroot нет | путь к бинарнику приходит из CLI/unit |

**Внешние зависимости — ноль.** Все библиотеки статически или на `std`:
`rusqlite` с фичей `bundled` (свой SQLite, не системный), `rustls` (не OpenSSL),
`russh` (не `openssl`/`libssh2`), TDLib — статически. Итог: `ldd` на артефакте
печатает «not a dynamic executable», и его можно залить хоть в Alpine, хоть в
distroless.

**aarch64 собирается через `cross`.** `rusqlite bundled` компилирует C, поэтому
нужен настоящий кросс-тулчейн, а не только `rustup target add`. CI использует
`cross` (подготовленный образ), как и остальной CI проекта, который уже собирает
в Docker.

**Windows и macOS — динамическая линковка допустима** (там нет проблемы
«незнакомый дистрибутив»), но и там нет glibc-специфики: код платформенных
отличий изолирован в `fsperm` (POSIX-права vs DACL) и в `tunnel` (пути к
`known_hosts`).

**Размер.** `opt-level="z"`, `lto="thin"`, `panic="abort"`, `strip`. Ожидаемо
6–9 МиБ без TDLib и 20–28 МиБ с ним; отсюда отдельный `-slim` артефакт в
манифесте, чтобы пользователь с собственным TDLib не платил трафиком за
дубликат.

---

## 4. Компоненты

```
                       ┌──────────────────────────────┐
   клиент (tdesktop)   │  SSH (свои креды пользователя)│
        │              └──────────────┬───────────────┘
        │  SFTP/ssh 'cat >'           │  туннель/прокси
        ▼                             ▼
┌───────────────────────────────────────────────────────────────┐
│ teleforge-agent                                               │
│                                                               │
│  ┌──────────┐   ┌───────────┐   ┌──────────┐   ┌───────────┐  │
│  │  CLI     │   │ Supervisor│   │ Updater  │   │ Logger    │  │
│  │ serve/…  │──►│ (состоян.)│──►│ (манифест│   │ (redact)  │  │
│  └──────────┘   └───────────┘   │  стадии) │   └───────────┘  │
│                                 └──────────┘                  │
│  ┌────────────────────────────┐   ┌─────────────────────────┐ │
│  │ API: TLS + unix-socket     │   │ Auth: mTLS + токен      │ │
│  │ framing → dispatch → JSON  │──►│ scopes, replay, limits  │ │
│  └────────────┬───────────────┘   └─────────────────────────┘ │
│               │                                                │
│  ┌────────────▼───────────┐   ┌──────────────────────────────┐ │
│  │ Vault                  │   │ Sync engine                  │ │
│  │ Argon2id → KEK → DEK   │   │ HLC + vector clock           │ │
│  │ meta.json, keys/*.wrap │   │ op_log, entity_field, cursor │ │
│  └────────────┬───────────┘   └──────────────┬───────────────┘ │
│               │                              │                 │
│  ┌────────────▼──────────┐   ┌───────────────▼──────────────┐  │
│  │ TDLib host (1 на акк.)│   │ Tunnel supervisor            │  │
│  │ database_encryption_key│  │ russh / ssh -L               │  │
│  └───────────────────────┘   └──────────────────────────────┘  │
└───────────────────────────────────────────────────────────────┘
                     │                          │
              TDLib (static)            PostgreSQL / MySQL
```

| Компонент | Ответственность | Модуль | Владелец потока |
|---|---|---|---|
| CLI | разбор argv, exit-коды, `self-test` | `cli`, `main` | главный |
| Supervisor | состояния, backoff, healthcheck | `supervisor` | главный |
| Updater | манифест, подпись, стадии, откат | `updater` | главный |
| API | приём соединений, framing, dispatch | `api::*` | accept-цикл |
| Auth | токен, scopes, replay, rate limit | `auth` | accept-цикл |
| Vault | KDF, обёртка DEK, unlock/lock | `vault`, `crypto::*` | главный |
| TDLib host | один `td_json_client` на аккаунт | `tdlib` | свой поток на аккаунт |
| Sync engine | единственный владелец `data.db` | `sync::*` | **один** sync-поток |
| Tunnel | SSH-форвард, readiness | `tunnel` | свой async-контекст |

Правило, которого нет в C++ и которое здесь зафиксировано типом: **`SyncConnection`
не `Sync`**. Один владелец — поток `sync`; API кладёт запрос в очередь. Это
структурный ответ на C5, а не комментарий «только с главного потока».

---

## 5. Модель угроз

| # | Угроза | Кто может | Закрывается |
|---|---|---|---|
| T1 | Кража дампа БД на сервере | админ VPS, бэкап, провайдер | сессии зашифрованы DEK; DEK только под паролем (Argon2id) |
| T2 | Офлайн-перебор пароля | владелец дампа | Argon2id m=64 МиБ, t=3; соль 32 байта случайная |
| T3 | Другой локальный процесс на VPS | сосед по VPS | unix-socket 0660 / loopback + токен + mTLS |
| T4 | Другой SSH-пользователь | сосед | у каждого свой каталог 0700; токен привязан к устройству |
| T5 | MITM на первом подключении к БД | сеть | пиннинг host key туннеля, `StrictHostKeyChecking=yes` |
| T6 | Подмена бинарника при обновлении | тот, кто контролирует GitHub | Ed25519-подпись манифеста + артефакта, корневой ключ офлайн |
| T7 | Откат на старую уязвимую версию | тот, кто контролирует обновления | `generation` монотонно, `assert_not_rollback` |
| T8 | Чтение ключа из core dump / swap | локальный админ | `LimitCORE=0`, `mlock`+`MADV_DONTDUMP`, `PR_SET_DUMPABLE=0` |
| T9 | Утечка через логи | любой, кто читает лог | `logging::redact_line` + отсутствие `Display` у секретов |
| T10 | Чужой агент отвечает на месте нашего | MITM в сети | клиент пиннит fingerprint listener-сертификата при установке |
| T11 | Клиент заливает «свой» бинарник | пользователь сам | это и есть модель: пользователь волен поставить что угодно на свою машину |
| T12 | Компромисс аккаунта GitHub | атакующий | не достаточно: без приватного корневого ключа манифест не подписать |

Отдельно про T11: агент **не** защищается от владельца сервера. Он на его машине.
Защита от владельца — это E2E-шифрование sync-данных (см. § 7.6), и оно есть.

---

## 6. Аутентификация клиента к агенту

### 6.1 Слои

| Слой | Что доказывает | Материал | Где лежит |
|---|---|---|---|
| SSH | кто вообще может открыть соединение | ключ/пароль пользователя | у пользователя, в `~/.ssh` |
| TLS 1.3 | что это тот самый агент | self-signed сертификат агента | агент: `identity/agent.cert.pem`; клиент: пиннинг fingerprint'а |
| mTLS клиента | что это то самое устройство | Ed25519-сертификат, подписанный CA агента | клиент: OS keychain; агент: `identity/clients/<device_id>/cert.pem` 0644 |
| Bearer-токен | что устройство было разблокировано | 32 случайных байта | клиент: OS keychain; агент: `auth/clients.json` с `sha256(token)` 0600 |
| nonce + счётчик | свежесть | память | сбрасывается при переподключении |
| passphrase | что человек присутствует | вводится | нигде не хранится |

### 6.2 Почему и mTLS, и токен

mTLS без токена = «владение файлом». Токен без mTLS = «владение строкой». Оба
крадутся копированием каталога, поэтому это не два фактора в строгом смысле.
Но они ломаются **по-разному**: сертификат бесполезен против другого агента (он
подписан CA этого агента), а токен бесполезен вне TLS-канала, к которому
привязан. Настоящий второй фактор — пароль: `unlock` требует его, и любая
мутирующая операция при запертом vault отклоняется.

### 6.3 Пейринг

```
клиент                          агент
  │  (SSH уже установлен)         │
  │                               │  pair --label "MacBook" (или RPC pair)
  │                               │  → код 32 символа, TTL 600 с,
  │                               │    на диске только sha256(код)
  │  pair {code, device_pubkey}   │
  │ ─────────────────────────────►│  сверка sha256(код) в constant-time
  │                               │  → ClientRecord{device_id, scopes,
  │                               │    token_hash, ssh fingerprint}
  │ ◄─────────────────────────────│  {device_token, client_cert}
  │  сохранить в OS keychain      │
```

Одноразовость: `redeemed_by` заполняется в той же транзакции, что и создание
`ClientRecord`, поэтому повторный `pair` тем же кодом вернёт
`1002 unauthenticated`.

### 6.4 Scopes

`status`, `sync:read`, `sync:write`, `tunnel`, `unlock`, `update`, `admin`.
`admin` даётся только тому устройству, которое выполнило установку. `revoke
--device-id` помечает запись `revoked`, не удаляя: история важна, и удаление
строки позволило бы повторно занять тот же `device_id`.

### 6.5 Хранение токена: SHA-256, а не Argon2id

Намеренно. Argon2id существует, чтобы замедлить **угадывание**. 256-битный
случайный токен не угадывается, и медленный KDF только замедлил бы каждый
запрос. Пароли → Argon2id, токены → SHA-256. Это записано в
`auth.rs`, чтобы позднее никто не «исправил консистентность».

### 6.6 Replay

`ReplayWindow` на соединение: счётчик + окно 1024 значения. Старше окна —
отклоняется без роста памяти. Хранится в памяти и умирает вместе с сессией;
это допустимо, потому что TLS-сессия тоже умирает.

---

## 7. Шифрование сессий

### 7.1 Почему текущая схема непригодна

Разобрано в § 2.1, здесь — коротко и по пунктам, с указанием, что именно ломается:

1. **Нет секрета.** `SHA-256(label + userId + phone)` — все три входа публичны.
   `userId` утекает через `messages.getFullUser`/контакты/экспорт, телефон
   перебираем.
2. **Один SHA-256 вместо KDF.** Одна попытка перебора = один SHA-256 ≈ наносекунды
   на GPU. Ожидаемая стоимость подбора телефона по дампу — часы.
3. **Нет соли.** Один и тот же ключ у всех пользователей с одинаковым
   `userId+phone` (то есть у всех устройств одного аккаунта, навсегда) и
   невозможность радужных таблиц обойти нельзя.
4. **Нет разделения.** Тот же ключ = ключ шифрования = ключ HMAC для
   `chat_id_h`/`message_id_h`/`account_id_h` (`teleforge_sync_crypto.h`,
   `HmacTag`/`HmacId`) = сид `ProjectEmbedding`. Утечка одного ломает всё.
5. **Нет ротации.** Нечего менять: ключ — функция от `userId`, который не
   меняется. `setExportKey` мог бы это исправить, но у него нет вызовов.
6. **«Шифрование» эмбеддингов не шифрование.** `ProjectEmbedding` — знаковая
   перестановка, то есть ортогональное преобразование; в комментарии кода прямо
   сказано, что косинус и L2 сохраняются. Сервер считает близость.
7. **`tenant_id` вычислим.** `PgTenantId` = `SHA-256(userId:phone)`, то есть
   тенант можно подтвердить для любого известного номера.

### 7.2 Новая схема

```
passphrase ──► Argon2id(salt_32, m=64 МиБ, t=3, p=4) ──► KEK (32 B)
                                                          │
DEK (32 B, OsRng, один раз на тенант) ────────────────────┤
                                                          ▼
                        wrap = XChaCha20-Poly1305(KEK, aad = tenant_id‖key_id, DEK)
                        verifier = XChaCha20-Poly1305(KEK, aad = …, "unlock-verifier-v1")
                        reco  = XChaCha20-Poly1305(Argon2id(recovery_code), aad = …, DEK)

subkey(DEK, label) = HKDF-SHA256(ikm=DEK, salt="tf/agent/v1/subkeys", info=label)
  labels: at-rest | tdlib-db | sync-data | blind | auth | verifier | recovery
```

Что это даёт по каждому пункту:

| Проблема | Решение |
|---|---|
| нет секрета | KEK из пароля, которого нет ни на сервере, ни в Telegram |
| один SHA-256 | Argon2id с валидируемым минимумом стоимости (`KdfCost::validate`) |
| нет соли | 32 случайных байта на тенант, хранятся рядом с шифротекстом |
| нет разделения | 7 меток HKDF, у каждой своё назначение |
| нет ротации | `key_id` версионирован; пароль меняется пере-обёрткой 32 байт |
| эмбеддинги | `subkey(DEK, sync-data)` + AEAD целиком, **без** серверного поиска по умолчанию (см. § 10.7) |
| `tenant_id` | 16 случайных байт, не производная от аккаунта |

### 7.3 Почему XChaCha20-Poly1305 по умолчанию, а AES-256-GCM — как чтение

* 192-битный nonce → случайный nonce безопасен неограниченное число раз. Агент
  пере-запечатывает файл сессии на каждом flush TDLib и не имеет надёжного
  счётчика между падениями. Для 96-битного nonce (GCM) пришлось бы вести
  персистентный счётчик на `key_id`, и его потеря = катастрофическая повторная
  генерация nonce.
* AES-256-GCM остаётся: (а) чтобы читать существующие `TFSYNC1`-бандлы
  (`teleforge_sync_crypto.cpp` пишет именно GCM с 12-байтным nonce);
  (б) там, где AES-NI быстрее. Для GCM реализован `NonceCounter`, который
  отказывается выдавать nonce после исчерпания.

Формат конверта (`crypto/aead.rs`, реализован и покрыт тестами):

```
0..8    magic "TFSEC1\0\0" (или "TFSYNC1\0" для чтения старых)
8..10   algorithm id  u16 BE   (1 = XChaCha20-Poly1305, 2 = AES-256-GCM)
10..12  key_id        u16 BE
12..14  nonce_len     u16 BE   (24 или 12; несовпадение с алгоритмом — ошибка)
14..16  reserved      = 0      (иначе ошибка, чтобы поле нельзя было «занять» позже)
16..    nonce
        ciphertext
        tag (16 B)
```

`aad` всегда включает адрес блока: `entity_key(41) ‖ field_id(2) ‖
tombstone(1)` для sync-значений и `tenant_id(16) ‖ key_id(2)` для обёртки DEK.
Благодаря этому перенос шифротекста в другую строку/поле не проходит проверку
тега.

### 7.4 Unlock

```
1. прочитать tenants/<hex>/meta.json  (соль, cost, key_id, aead, verifier)
2. KEK = Argon2id(passphrase, salt, cost)
3. open(KEK, aad, verifier)  → должно дать "teleforge-agent-unlock-verifier-v1"
4. open(KEK, aad, wrapped_dek) → DEK
5. subkey(DEK, …) по требованию; в памяти держим только DEK
6. idle_lock_secs (900 по умолчанию) → автоматический lock
```

Verifier — не хеш KEK, а AEAD-запись известной константы. `verifier =
SHA-256(KEK)` дал бы атакующему одношаговый оракул на каждое угадывание поверх
Argon2id; проверка тега такого оракула не даёт.

Шаги 3 и 4 оба проверяют тег, поэтому «неверный пароль» и «повреждённый файл»
различаются только внутри (для лога), наружу — одинаковый `1002`.

### 7.5 Потеря пароля

| Что потеряно | Что остаётся | Результат |
|---|---|---|
| пароль | recovery code | `unlock_with_recovery` → DEK; сессии и sync-данные целы |
| пароль и recovery code | ничего | сессии Telegram невосстановимы — нужен повторный вход в аккаунт; sync-данные тоже (они под тем же DEK) |
| recovery code, пароль есть | пароль | `recovery-rotate` перевыпускает код |

Recovery code — 24 случайных байта, показывается **один раз** при
`init`/`pair`, с явным чекбоксом «я сохранил». Он оборачивает тот же DEK вторым
ключом, поэтому потеря пароля не означает потерю данных. Обратное тоже верно и
специально спроектировано: **не существует** пути восстановления без одного из
двух, потому что иначе существовал бы путь, которым пользуется атакующий.

Смена пароля — `change_passphrase`: новая соль, новый KEK, `key_id += 1`, новая
обёртка. Данные не пере-шифровываются: DEK не менялся. Recovery-обёртка требует
перевыпуска (нужен сам код) — это явная ошибка `Unsupported`, а не тихая потеря
кода восстановления.

### 7.6 Граница: что агент может прочитать

Честно и явно, потому что это влияет на выбор пользователя:

| Данные | Агент видит | Почему |
|---|---|---|
| TDLib-сессия | **да** (после unlock) | он её использует; иначе Telegram не работает |
| sync-значения | **нет** | клиент шифрует их сам ключом, производным от пароля пользователя; агент видит только метаданные (адрес, поле, часы) |
| метаданные sync | да: `dialog_id`, `message_id`, `field_id`, HLC, vclock | нужны для упорядочивания; это цена серверного мержа |
| пароль сервера/БД | да (в vault) | нужен для подключения |

То есть компромисс агента = компромисс Telegram-сессии, но **не** компромисс
sync-содержимого. Это максимум, который можно получить, оставив серверный мерж
работающим.

---

## 8. Формат хранения

### 8.1 Дерево

```
<data_dir>                                      0700
├── meta.json                     реестр тенантов (id'шники)  0640
├── auth/
│   └── clients.json              sha256(токенов), scopes     0600
├── identity/                     CA и сертификат агента      0700
│   ├── ca.key.pem                                            0600
│   ├── ca.pub.pem                                            0644
│   ├── agent.cert.pem                                        0644
│   ├── agent.key.pem                                         0600
│   └── clients/<device_id>/cert.pem                          0644
├── tenants/<tenant_hex>/                                      0700
│   ├── meta.json                 соль, cost, key_id, verifier 0640
│   ├── keys/
│   │   ├── dek.0001.wrap         AEAD(KEK, DEK)               0600
│   │   └── reco.0001.wrap        AEAD(recovery KEK, DEK)      0600
│   ├── accounts/<account_hex>/                                0700
│   │   ├── session.db            TDLib, шифрует сам TDLib     0600
│   │   ├── tdlib.json.enc        параметры                   0600
│   │   └── files/                кэш TDLib                    0700
│   ├── sync/
│   │   └── data.db               логический store (WAL)        0600
│   └── logs/                                                  0700
└── updater/
    ├── releases/<version>/teleforge-agent                      0700
    ├── current -> releases/<version>
    ├── previous -> releases/<version>
    └── state.json               generation, current, previous  0600
```

### 8.2 Правила, которые нельзя нарушать

1. **Ничего в `$TMPDIR`.** `DecompressToTemp` в `teleforge_sync_snapshot.cpp`
   писал расшифрованную БД в `QDir::tempPath()` с `setAutoRemove(false)` (L9).
   Агент такой путь не использует; если нужен временный файл, он создаётся
   внутри `tenants/<hex>/` с 0600 и удаляется через `unlink` сразу после открытия
   (файл живёт, пока открыт дескриптор, и уже недоступен по имени).
2. **Все пути абсолютные.** `ExportPersonalityCoreSnapshot` с путём
   `./tdata/personality_core.tforge` зависел от CWD (M14).
3. **Права выставляются до записи содержимого,** а не после: `write_private_file`
   сначала `O_CREAT` с нужным режимом, потом пишет. Иначе есть окно, в котором
   файл с ключом читается всеми.
4. **`O_NOFOLLOW`.** Симлинк, подложенный в каталог ключей, — это подмена ключа.
5. **`fsync` каталога** после `rename` в `write_private_file_replace`, иначе
   переименование может не пережить питание.

### 8.3 Zeroize

Реализовано в `secret.rs`:

* `SecretBytes` не `Clone` — копия вышла бы из-под контроля зануления;
* `Drop` зануляет через `write_volatile` + `compiler_fence` (оптимизатор не имеет
  права выбросить);
* `Debug` печатает `<redacted, N bytes>`, `Display` отсутствует;
* `expose()` — единственный путь наружу, назван так, чтобы `grep` находил все
  места использования;
* `ct_eq` — сравнение без раннего выхода;
* `lock_pages()` — `mlock` + `madvise(MADV_DONTDUMP)`; провал не фатален, но
  виден в `self-test`.

Порядок действий процесса:

```
main:
  setrlimit(RLIMIT_CORE, 0)      # нет core dump
  prctl(PR_SET_DUMPABLE, 0)      # нельзя ptrace/read /proc/<pid>/mem
  madvise по аллокациям секретов # не в swap
```

Чего это не даёт: процесс, который может читать `/proc/<pid>/mem` (то есть
root или тот же uid с `PR_SET_DUMPABLE=1`), всё равно прочитает ключ. Это
записано в документации, а не замалчивается.

---

## 9. Протокол клиент ↔ агент

### 9.1 Стек и почему такой

```
SSH-канал (клиента)  →  TLS 1.3 + mTLS  →  кадр [len u32 BE][JSON]  →  JSON-RPC-подобный конверт
```

* **JSON, не Protobuf в v1.** Клиент уже говорит на JSON в трёх местах: манифесты
  `TFMETA`/`TFLOCK` (`teleforge_sync_transport.cpp`), API плагинов
  (`plugin_runner.cpp`), сериализация `PersonalityCore`. Вводить новый формат в
  первой же версии — это отладка двух новых вещей одновременно.
* **Длина перед кадром, не перевод строки.** `sync.push` бывает на мегабайты, а
  JSON-строки содержат `\n`. Плюс лимит проверяется *до* чтения тела, поэтому
  неаутентифицированный пир не может заставить агент выделить память.
* **Protobuf — v2, и он уже описан** (§ 9.6), потому что схема важнее формата и
  её надо согласовать сейчас.

### 9.2 Правила версионирования

* `protocol` — целое. Агент принимает `MIN_SUPPORTED..=PROTOCOL_VERSION`, вне
  окна — `1001 unsupported_protocol`, чтобы клиент показал «обновите клиент», а
  не «ошибка разбора».
* Добавление поля — не смена версии. Удаление/переименование/смена типа — смена.
* `request_id` возвращается как есть.
* `idempotency_key` обязателен для всех мутирующих методов (проверяется в
  `Envelope::validate`).

### 9.3 Идемпотентность на двух уровнях

| Уровень | Ключ | Что защищает |
|---|---|---|
| запрос | `idempotency_key` | повтор после обрыва связи: агент вернёт сохранённый ответ |
| операция | `op_id` (16 B, от клиента) | даже *другой* запрос, содержащий уже применённую операцию, безопасен |

Почему второго уровня недостаточно одного первого: клиент может разбить пачку
по-разному при повторе, и `idempotency_key` тогда другой, а операции — те же.
`op_log` с `UNIQUE INDEX idx_op_id` делает это свойством хранилища, а не
соглашением.

Именно этого не хватало `teleforge_events` с `PRIMARY KEY (tenant_id,
content_hash, event_type)` + `ON CONFLICT DO NOTHING`: оно глушило не только
повторы, но и **вторую настоящую правку** (H9).

### 9.4 Методы

| Метод | Scope | Можно при запертом vault | Мутирующий |
|---|---|---|---|
| `hello` | status | да | нет |
| `pair` | admin | да | да |
| `unlock` | unlock | да | да |
| `lock` | unlock | да | да |
| `status` | status | да | нет |
| `sync.stamp` | sync:read | нет | нет |
| `sync.pull` | sync:read | нет | нет |
| `sync.push` | sync:write | нет | да |
| `sync.conflicts` | sync:read | нет | да (resolved_by) |
| `tunnel.up` / `tunnel.down` | tunnel | нет | да |
| `tunnel.status` | tunnel | нет | нет |
| `accounts` | sync:write | нет | нет |
| `account.start` / `account.stop` | sync:write | нет | да |
| `update.check` | update | да | нет |
| `shutdown` | admin | нет | да |

Таблица зафиксирована кодом (`Method::allowed_while_locked`,
`Method::is_mutating`, `Method::scope`) с `match` без `_`-ветки — новый метод
обязан принять решение, иначе не скомпилируется.

### 9.5 Примеры

Handshake:

```json
→ {"protocol":1,"request_id":"01J9Z0","idempotency_key":"",
   "method":"hello","params":{"client_version":"0.1.0",
                              "device_id":"a1b2c3d4e5f60718"}}

← {"protocol":1,"request_id":"01J9Z0","ok":true,
   "result":{"agent_version":"0.1.0","protocol":1,
             "min_client_version":"0.1.0","tdlib_version":"1.8.29",
             "vault":"locked","accounts":1,"high_water":1234},
   "error":null}
```

`sync.push` — одна операция из пачки:

```json
→ {"protocol":1,"request_id":"01JA01","idempotency_key":"01JA01",
   "method":"sync.push",
   "params":{"tenant":"5f3c8e...","account":"9a1b7d...",
     "ops":[{
       "op_id":"9f0e1d2c3b4a5968",
       "kind":3,"dialog_id":12345,"message_id":0,"sub":0,
       "field":32,"hlc":"0000018f3c1a2b40-0000","writer_seq":7,
       "tombstone":false,"vclock":"dev-a:7,dev-b:2",
       "value_ct":"<hex AEAD-конверт>"
     }]}}

← {"protocol":1,"request_id":"01JA01","ok":true,
   "result":{"applied":1,"ignored":0,"duplicates":0,"rejected":0,
             "conflicts":0,"high_water":1235},
   "error":null}
```

Конфликт — видимый, а не «кто последний, тот и прав»:

```json
← {"protocol":1,"request_id":"01JB02","ok":true,
   "result":{"applied":0,"ignored":0,"duplicates":0,"rejected":0,
             "conflicts":1,"conflict_ids":[41],"high_water":1240},
   "error":null}
```

Ошибка:

```json
← {"protocol":1,"request_id":"01JB03","ok":false,"result":{},
   "error":{"code":1004,"message":"vault is locked; run `teleforge-agent unlock` first",
            "retryable":false}}
```

Коды (`api::protocol::code`): `1000` invalid_request, `1001`
unsupported_protocol, `1002` unauthenticated, `1003` forbidden, `1004`
vault_locked, `1005` not_found, `1006` conflict, `1007` rate_limited,
`1008` idempotent_replay, `1009` tunnel_failed, `1010` tdlib_failed,
`1011` update_rejected, `1099` internal.

### 9.6 Protobuf-схема (v2, согласована сейчас)

```protobuf
syntax = "proto3";
package teleforge.agent.v1;

message Hlc   { uint64 physical_ms = 1; uint32 logical = 2; }

message EntityKey {
  uint32 kind       = 1;   // 1 message_revision, 2 presence, 3 peer, 4 memory,
                           // 5 setting, 6 per_chat_setting, 7 artifact
  bytes  account_id = 2;   // 16
  sint64 dialog_id  = 3;
  sint32 message_id = 4;
  uint32 sub        = 5;
}

message Op {
  bytes      op_id      = 1;   // 16, idempotency at the op level
  EntityKey  key        = 2;
  uint32     field_id   = 3;   // стабильные номера из FieldId, append-only
  Hlc        hlc        = 4;
  uint64     writer_seq = 5;
  bool       tombstone  = 6;
  bytes      vclock     = 7;   // "dev:seq,dev:seq"
  bytes      value_ct   = 8;   // AEAD-конверт, клиентский ключ
}

message SyncPushRequest  { bytes tenant = 1; bytes account = 2;
                           repeated Op ops = 3; }
message SyncPushResponse { uint64 applied = 1; uint64 ignored = 2;
                           uint64 duplicates = 3; uint64 rejected = 4;
                           uint64 conflicts = 5; repeated int64 conflict_ids = 6;
                           uint64 high_water = 7; }

message StatusResponse {
  string agent_version      = 1;
  uint32 protocol           = 2;
  string tdlib_version      = 3;
  string vault              = 4;   // locked | unlocked
  uint64 high_water         = 5;
  map<string, Component> components = 6;
}
message Component { string state = 1; uint32 restarts = 2;
                    optional string last_error = 3; }
```

Правило совместимости: номера полей не переиспользуются; `reserved` ставится
сразу при удалении поля.

---

## 10. Модель данных для синхронизации

### 10.1 Единица репликации

Не «снимок БД», а **операция над полем сущности**:

```
EntityKey = { kind, account_id[16], dialog_id, message_id, sub }
Op        = { op_id, key, field_id, hlc, writer_seq, vclock, tombstone, value_ct }
```

`account_id` — **владелец аккаунта**, всегда. Это прямое исправление H8, где
`BuildRow` писал в `accountIdH` отправителя:

```cpp
row.accountIdH = HmacId(m.fromId, key);   // teleforge_sync_rows.cpp
```

Здесь такого поля нет вовсе: `account_id` берётся из `EntityKey` и не может быть
подменён отправителем, потому что отправителя в ключе нет.

### 10.2 Таблицы

```sql
CREATE TABLE seq (                     -- монотонный счётчик операций
  name TEXT PRIMARY KEY, next INTEGER NOT NULL) WITHOUT ROWID;

CREATE TABLE entity_root (             -- причинность на сущность
  account_id BLOB NOT NULL, entity_kind INTEGER NOT NULL,
  dialog_id INTEGER NOT NULL, message_id INTEGER NOT NULL, sub INTEGER NOT NULL,
  vclock TEXT NOT NULL, last_hlc BLOB NOT NULL, last_seq INTEGER NOT NULL,
  deleted INTEGER NOT NULL DEFAULT 0, deleted_hlc BLOB,
  PRIMARY KEY (account_id, entity_kind, dialog_id, message_id, sub)) WITHOUT ROWID;

CREATE TABLE entity_field (            -- поле сущности + его версия
  account_id BLOB NOT NULL, entity_kind INTEGER NOT NULL,
  dialog_id INTEGER NOT NULL, message_id INTEGER NOT NULL, sub INTEGER NOT NULL,
  field_id INTEGER NOT NULL, hlc BLOB NOT NULL, writer_seq INTEGER NOT NULL,
  tombstone INTEGER NOT NULL DEFAULT 0, value_ct BLOB NOT NULL,
  op_seq INTEGER NOT NULL,
  PRIMARY KEY (account_id, entity_kind, dialog_id, message_id, sub, field_id)
) WITHOUT ROWID;

CREATE TABLE op_log (                  -- журнал, append-only
  op_seq INTEGER PRIMARY KEY AUTOINCREMENT,
  op_id BLOB NOT NULL, account_id BLOB NOT NULL,
  entity_kind INTEGER NOT NULL, dialog_id INTEGER NOT NULL,
  message_id INTEGER NOT NULL, sub INTEGER NOT NULL,
  field_id INTEGER NOT NULL, hlc BLOB NOT NULL, writer_seq INTEGER NOT NULL,
  tombstone INTEGER NOT NULL DEFAULT 0, value_ct BLOB NOT NULL,
  applied_at INTEGER NOT NULL, verdict INTEGER NOT NULL);
CREATE UNIQUE INDEX idx_op_id ON op_log (op_id);

CREATE TABLE conflict (                -- то, что нельзя решить молча
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  account_id BLOB NOT NULL, entity_kind INTEGER NOT NULL,
  dialog_id INTEGER NOT NULL, message_id INTEGER NOT NULL, sub INTEGER NOT NULL,
  field_id INTEGER NOT NULL, local_hlc BLOB NOT NULL, remote_hlc BLOB NOT NULL,
  detected_at INTEGER NOT NULL, resolved_by TEXT);

CREATE TABLE device_cursor (           -- докуда дошёл каждый девайс
  account_id BLOB NOT NULL, device_id TEXT NOT NULL,
  since_seq INTEGER NOT NULL, applied_ops INTEGER NOT NULL, last_pull_at INTEGER,
  PRIMARY KEY (account_id, device_id)) WITHOUT ROWID;

CREATE TABLE tdlib_cursor (            -- чтобы рестарт не перечитывал всё
  account_id BLOB PRIMARY KEY, chat_id INTEGER, message_id INTEGER,
  updated_at INTEGER NOT NULL);
```

`PRAGMA journal_mode=WAL`, `synchronous=FULL`, `busy_timeout=5000` — на **каждом**
соединении (H6: в C++ `busy_timeout` ставился только в `Storage::initialize`, а
`CreateDatabaseSnapshot`/`MergeDataDb` открывали свои соединения без него).

### 10.3 Два счётчика времени вместо `updatedAt`

**HLC (hybrid logical clock)** — `physical_ms u64 + logical u16`:

* `tick(now)`: если часы ушли вперёд — берём `now`, `logical = 0`; иначе
  физическая часть стоит, `logical += 1`;
* `observe(remote, now)` — двигает наши часы за пределы увиденного.

Что это лечит (три реальных бага из `MergeDataDb`):

| Симптом в C++ | Причина | Как решено |
|---|---|---|
| две правки в одну секунду, одна теряется | `r.updatedAt >= l.updatedAt` при равных секундах | HLC различает их `logical` |
| VPS со спешащими часами выигрывает у всех | сравнение по wall-clock | `tick` монотонен при отставании часов, `observe` впитывает чужое будущее |
| порядок зависит от точности серверных часов | `WeWinElection` сравнивает `message.date` | порядок только по HLC/vclock, серверное время не участвует |

**Vector clock** — `BTreeMap<device_id, seq>`, потому что метка времени не умеет
отличать «B видел правку A и сделал свою» от «B правил одновременно».

```
A = {a:3}, B = {b:1}         → Concurrent  (настоящий конфликт)
A = {a:3}, B = {a:3, b:1}    → A Before B  (B знал про A)
A = {a:3}, B = {a:3}         → Equal       (повтор)
```

### 10.4 Решение конфликтов

```rust
match classify(root, local_field, incoming) {
    Applied            => применить,
    Ignored            => отбросить (локальное доминирует),
    Duplicate          => вернуть сохранённый результат,
    RejectedTombstone  => отбросить (удаление новее),
    Conflict           => применить НИЧЕГО и записать conflict,
}
```

Правила:

* **разные поля** одной сущности при `Concurrent` — мержатся оба (нет конфликта:
  правка `maxMemoryBlockChars` и правка `directoryWhitelistJson` не мешают друг
  другу — в C++ `INSERT OR REPLACE` затирал одну из них);
* **одно поле** при `Concurrent` — обе версии сохраняются, создаётся строка
  `conflict`, клиент показывает пользователю;
* **удаление** — tombstone с `deleted_hlc`; запись с меньшим HLC отклоняется
  (`RejectedTombstone`), поэтому «воскрешение» удалённого сообщения невозможно;
* **наборы** (имена, юзернеймы, био) — `FieldId::is_set_member`, добавление
  элемента не конфликтует с добавлением другого элемента.

`conflict` не разрешается автоматически никогда. Автоматический выбор победителя
— это ровно тот баг, который привёл к H7.

### 10.5 Монотонный курсор

```
seq.next: UPDATE seq SET next = next + 1 WHERE name='op_seq' RETURNING next
```

* выдаётся в той же транзакции, что и вставка поля, поэтому не бывает
  «последовательность выдана, строки нет»;
* не переиспользуется и не пере-нумеруется `VACUUM` (это хранимый счётчик, а не
  rowid — в отличие от `storeSyncArtifact`, где `max(id)+1` переиспользовал id,
  L7);
* клиент передаёт `since_seq`, получает `> since_seq` пачками по
  `PageSpec::limit` (по умолчанию 500, максимум 2000 — запрашивает агент, не
  клиент);
* `since_seq` больше нашего `high_water` → `1000`, а не «пусто навсегда»:
  клиент говорит о другом тенанте или врёт.

Замена `PgFetchRemoteStamp`: вместо `sha256` всего датасета (полный скан, и
непонятно, насколько отстал) — `stamp { high_water, accounts, last_write_ms }`.

### 10.6 Транзакционная дисциплина

Единственный способ писать (`engine::TransactionGuard`):

```
BEGIN IMMEDIATE
  UPDATE seq ... RETURNING next
  INSERT INTO entity_field ...
  INSERT INTO op_log ... (op_id уникален)
  INSERT/UPDATE entity_root
COMMIT
```

`Drop` у guard'а делает `ROLLBACK`, если `commit()` не был вызван. То есть `?`
внутри блока физически не может оставить соединение в открытой транзакции — это
и есть исправление C4 (`addEditedMessage`/`addDeletedMessage` логировали ошибку и
не делали `rollback`, после чего все последующие `begin_transaction` падали с
«cannot start a transaction within a transaction»).

Один поток-владелец (`SyncConnection` не `Sync`) — исправление C5.

### 10.7 Эмбеддинги

В C++ `CollectEmbeddingRows` выгружает векторы в `teleforge_embeddings`
**открытым текстом** (`embedding REAL[]`) с «проекцией» `ProjectEmbedding`,
которая сохраняет косинус. То есть серверный поиск сходства работает у любого,
кто получил дамп.

В агенте по умолчанию: `value_ct = AEAD(subkey(DEK, sync-data), вектор)`. Поиск
— на клиенте. Если пользователю нужен серверный ANN-поиск, это отдельный
включаемый режим с явным предупреждением в UI, и тогда в БД лежит проекция —
но уже на ключе `subkey(DEK, blind)`, а не на ключе шифрования, и параметры
(`dims`, `model_id`) остаются открытыми осознанно, потому что без них индекс
бесполезен. Модель угроз для этого режима записывается в UI, а не в комментарий
кода.

---

## 11. Интеграция TDLib

### 11.1 Линковка

| Способ | Где | Почему |
|---|---|---|
| **static** `libtdjson.a` | linux musl x86_64/aarch64 (основной) | один файл, который клиент заливает по SSH: нет `.so`, нет `LD_LIBRARY_PATH`, нет расхождения версий |
| **dylib** | Windows/macOS, дистрибутивный пакет | там статическая линковка TDLib непрактична |
| **slim** (без TDLib) | отдельный артефакт в манифесте | пользователь с собственным TDLib экономит ~20 МиБ трафика при заливке |

В коде: `#[cfg(feature = "tdlib-link")] mod ffi` с `#[link(name = "tdjson")]`.
Фича выключена по умолчанию, поэтому `cargo check --offline` работает на машине
без TDLib (это проверено: см. § 15).

Используется `td_json_client_*` (`td/telegram/td_json_client.h`):
`td_json_client_create`, `td_json_client_send`, `td_json_client_receive`,
`td_json_client_execute`, `td_json_client_destroy`. Не `td_*`-семейство: оно
мультиплексирует всех клиентов через одну очередь, что делает per-account
shutdown и backpressure трудно доказуемыми.

### 11.2 Один клиент на аккаунт

TDLib привязывает `receive` к потоку, который его вызывает. Поэтому:

```
аккаунт A → поток A → TdlibHost A → канал запросов/ответов
аккаунт B → поток B → TdlibHost B → канал
```

`TdlibHost` не `Clone`; сырой указатель нулируется в `Drop`, чтобы двойной
`destroy` был невозможен. Блокирующий `receive` никогда не вызывается на
accept-цикле: он живёт на своём потоке, а API общается через канал.

### 11.3 Шифрование данных TDLib

`setTdlibParameters` принимает `database_encryption_key`. Агент передаёт туда
`subkey(DEK, LABEL_TDLIB_DB)`, и TDLib сам шифрует `db.sqlite`/`key`/`binlog`.

Ключ существует только между `unlock` и `lock`. Следствие, которое надо
проговорить: **запертый агент не может обслуживать Telegram**. Альтернатива —
ключ на диске, то есть отсутствие шифрования; выбрано первое.

Медиа-кэш (`files/`) шифруется не TDLib, а ФС (LUKS) или не шифруется —
политика явно описана в § 8 и в UI. Скачивать медиа на сервер агент по умолчанию
не просит: sync работает с текстом и метаданными.

### 11.4 Проверка версии

```rust
pub fn check_version(reported: &str) -> Result<()>   // сравнение по числам
```

`(1,10,0) > (1,9,0)` — лексикографическое сравнение дало бы обратное и
отвергло бы годную библиотеку. Это покрыто тестом.
Минимум: `MIN_TDLIB_VERSION = "1.8.0"` — ниже нет одновременно
`database_encryption_key` и нужной семантики `updateMessageSendSucceeded`, на
которой держится sync.

Версия читается через `getOption{"name":"version"}` и попадает в
`status.tdlib_version`.

### 11.5 Что переносится из `ayu/features/spy` и архива

Не код, а схема событий. Соответствие полей:

| Таблица C++ | Поля → `FieldId` |
|---|---|
| `OnlineEvent` | `PresenceKind=20`, `PresenceOnlineTill=21`, `PresenceManualLastSeen=22` |
| `PeerArchiveProfile` | первый/последний визит — в `entity_root`/`FieldId` набора |
| `PeerArchiveName` | `PeerFirstName=30`, `PeerLastName=31` (set members) |
| `PeerArchiveUsername` | `PeerUsername=32` (set member) |
| `PeerArchiveBio` | `PeerBio=33` (set member) |
| `PeerArchiveChatMembership` | `PeerChatTitle=35`, `PeerSelfInChat=36`, `PeerStillInChat=37` |
| `DeletedMessage`/`EditedMessage` | `EntityKind::MessageRevision` + `Text…Flags` (1..18) |
| `MemoryItem` | `EntityKind::MemoryItem`, ключ — `contentHash`, не `id` |
| `MemoryEmbedding` | `MemoryEmbedding=48`, значение — AEAD-конверт |
| `PerChatSettings` | `EntityKind::PerChatSetting`, по одному полю на ключ |
| `PersonalityCore` | **не синхронизируется** в этой модели (см. § 12.3) |

Отдельно: `MemoryItem.id` в C++ переносится между устройствами, и два разных
факта с `id=17` затирают друг друга. Здесь ключ — `contentHash`, а `id` остаётся
локальным и в сеть не уходит вообще.

---

## 12. Распространение и обновление

### 12.1 Где что лежит

```
Ветка agent-bin (force-push, один коммит — чтобы не росла история)
  channels/stable/manifest.json
  channels/stable/manifest.json.sig
  channels/stable/root.pub.hex

GitHub Releases (неизменяемые артефакты)
  teleforge-agent-0.2.0-x86_64-unknown-linux-musl
  teleforge-agent-0.2.0-aarch64-unknown-linux-musl
  teleforge-agent-0.2.0-x86_64-pc-windows-msvc.exe
  teleforge-agent-0.2.0-aarch64-apple-darwin
  teleforge-agent-0.2.0-<triple>.slim
  teleforge-agent-0.2.0-<triple>.zstd-patch      (дельта от 0.1.0)
```

Артефакты **не** кладутся в ветку: 20 МиБ × 4 платформы × каждый релиз сделают
ветку несклонируемой за год. В ветке — только подписанный указатель; «бинарник в
отдельной ветке» остаётся верным для того, что клиент качает первым, а байты
берутся из Releases с `sha256`, зафиксированным в подписанном манифесте.

### 12.2 Манифест

```json
{
  "schema": 1,
  "channel": "stable",
  "version": "0.2.0",
  "generation": 7,
  "min_client_version": "0.1.0",
  "min_agent_version": "0.1.0",
  "tdlib_version": "1.8.29",
  "protocol": 1,
  "published_at": 1771000000,
  "artifacts": [
    { "triple": "x86_64-unknown-linux-musl", "libc": "musl",
      "url": "https://github.com/.../teleforge-agent-0.2.0-x86_64-unknown-linux-musl",
      "sha256": "…64 hex…", "size": 8123456, "slim": false,
      "sig": "…128 hex…" }
  ],
  "deltas": [
    { "from": "0.1.0", "to": "0.2.0", "triple": "x86_64-unknown-linux-musl",
      "algorithm": "zstd-patch", "url": "…", "sha256": "…", "size": 412000,
      "result_sha256": "…" }
  ]
}
```

`generation` — монотонный счётчик, единственная защита от отката на старую
уязвимую версию. `min_client_version` — низшая совместимая версия клиента
(зеркало `MIN_CLIENT_VERSION` в агенте). `result_sha256` у дельты — чтобы патч не
мог дать бинарник, отличный от подписанного полного артефакта.

### 12.3 Подпись

Канонические байты — не «тот JSON, что отдан»:

```
TFAGENT-MANIFEST\n
schema 1\n
channel stable\n
version 0.2.0\n
generation 7\n
min_client_version 0.1.0\n
min_agent_version 0.1.0\n
tdlib_version 1.8.29\n
protocol 1\n
published_at 1771000000\n
artifact x86_64-unknown-linux-musl musl 8123456 <sha256> full <sig>\n
…
delta x86_64-unknown-linux-musl 0.1.0 0.2.0 zstd-patch 412000 <result_sha256>\n
```

Артефакты и дельты сортируются по `triple` (+ `from` для дельт), поэтому
подписывающий и проверяющий не могут разойтись в порядке.

Почему канонизация, а не «подпишем байты ответа»: порядок ключей и пробелы в JSON
не стабильны между сериализаторами, и подпись ломалась бы при любом
переформатировании — а «давайте нормализуем перед проверкой» это классический
путь к обходу проверки подписи.

Ключ: тот же Ed25519-корень, что у плагинов (`docs/PLUGIN_TRUST.md`,
`kRootPublicKeyHex`). Вшит в клиент и в агент (`TF_AGENT_ROOT_PUBKEY_HEX`,
`crypto::sig::COMPILED_ROOT_PUBKEY_HEX`). Без него `self-test` падает с
`SELF_TEST_FAILED` — релиз без корневого ключа физически нельзя выпустить (это
проверено: см. § 15).

### 12.4 Проверка перед запуском

Клиент, получив манифест:

1. `verify_manifest` — `validate_shape` (схема, канал, формат sha256/подписи) →
   `sig::verify` над каноническими байтами;
2. `assert_client_supported` — `min_client_version`;
3. `assert_not_rollback` — `generation > last_applied`;
4. `select_artifact` — по `uname -m`/`uname -s`/libc; предпочтение musl на Linux;
5. артефакт: `sha256` == манифест **и** отдельная подпись артефакта (защита от
   подмены `sha256` в валидном манифесте);
6. заливка, затем `sha256sum` **на сервере** и сверка (защита от порчи при
   передаче и от подмены на диске);
7. `--self-test` (exit 0 обязателен);
8. `ln -sfn`, restart, `status`-healthcheck;
9. провал на 7–8 → откат на `previous`.

Что это не защищает: подмену самого клиента. Это вне модели — если у атакующего
код на машине пользователя, он и так имеет всё.

### 12.5 `agent-bin` и CI

`.github-workflow-draft.yml` собирает musl-статические артефакты, подписывает,
кладёт в Releases и синхронизирует ветку `agent-bin` (один коммит, force-push).
Ключи — только в secrets (`TF_AGENT_SIGNING_KEY`), не в коде; ключ не
используется на этапе `cargo test`/`check`.

---

## 13. Отдельные меры по опасным местам

### 13.1 Секреты в памяти

| Мера | Где | Эффект |
|---|---|---|
| `SecretBytes` без `Clone`/`Display`, `Drop` зануляет | `secret.rs` | секрет не утечёт «случайной копией» |
| `mlock` + `MADV_DONTDUMP` | `secret.rs::lock_pages` | нет в swap, нет в core |
| `RLIMIT_CORE=0`, `PR_SET_DUMPABLE=0` | `main` (TODO) | нельзя прочитать `/proc/<pid>/mem` |
| `LimitMEMLOCK=16M` в unit | unit | `mlock` вообще работает |
| ключ только между `unlock` и `lock` | `vault` | нет ключа «на всякий случай» на диске |

### 13.2 Логирование

* Типы с секретами не реализуют `Display`; `Debug` печатает `<redacted>`.
* Каждая строка проходит `logging::redact_line`: ключи из `SENSITIVE_KEYS`
  (`password`, `token`, `secret`, `apikey`, `pgconnstring`, `dek`, `salt`,
  `verifier`, `signature`, …) и любые «блобы» длиннее 64 символов, похожие на
  hex/base64, заменяются.
* TDLib: `td_set_log_verbosity_level(1)` — только fatal/error; сообщения
  проходят через `redact_line` (TDLib в info-уровне печатает названия чатов).
* Это исправляет L6 (`LOG` с произвольным `detail`) и M14 (секрет в логе).

### 13.3 Права

`fsperm` — единственная точка создания файлов/каталогов агента: `0700` на
каталоги, `0600` на файлы с секретами, `0640` на конфиг, `0660` на сокет,
`O_NOFOLLOW`, `set_mode` **до** записи, `fsync` каталога после `rename`.
`assert_private(data_dir)` в `serve` и `self-test` отказывается стартовать, если
каталог доступен группе/остальным.

На Windows — явная защищённая DACL вместо унаследованной: `fsperm` снимает
наследование через `icacls /inheritance:r /grant:r <user>:(OI)(CI)F` и **честно
сообщает**, что POSIX-режимы на этой платформе не верифицируются
(`fsperm::posix_modes_enforced()`), поэтому `self-test` печатает `warn`, а не
«0700 проверено». Production-таргет — Linux.

### 13.4 Устойчивость к подмене бинарника

* Подпись Ed25519 манифеста **и** артефакта, корневой ключ вшит.
* `generation` монотонный, откат запрещён.
* `current` — симлинк, подмена = `rename`, а не «дописывание поверх».
* `previous` сохраняется до healthcheck.
* `sha256sum` проверяется на сервере после заливки.
* Пиннинг fingerprint'а listener-сертификата при установке (T10).

---

## 14. Чего в дизайне нет и почему

| Не делаем | Причина |
|---|---|
| серверный ANN-поиск по эмбеддингам по умолчанию | он требует открытого текста или проекции, сохраняющей косинус — ровно то, что делает `ProjectEmbedding` (M10) |
| синхронизацию `PersonalityCore` целиком | именно она уносила `openAiApiKey` и пароль БД (C2). Синхронизируются отдельные не-секретные поля, а секреты — в vault и никогда в сеть |
| облачный «чат синхронизации» в Telegram | канал виден, пересылаем, не чистится, ломается ghost-режимом (H10, M13). Агент заменяет этот транспорт |
| gRPC в v1 | см. § 9.1; Protobuf-схема зафиксирована, транспорт — v2 |
| мастер-пароль «на все тенанты» | один тенант = одна соль = одна обёртка; общий KEK сделал бы компромисс одного тенанта компромиссом всех |
| автоматическое разрешение конфликтов | это и был H7 |

---

## 15. Проверка

Тулчейн на машине разработки **есть** (`cargo 1.96.0-nightly`), поэтому код
проверен, а не только прочитан. После шага 1a (`docs/migration.md`) пассивный
модуль, примитивы `crypto/**` и хранилище секретов уже не заглушки:

```
cargo check --offline --all-targets   # 0 ошибок, 0 предупреждений
cargo test  --offline                 # 220 тестов, 0 падений
cargo run   --bin teleforge-agent -- version     # teleforge-agent 0.1.0
cargo run   --bin teleforge-agent -- self-test   # JSON-отчёт; exit 0 при warn,
                                                 #   exit 7 (SELF_TEST_FAILED) при fail
```

Тестами покрыта вся логика, которую можно проверить без сети и без TDLib:
HLC, vector clock, `classify`/`plan`, курсор, протокол, framing, updater,
формат конверта AEAD, реальные Argon2id/HKDF/XChaCha20-Poly1305, пассивный журнал
(идемпотентность, порядок `op_seq`, изоляция аккаунтов, отсутствие LWW, ретенция),
конверт секретов (включая «в `Debug` нет плейнтекста») и `--self-test`.
Подробная таблица «свойство → тест» — `docs/passive-module.md` § 7.

Тестами покрыта вся логика, которую можно проверить без сети и без TDLib:

* HLC монотонен при остановившихся и при убежавших назад часах;
* vector clock различает `Concurrent`/`Before`/`After`/`Equal`, encode/decode;
* `classify` даёт `Conflict` для конкурентной правки одного поля и `Applied` —
  для разных полей; повтор даёт `Duplicate`; tombstone не воскрешается;
* курсор не откатывается, не переполняется, лимит страницы зажимается;
* framing отвергает превышение размера до чтения тела, нулевой кадр, обрезанное
  тело;
* `redact_line` вырезает `password=`, токены, длинные блобы;
* конверт AEAD: round-trip заголовка, порядок байт == порядок значений;
* версии сравниваются численно (`1.10.0 > 1.9.0`) и в TDLib, и в манифесте;
* канонические байты манифеста не зависят от порядка артефактов и меняются от
  каждого подписываемого поля;
* `decide` ставит `Install` только для реально новой версии и refuses при
  откате/старом клиенте; `healthcheck` ловит несовпадение версии/протокола и
  `failed(` в компонентах;
* `plan_ssh_args` не содержит `accept-new`, destination последний;
* `rewrite_conn_host_port` вырезает `password=`;
* схема объявляет `account_id` во всех таблицах, `op_id` уникален,
  `busy_timeout`/WAL в схеме;
* пассивный журнал: дедуп по `content_hash`, монотонный `op_seq` на аккаунт,
  чужой `account_id` отклоняется, new+edit+edit+delete = 4 строки,
  `ROLLBACK` при ошибке и при панике, ретенция по возрасту и по размеру;
* реальные примитивы: Argon2id детерминирован по соли и чувствителен к паролю,
  HKDF разделяет метки, AEAD не открывается чужим ключом/AAD, `StubKdf`/`StubAead`
  отказывают в релизной сборке и несовместимы с настоящими;
* конверт секретов: перенос в чужой аккаунт и подмена `key_id` не открываются,
  `Debug`/`to_json` не содержат ни пароля, ни DEK, файл создаётся через `fsperm`;
* `--self-test`: отчёт с 8 проверками, `ok`/`exit_code`, провал схемы и прав
  ловится, отсутствующий каталог данных — `warn`, а не `fail`.

Не покрыто (и это честно записано): все примитивы (`argon2`, `aead`, `ed25519`,
`sha2`, `hkdf`, `rusqlite`, `russh`, FFI) — они заглушки с `TODO(<crate>)`,
потому что требуют сети и вендоринга.

---

## 16. Матрица: находка аудита → решение

| Находка | Как закрыта | Где |
|---|---|---|
| **C1** ключ из `userId+phone` | Argon2id + случайная соль + случайный DEK; ничего не выводится из публичных данных | § 7.2, `crypto::kdf` |
| **C2** секреты уезжают в облако | нет `VACUUM INTO`; `PersonalityCore` не синхронизируется; секреты в vault, не в сети | § 10.1, § 14 |
| **C3** одна БД на аккаунты | `account_id` в каждой таблице и в `EntityKey`, ключ тенанта случайный | § 10.2 |
| **C4** нет `rollback` | `TransactionGuard` c `ROLLBACK` в `Drop` | § 10.6 |
| **C5** доступ к SQLite из воркеров | `SyncConnection` не `Sync`, один поток-владелец | § 4, § 10.6 |
| **H1** очередь присутствия в памяти | операция пишется в `op_log` в той же транзакции, что и поле; нет «отложенного flush» | § 10.6 |
| **H2** нет FLOOD_WAIT/backoff | `supervisor::Backoff`, `RateLimiter`, компонент уходит в `Failed` и ждёт | § 4, § 6.6 |
| **H3** нет ретенции | ретенция привязана к `op_seq`/HLC и к `prune_upto(keep_floor)` по `device_cursor` | § 10.5 |
| **H4** `MemorySyncEvent` — журнал в никуда | его нет; `op_log` — единственный журнал и он читается | § 10.2 |
| **H5** полный скан на каждую запись | дельта по `op_seq`, пачки, дебаунс на клиенте | § 10.5 |
| **H6** нет `busy_timeout` | `PRAGMA` в `SCHEMA_SQL`, на каждом соединении | § 10.2 |
| **H7** LWW по секундам, мерж по `id` | HLC + vector clock + пофайловый мерж; `MemoryItem` по `contentHash` | § 10.3, § 10.4 |
| **H8** `accountIdH` = `fromId` | `account_id` — владелец, из `EntityKey`; уникальность `op_id` | § 10.1, § 10.2 |
| **H9** `teleforge_events` не журнал | `op_log` с `UNIQUE(op_id)` и ревизией в ключе | § 9.3, § 10.2 |
| **H10** выборы лидера в канале | транспорта-канала нет; лидер не нужен, есть один агент | § 14 |
| **H11** плагины получают всё | агент плагинов не запускает; gate остаётся задачей клиента | — |
| **M4** дубли `OnlineEvent` | `entity_root.vclock` + `Equal → Duplicate` | § 10.4 |
| **M10** «спрятанный» вектор | эмбеддинги AEAD целиком, поиск на клиенте | § 10.7 |
| **M12** туннель блокирует воркер | state machine + `russh`, без блокирующего ожидания | § 4, `tunnel` |
| **M14** секреты plaintext у БД | vault + `agent.toml` без секретов | § 3.2, § 8 |
| **L7** `max(id)+1` | `seq.next` хранимый счётчик | § 10.5 |
| **L9** расшифрованная БД в `/tmp` | ничего в `$TMPDIR`, tmp внутри тенанта с `unlink` | § 8.2 |
| **L5** неограниченная очередь | `IdempotencyStore` ограничен TTL и ёмкостью | `api::server` |

Не закрыто намеренно: H11 (плагины) — это клиентская подсистема, а не серверная;
вынесено в `docs/migration.md` как отдельная задача.

---

## 17. Открытые вопросы

1. **TDLib static на musl.** `tdjson` тянет OpenSSL и zlib; статическая сборка под
   musl требует `-DTD_ENABLE_LTO=OFF` и явного `RUSTFLAGS` с `+crt-static`.
   Нужен один пробный прогон CI, пока это предположение.
2. **Медиа на сервере.** Сейчас агент не качает медиа. Если понадобится «архив с
   фото», это отдельное решение про шифрование файлов (LUKS vs per-file AEAD) и
   про объём.
3. **Несколько тенантов на одном VPS.** Схема поддерживает (`tenants/*`), но UI
   клиента предполагает один. Решить до первой миграции.
4. **`--tunnel-backend=ssh-binary`** нужен только на первый переходный период;
   убрать после того как `russh`-путь проверен на реальных хостингах (некоторые
   провайдеры режут нестандартные каналы).
5. **Windows-сервер.** Агент под Windows технически возможен (`fsperm` уже
   помечает, что нужна DACL), но не приоритет.
6. **Совместимость со старым `TFSYNC1`.** Читать умеем (формат конверта
   поддерживает `AES-256-GCM` и legacy-magic), но конвертер старых бандлов в
   новую модель не написан — это шаг 4 в `docs/migration.md`.
