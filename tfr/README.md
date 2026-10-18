# teleforge-agent

Серверный агент TeleForge: SSH-туннель до вашей БД, шифрованное хранение сессий
Telegram, серверная сторона синхронизации и самопроверяющийся апдейтер.

Один бинарник. Ставится **десктопным клиентом** по вашему SSH-доступу, слушает
только `127.0.0.1` и unix-socket, наружу не открывается.

```
rust/teleforge-agent/
├── README.md                     ← вы здесь
├── Cargo.toml                    крейт и план зависимостей (сейчас: только std)
├── docs/
│   ├── architecture.md           полный дизайн: компоненты, крипто, протокол, модель данных
│   ├── db-ingest.md              серверный ingest из БД: доступ, таблицы, курсор, включение
│   └── migration.md              пошаговый перенос C++ → Rust, dual-run
├── src/
│   ├── main.rs                   CLI, диспетчеризация, exit-коды
│   ├── lib.rs                    состав крейта, версии, политика
│   ├── error.rs                  ошибки → коды протокола и exit-коды
│   ├── cli.rs                    разбор argv
│   ├── config.rs                 agent.toml (без секретов)
│   ├── secret.rs                 SecretBytes: зануление, mlock, constant-time
│   ├── fsperm.rs                 единственная точка создания файлов и прав
│   ├── logging.rs                redaction, уровни
│   ├── auth.rs                   mTLS/токен, scopes, replay, rate limit
│   ├── vault.rs                  соль, KDF, обёртка DEK, unlock, recovery
│   ├── tdlib.rs                  TDLib через C ABI, шифрование его БД
│   ├── tunnel.rs                 SSH-форвард, состояние, known_hosts
│   ├── supervisor.rs             состояния компонентов, backoff, health
│   ├── db/                       чтение PostgreSQL/MySQL → запечатать → ops (pull-first)
│   │   ├── mod.rs                DbSource/DbRow/DbStatus, классы ошибок
│   │   ├── allowlist.rs          что агенту разрешено читать (и что запрещено навсегда)
│   │   ├── credentials.rs        строка подключения в vault-слоте
│   │   ├── postgres.rs, mysql.rs драйверы, rustls+ring, probe грантов
│   │   └── ingest.rs             проход: курсор → SELECT → seal → ops (одна транзакция)
│   ├── updater.rs                манифест, Ed25519, стадии, откат
│   ├── crypto/
│   │   ├── kdf.rs                Argon2id + HKDF + метки
│   │   ├── aead.rs               XChaCha20-Poly1305 / AES-256-GCM, конверт
│   │   └── sig.rs                Ed25519, sha256, hex
│   ├── sync/
│   │   ├── mod.rs                зачем новая модель (и чем плоха старая)
│   │   ├── clock.rs              гибридные логические часы
│   │   ├── vclock.rs             векторные часы, причинность
│   │   ├── model.rs              EntityKey, FieldId, схема SQLite
│   │   ├── cursor.rs             монотонный курсор, страницы
│   │   └── engine.rs             classify/plan, транзакции
│   └── api/
│       ├── mod.rs                стек транспорта
│       ├── http.rs               HTTP/1.1: границы до аллокации, статусы
│       ├── tls.rs                rustls 1.3, пиннинг сертификата
│       ├── pem.rs, base64.rs     разбор PEM и base64 без лишних зависимостей
│       ├── rpc.rs                конверт, auth, rate limit, idempotency, dispatch
│       ├── sync.rs               SyncService: push/pull/stamp/conflicts/prune
│       ├── signals.rs            SIGTERM/SIGINT → флаг остановки
│       ├── protocol.rs           методы, конверт, коды, JSON-хелперы
│       ├── framing.rs            кадры с префиксом длины (v2-совместимо)
│       └── server.rs             listener, лимиты, планировщики ретенции и tf-db-ingest
└── .github-workflow-draft.yml    черновик CI (musl, подпись, Releases, agent-bin)
```

---

## Зачем это отдельный процесс

Сейчас всё это делает само приложение:

* `ayu/features/sync/teleforge_ssh_tunnel.cpp` — поднимает `ssh -L` до БД;
* `ayu/features/sync/teleforge_sync*.cpp` — шифрует и выгружает базу;
* `ayu/features/spy/**`, `ayu/features/teleforge/tf_peer_archive*` — собирают то,
  что синхронизируется.

Три причины вынести это на сервер:

1. **Ключ.** Сегодня ключ синхронизации — `SHA-256("TeleForge-Sync-v1" + userId +
   phone + "account")` (`teleforge_sync_crypto.cpp`, `DeriveSyncKey`): один
   SHA-256 от двух публичных значений. Подробный разбор — в
   `docs/architecture.md` § 7.1. Настоящий ключ должен вводиться человеком и
   жить там, где данных нет.
2. **Сессия.** Пока клиент хранит `tdata` и TeleForge-базу рядом, любая утечка
   каталога — это утечка и сессии, и архива. Отдельный процесс с отдельными
   правами и отдельным шифрованием разрывает эту связь.
3. **Доступность.** Агент работает, когда клиент закрыт: туннель к БД, ретенция,
   приём синхронизации с телефона.

Документ с разбором всех найденных проблем и матрицей «находка → решение»:
`docs/architecture.md` § 16.

---

## Как это ставится

**Клиент ставит агента сам.** Пользователь вводит доступ к серверу в десктопном
клиенте, дальше всё автоматически:

```
1. пользователь: user/password | root | alias из ~/.ssh/config | путь к ключу
2. клиент подключается по SSH
3. определяет цель: uname -s, uname -m, libc, uid
4. берёт подписанный манифест из ветки agent-bin
5. скачивает нужный артефакт из Releases, проверяет sha256 и подпись Ed25519
6. заливает бинарник (SFTP, либо ssh 'cat >', либо base64 — что доступно)
7. проверяет sha256 на сервере
8. запускает `teleforge-agent self-test` (exit 0 обязателен)
9. ставит user-level systemd unit, loginctl enable-linger
10. systemctl --user enable --now
11. bootstrap: pair → init(tenant, passphrase) → tenant_id, device_token, recovery_code
12. healthcheck через `status`, показывает состояние в UI
```

Полная последовательность, права, что требует root и почему, а также fallback'и
для серверов без SFTP — в `docs/architecture.md` § 3.

Обновление: клиент сравнивает `status.agent_version` с манифестом, скачивает
новый бинарник, заливает по SSH, проверяет, кладёт в
`updater/releases/<version>/`, атомарно переключает симлинк `current`,
перезапускает юнит, проверяет healthcheck и при провале откатывается на
`previous`. Рабочий файл никогда не перезаписывается.

---

## Сборка

### Требуется

* Rust ≥ 1.85 (на машине разработки проверено на `1.96.0-nightly`; 1.85 —
  требование `zeroize` 1.9 и `rusqlite` 0.40);
* для musl-артефактов: `rustup target add x86_64-unknown-linux-musl` и
  `musl-tools`; для aarch64 — `cross`;
* для `--features tdlib-link`: собранный `libtdjson.a`/`libtdjson.so` и
  `TDJSON_LIB_DIR`;
* для подписи релиза: `TF_AGENT_ROOT_PUBKEY_HEX` в окружении сборки (иначе
  `self-test` откажется проходить — это защита от выпуска сборки, которая не
  умеет проверять обновления).

### Проверка без сети

Зависимости вендорятся через `Cargo.lock` в `target/`, поэтому проверка работает
офлайн, без TDLib (сеть нужна только на первый `cargo fetch`):

```bash
cargo check --offline --all-targets     # 0 ошибок, 0 предупреждений
cargo test  --offline                   # 412 тестов
cargo run   --offline --bin teleforge-agent -- version
cargo run   --offline --bin teleforge-agent -- self-test --tenant <hex>
```

`self-test` печатает один JSON-объект; `exit 7`, если провалилась хоть одна
проверка (схема БД, права, Argon2id/AEAD, `store.txn`). На не-Linux права
проверяются как `warn`, а не `fail` — см. `docs/passive-module.md` § 5.4.

Что именно покрыто тестами — `docs/architecture.md` § 15 и
`docs/passive-module.md` § 7.

### Сборка релиза

```bash
# linux x86_64, статически (основной артефакт: его клиент заливает по SSH)
cargo build --release --target x86_64-unknown-linux-musl

# linux aarch64
cross build --release --target aarch64-unknown-linux-musl

# без TDLib (slim, ~20 МиБ меньше)
cargo build --release --target x86_64-unknown-linux-musl --no-default-features
```

Профиль `release` настроен на размер (`opt-level = "z"`, `lto = "thin"`,
`panic = "abort"`, `strip`), потому что этот бинарник едет через SSH-соединение
пользователя.

### Зависимости

В `Cargo.toml` подключены только те, что уже используются кодом; остальные
перечислены комментариями и остаются заглушками с `TODO(<crate>)`.

| Крейт | Состояние | Для чего |
|---|---|---|
| `argon2` | подключён | KEK из пароля (§ 7.2) |
| `chacha20poly1305`, `aes-gcm` | подключены | шифрование на диске (§ 7.3) |
| `hkdf`, `sha2` | подключены | разделение подключей, `content_hash` |
| `rand` | подключён | соли, nonce, DEK, токены (`OsRng`) |
| `zeroize` | подключён | зануление стековых копий в KDF |
| `rusqlite` (`bundled`) | подключён | пассивный store, без системного SQLite |
| `hmac`, `ed25519-dalek` | план | blinding id, подпись манифеста |
| `russh`, `russh-keys` | план | SSH-туннель в процессе |
| `serde`, `serde_json` | подключены | протокол `sync.*`, `auth/clients.json` |
| `rustls` (+ `ring`) | подключён | TLS 1.3 для loopback-listener'а |
| `libc` (только unix) | подключён | SIGTERM/SIGINT → флаг остановки |
| `toml` | план | `agent.toml` (сейчас флаги CLI) |
| `tiny_http`/`hyper` | план | если понадобится keep-alive (сейчас свой HTTP/1.1) |
| `tokio` | план | асинхронный accept и туннель |
| `libc` | план | `mlock`, `prctl`, `RLIMIT` |
| `tracing`, `tracing-subscriber` | план | структурированные логи |

---

## Текущее состояние

Серверная половина пассивного контура, хранилище секретов **и транспорт
`sync.*`** реализованы; SSH, TDLib и подпись манифеста остаются заглушками,
возвращающими `Error::NotImplemented` с именем нужного крейта.

**Уточнённая модель (после согласования):** агент стоит на том же сервере, что и
БД пользователя, и сам читает из неё изменения (шифрует и публикует), а клиент в
основном читает (`sync.pull`); `sync.push` — только для локальных данных.
Серверное чтение БД, `db.status` и права БД **реализованы** в объёме
outbox-таблицы: `src/db/**`, курсор схемы v3, поток `tf-db-ingest`,
`--ingest-*`, роли токенов. Как включить и что читается — `docs/db-ingest.md`;
расхождения и открытые пункты — `docs/transport.md` § 15.

| Готово | Заглушка / не начато |
|---|---|
| пассивный журнал: `ops`, `entities`, `presence`, `profiles`, `archive_obs` | клиентская половина (`Agent::ShadowIngest`) — `docs/client-integration.md` |
| транспорт: TLS 1.3 + HTTP/1.1 + JSON-RPC, `sync.push`/`pull`/`stamp`/`conflicts`/`prune`, лимиты, 400/401/403/409/413/429 | mTLS на loopback, unix-socket listener, `pair`/`unlock` по сети |
| серверный ingest: PostgreSQL/MySQL по TLS, allowlist таблиц/колонок, курсор v3, запечатывание `subkey(LABEL_SYNC_DATA)`, `db.status`, `db.ingest` | отображение облачных таблиц (`messages` и др.), `db-check`, `db-credentials rotate` |
| приём событий: валидация, HLC, дедуп, `op_seq`, RAII-транзакция (C4), конфликты | — |
| ретенция по возрасту/размеру + `prune_upto` + планировщик в `serve` | — |
| миграции схемы `v1 → v2 → v3`, идемпотентные | — |
| Argon2id, HKDF, XChaCha20-Poly1305, AES-256-GCM, `OsRng`, `SecretBytes::wipe` | `mlock`/`MADV_DONTDUMP` (`SecretBytes::lock_pages`) |
| account-level конверт (C1), `rotate`, recovery-код, права `0600`, `Debug` без секретов | `recovery-rotate` (перевыпуск кода) |
| HLC, vector clock, `classify`/`plan` | запись `op_log` через `sync::engine` |
| канонические байты манифеста, `decide`, `healthcheck` | скачивание и подпись (Ed25519) |
| `self-test`: схема, миграции, права, KDF/AEAD, recovery, `store.txn`, коды выхода | — |
| `plan_ssh_args`, `parse_target`, `rewrite_conn_host_port` | `russh` |
| `parse_version`, `check_version`, состояния авторизации TDLib | FFI-вызовы |
| redaction логов, `fsperm`, `Backoff`, `IdempotencyStore` | — (ACL Windows — `icacls`, без верификации) |

Что делать первым — `docs/migration.md`, раздел «Что делать первым»; что сделано
по шагу 1a — `docs/passive-module.md`.

---

## Правила, которые нельзя нарушать при доработке

1. **Секрет не выводится из публичных данных.** Ни `userId`, ни телефон, ни
   username не участвуют в выводе ключа.
2. **Секрет не реализует `Display`.** `Debug` у `SecretBytes` печатает
   `<redacted>`; любая новая обёртка над секретом обязана это повторять.
3. **Новый файл с секретом создаётся только через `fsperm`.** Права ставятся до
   записи содержимого; `O_NOFOLLOW`.
4. **Новое поле сущности получает новый `FieldId`.** Номера не переиспользуются.
5. **`match` по `Method`/`FieldId`/`EntityKind` без `_`** — новый вариант обязан
   обработаться явно, иначе не скомпилируется.
6. **Конфликт не разрешается автоматически.** Автовыбор победителя — это баг,
   который уже был.
7. **Ничего не пишется в `$TMPDIR`.**
8. **Логи проходят через `redact_line`.** Прямой `println!` секрета — дефект.
