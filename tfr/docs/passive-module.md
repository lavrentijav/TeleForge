# Пассивный модуль: журнал событий, хранилище секретов, `--self-test`

Статус: **реализовано**, шаги «1a» и «3 (серверная половина)» из
[`migration.md`](migration.md). Модуль хранит то, что клиент или **сам сервер**
наблюдает без запроса: сообщения (включая правки и удаления), присутствие,
профили, архивные наблюдения — с привязкой к аккаунту, монотонной
последовательностью и без LWW-затирания. После уточнения модели основной
источник — серверный ingest из общей БД ([`db-ingest.md`](db-ingest.md)), а
`sync.push` остаётся для локальных фактов; транспорт — [`transport.md`](transport.md).
Поверх журнала работают ретенция по расписанию, миграции схемы (v1 → v2 → v3) и
recovery-код account-level конверта.

Закрывает Critical **C1** из `docs/audits/passive-sync-audit.md`, а попутно
адресует **C3, C4, H3, H6, H7, H8, H9**.

Код: `rust/teleforge-agent/src/{store,keystore,selftest,auth}.rs`,
`src/api/{http,tls,pem,rpc,sync,server,signals,base64}.rs` и `src/db/**`
(серверный ingest из общей БД, [`db-ingest.md`](db-ingest.md)).
Тесты: `cargo test` — **412** (было 323), `cargo clippy --all-targets -D warnings`
и `cargo fmt --check` чистые.

> ### Тот же контур, а не «вторая БД рядом с клиентской»
>
> `sync/data.db` — не замена и не копия клиентской `data.tforge`. Это журнал
> **серверной стороны одного контура**: его наполняет агент, стоящий на том же
> сервере, что и общая БД пользователя (PostgreSQL/MySQL, как сейчас
> `teleforge_ssh_tunnel.cpp`), и отдаёт клиентам через `sync.pull`. Клиент —
> **читатель**: он подтягивает изменения и применяет их к своей локальной БД.
> `sync.push` из клиента — дополнительный путь только для того, чего в БД нет
> (ghost-настройки, «прочитано в госте»).
>
> Следствие для этой схемы: `ops` — это реплика изменений **из общей БД**, а не
> самостоятельный источник истины. Истина остаётся в БД на сервере (и,
> опционально, в облаке/канале как зашифрованная копия). Поэтому `op_seq` — это
> курсор публикации, а не «номер события, которое придумал клиент».
>
> **Статус наполнения:** сейчас журнал наполняется только через `sync.push`.
> Серверного чтения БД (`db.*`, курсор чтения, шифрование DB-строк) в коде ещё
> **нет** — это ближайший шаг; полный список расхождений — `docs/transport.md`
> § 15.

---

## 1. Карта модулей

| Файл | Что внутри |
|---|---|
| `src/store/mod.rs` | публичный API и таблица гарантий; `open_for_tenant` |
| `src/store/schema.rs` | DDL как одна константа `SCHEMA_SQL`, `SCHEMA_VERSION`, списки обязательных таблиц/индексов |
| `src/store/event.rs` | `AccountId`, `EventKind`, `EntityId` (парсер), `Event`, `content_hash`, `RejectReason`, `IngestResult` |
| `src/store/conn.rs` | `Store` (одно соединение), `Tx` — RAII-транзакция с `ROLLBACK` в `Drop`, все write-примитивы |
| `src/store/ingest.rs` | валидация и `ingest`/`ingest_batch` в одной транзакции |
| `src/store/query.rs` | истории, производные «текущие» значения, `self_check` |
| `src/store/retention.rs` | `purge_by_age`, `purge_by_size`, `prune_upto`, `pruned_upto`, `RetentionPolicy` |
| `src/store/migrate.rs` | версии схемы, идемпотентные шаги (`v1→v2→v3`), `read_version`, `column_exists` |
| `src/store/cursor.rs` | `db_cursor` — позиция чтения общей БД: `read`/`advance`/`advance_in_tx`, монотонность в SQL |
| `src/db/mod.rs` | `DbSource`/`DbRow`/`DbStatus`, `class_of`, классы ошибок; граница «что видно наружу» |
| `src/db/{postgres,mysql}.rs` | драйверы (синхронные, rustls+ring), один `SELECT` из allowlist, probe грантов |
| `src/db/allowlist.rs` | закрытый список таблиц/колонок, запрет `personality_core`/`openai_api_key`/`pgConnString` |
| `src/db/ingest.rs` | проход: курсор → страница → запечатать → одна транзакция (`ops` + `db_cursor`) → health |
| `src/db/credentials.rs` | vault-слот строки подключения (`subkey(DEK, LABEL_DB_CONN)`), `0600` |
| `src/keystore.rs` | конверт аккаунта: `Kdf`/`Aead` трейты + реальные и stub-реализации, `Envelope` (с recovery-обёрткой), `SecretStore`, `probe()` |
| `src/selftest.rs` | `--self-test`: список проверок, JSON-отчёт, код выхода |
| `src/auth.rs` | `AuthStore` (`auth/clients.json`, sha256 токена), scopes, rate limiter, replay window |
| `src/api/http.rs` | HTTP/1.1: границы до аллокации, статус-коды, `Connection: close` |
| `src/api/tls.rs` | rustls 1.3, self-signed listener, пиннинг сертификата в тестах |
| `src/api/rpc.rs` | конверт, порядок проверок, idempotency, rate limit, `sync.*`-диспетчер |
| `src/api/sync.rs` | `SyncService`: один владелец стора на тенант, push/pull/stamp/conflicts/prune, ретенция |
| `src/api/server.rs` | listener, лимит соединений, таймауты, планировщики ретенции и `tf-db-ingest` |

Соседние модули, которые перестали быть заглушками (это было необходимо, иначе
C1 не закрыть): `crypto::kdf` (Argon2id, HKDF-SHA256, OsRng), `crypto::aead`
(XChaCha20-Poly1305 и AES-256-GCM, seal/open), `secret` (реальный `OsRng`,
`SecretBytes::wipe`), `fsperm` (права на Windows), `vault` (реальный `unlock`).

---

## 2. Схема БД (DDL)

Файл: `<data_dir>/tenants/<tenant_hex>/sync/data.db` (тот же путь, что возвращает
`vault::sync_db_path`), режим `0600`, WAL. Один файл на тенант, `account_id` —
первая колонка каждой таблицы: это выбор архитектуры «один файл + обязательный
префикс аккаунта», а не «файл на аккаунт» (§ 8.1 `architecture.md`).

`SCHEMA_VERSION = 3`, хранится в `schema_meta`. Агент отказывается открывать
хранилище, у которого версия *новее* его собственной, и **мигрирует** более
старое (v1 → v2 → v3, см. § 2.3).

```sql
PRAGMA journal_mode = WAL;
PRAGMA synchronous = NORMAL;
PRAGMA busy_timeout = 5000;
PRAGMA foreign_keys = ON;
PRAGMA auto_vacuum = INCREMENTAL;

CREATE TABLE IF NOT EXISTS schema_meta (
    key   TEXT NOT NULL PRIMARY KEY,
    value TEXT NOT NULL
) WITHOUT ROWID;
INSERT OR IGNORE INTO schema_meta (key, value) VALUES ('schema_version', '1');
INSERT OR IGNORE INTO schema_meta (key, value) VALUES ('format', 'teleforge-passive-store');

-- Реестр аккаунтов. high_water — последний выданный op_seq для аккаунта.
CREATE TABLE IF NOT EXISTS accounts (
    account_id          BLOB    NOT NULL PRIMARY KEY,
    created_at_ms       INTEGER NOT NULL,
    last_write_ms       INTEGER NOT NULL,
    high_water          INTEGER NOT NULL DEFAULT 0,
    device_count        INTEGER NOT NULL DEFAULT 0,
    -- v2: счётчик conflict_id. Хранимый, а не MAX(id)+1: id не переиспользуется
    -- даже после prune (клиент мог запомнить значение).
    conflict_high_water INTEGER NOT NULL DEFAULT 0
) WITHOUT ROWID;

-- Водяной знак часов на устройство: проверка монотонности HLC.
CREATE TABLE IF NOT EXISTS device_clock (
    account_id        BLOB    NOT NULL,
    device_id         TEXT    NOT NULL,
    last_hlc_physical INTEGER NOT NULL,
    last_hlc_logical  INTEGER NOT NULL,
    last_seen_ms      INTEGER NOT NULL,
    PRIMARY KEY (account_id, device_id),
    FOREIGN KEY (account_id) REFERENCES accounts (account_id)
) WITHOUT ROWID;

-- Проекция сущности: только монотонные агрегаты, никогда значение.
CREATE TABLE IF NOT EXISTS entities (
    account_id        BLOB    NOT NULL,
    entity_id         TEXT    NOT NULL,
    kind              INTEGER NOT NULL,
    first_seen_ms     INTEGER NOT NULL,
    last_seen_ms      INTEGER NOT NULL,
    revision_count    INTEGER NOT NULL DEFAULT 0,
    last_op_seq       INTEGER NOT NULL DEFAULT 0,
    last_hlc_physical INTEGER NOT NULL DEFAULT 0,
    last_hlc_logical  INTEGER NOT NULL DEFAULT 0,
    deleted           INTEGER NOT NULL DEFAULT 0,
    deleted_hlc_physical INTEGER,
    deleted_hlc_logical  INTEGER,
    PRIMARY KEY (account_id, entity_id),
    FOREIGN KEY (account_id) REFERENCES accounts (account_id)
) WITHOUT ROWID;
CREATE INDEX IF NOT EXISTS idx_entities_kind_time
    ON entities (account_id, kind, last_seen_ms DESC);

-- Журнал: одна строка на событие, навсегда. Источник истины.
CREATE TABLE IF NOT EXISTS ops (
    op_seq        INTEGER NOT NULL,
    account_id    BLOB    NOT NULL,
    kind          INTEGER NOT NULL,
    entity_id     TEXT    NOT NULL,
    field         TEXT,
    value         BLOB    NOT NULL,
    hlc_physical  INTEGER NOT NULL,
    hlc_logical   INTEGER NOT NULL,
    device_id     TEXT    NOT NULL,
    content_hash  BLOB    NOT NULL,
    applied_at_ms INTEGER NOT NULL,
    UNIQUE (account_id, op_seq),
    UNIQUE (account_id, content_hash),
    FOREIGN KEY (account_id) REFERENCES accounts (account_id)
);
CREATE INDEX IF NOT EXISTS idx_ops_entity_time
    ON ops (account_id, entity_id, hlc_physical, hlc_logical);
CREATE INDEX IF NOT EXISTS idx_ops_applied ON ops (applied_at_ms);

-- Присутствие (замена OnlineEvent, у которого не было account_id).
CREATE TABLE IF NOT EXISTS presence (
    account_id    BLOB    NOT NULL,
    op_seq        INTEGER NOT NULL,
    peer_id       INTEGER NOT NULL,
    status        INTEGER NOT NULL,
    detail_ms     INTEGER,
    observed_ms   INTEGER NOT NULL,
    hlc_physical  INTEGER NOT NULL,
    hlc_logical   INTEGER NOT NULL,
    device_id     TEXT    NOT NULL,
    content_hash  BLOB    NOT NULL,
    PRIMARY KEY (account_id, op_seq),
    FOREIGN KEY (account_id) REFERENCES accounts (account_id)
) WITHOUT ROWID;
CREATE INDEX IF NOT EXISTS idx_presence_peer_time
    ON presence (account_id, peer_id, observed_ms DESC, op_seq DESC);
CREATE INDEX IF NOT EXISTS idx_presence_observed ON presence (observed_ms);

-- История профиля (замена PeerArchiveName/Username/Bio: одна строка на
-- наблюдение, но с аккаунтом и временем). «Текущее» значение — это запрос.
CREATE TABLE IF NOT EXISTS profiles (
    account_id    BLOB    NOT NULL,
    op_seq        INTEGER NOT NULL,
    peer_id       INTEGER NOT NULL,
    field         TEXT    NOT NULL,
    value         BLOB    NOT NULL,
    observed_ms   INTEGER NOT NULL,
    hlc_physical  INTEGER NOT NULL,
    hlc_logical   INTEGER NOT NULL,
    device_id     TEXT    NOT NULL,
    content_hash  BLOB    NOT NULL,
    PRIMARY KEY (account_id, op_seq),
    FOREIGN KEY (account_id) REFERENCES accounts (account_id)
) WITHOUT ROWID;
CREATE INDEX IF NOT EXISTS idx_profiles_peer_field_time
    ON profiles (account_id, peer_id, field, observed_ms DESC, op_seq DESC);
CREATE INDEX IF NOT EXISTS idx_profiles_observed ON profiles (observed_ms);

-- Архивные наблюдения: «этот peer — автор сообщения в этом чате», «участник»,
-- «чат открыли». payload непрозрачен.
CREATE TABLE IF NOT EXISTS archive_obs (
    account_id    BLOB    NOT NULL,
    op_seq        INTEGER NOT NULL,
    peer_id       INTEGER NOT NULL,
    source        TEXT    NOT NULL,
    payload       BLOB    NOT NULL,
    observed_ms   INTEGER NOT NULL,
    hlc_physical  INTEGER NOT NULL,
    hlc_logical   INTEGER NOT NULL,
    device_id     TEXT    NOT NULL,
    content_hash  BLOB    NOT NULL,
    PRIMARY KEY (account_id, op_seq),
    FOREIGN KEY (account_id) REFERENCES accounts (account_id)
) WITHOUT ROWID;
CREATE INDEX IF NOT EXISTS idx_archive_peer_time
    ON archive_obs (account_id, peer_id, observed_ms DESC);
CREATE INDEX IF NOT EXISTS idx_archive_observed ON archive_obs (observed_ms);

-- v2: то, что агент отказывается решать молча. Обе версии значения остаются в
-- `ops`; эта таблица — индекс пар «другое устройство уже записало это поле с
-- более новым HLC». `resolved_by` заполняет человек (через клиент).
CREATE TABLE IF NOT EXISTS conflicts (
    account_id        BLOB    NOT NULL,
    conflict_id       INTEGER NOT NULL,
    entity_id         TEXT    NOT NULL,
    field             TEXT,
    kind              INTEGER NOT NULL,
    local_op_seq      INTEGER NOT NULL,
    local_hlc_physical INTEGER NOT NULL,
    local_hlc_logical  INTEGER NOT NULL,
    local_device      TEXT    NOT NULL,
    remote_op_seq     INTEGER NOT NULL,
    remote_hlc_physical INTEGER NOT NULL,
    remote_hlc_logical  INTEGER NOT NULL,
    remote_device     TEXT    NOT NULL,
    detected_at_ms    INTEGER NOT NULL,
    resolved_by       TEXT,
    PRIMARY KEY (account_id, conflict_id),
    FOREIGN KEY (account_id) REFERENCES accounts (account_id)
) WITHOUT ROWID;
CREATE INDEX IF NOT EXISTS idx_conflicts_detected
    ON conflicts (account_id, detected_at_ms ASC, conflict_id ASC);

-- v3: позиция чтения общей БД (PostgreSQL/MySQL) для серверного ingest.
-- Отдельная таблица, а не device_clock/high_water: те описывают устройства и
-- журнал. `source` — таблица, к которой относится курсор: id из разных таблиц
-- несравнимы. last_key монотонен в SQL (CASE WHEN excluded.last_key > ...), и
-- пишется в ОДНОЙ транзакции с ops, поэтому «опубликовано, но не записано» и
-- «записано, но не опубликовано» невозможны.
CREATE TABLE IF NOT EXISTS db_cursor (
    account_id BLOB    NOT NULL,
    source     TEXT    NOT NULL,
    last_key   INTEGER NOT NULL,
    updated_at INTEGER NOT NULL,
    PRIMARY KEY (account_id, source),
    FOREIGN KEY (account_id) REFERENCES accounts (account_id)
) WITHOUT ROWID;
```

### 2.1 Почему именно так

| Решение | Причина (находка аудита) |
|---|---|
| `account_id` в каждой таблице + `FOREIGN KEY` на `accounts` | C3: у `OnlineEvent`, `PeerArchive*`, `MemoryItem` не было аккаунта, и id чатов (выделяются в рамках аккаунта) сталкивались. Здесь запись без зарегистрированного аккаунта — ошибка `FOREIGN KEY`, а не строка, которая потом «протечёт» в чужой аккаунт |
| `UNIQUE (account_id, op_seq)` | Требование дизайна: `op_seq` монотонна **на аккаунт**. Глобальный счётчик был бы межаккаунтной связью |
| `UNIQUE (account_id, content_hash)` | Идемпотентность повтора. Именно per-account: одинаковые байты под двумя аккаунтами — два факта |
| `ops` без `WITHOUT ROWID` | Нужен `rowid` для дешёвого удаления самых старых строк при ретенции |
| `presence`/`profiles`/`archive_obs` c `PRIMARY KEY (account_id, op_seq)` и `WITHOUT ROWID` | Ключ и есть путь доступа; `op_seq` общий с `ops`, поэтому проекция и журнал удаляются вместе |
| `entities` хранит только агрегаты (`revision_count`, `last_seen_ms`, `deleted`) | H7: единственное, что можно обновлять, — монотонные агрегаты. Значение никогда не перезаписывается |
| Индексы `(account_id, peer_id, время DESC)` | H3: в C++ индекс не совпадал с запросом, и «история для пира» была сканом. Здесь под каждый реальный запрос |
| Индекс по `applied_at_ms` / `observed_ms` | Ретенция по возрасту должна быть индексным диапазоном, а не сканом (H3) |
| `db_cursor` отдельной таблицей (v3) | Позиция чтения общей БД — не часы устройства и не `op_seq`. Перегрузка любого из них связала бы два независимых счётчика; ключ `(account_id, source)` потому, что id из разных таблиц несравнимы (`docs/db-ingest.md` § 4) |

### 2.2 Прагмы: обоснование

* `journal_mode=WAL` — читатель (`sync.pull`, `self-test`) не блокирует писателя;
  это то, чего не хватало при единственном соединении и `VACUUM INTO` (C5/H6).
* `busy_timeout=5000` — **на каждом соединении** (`SCHEMA_SQL` выполняется в
  `Store::finish_open`, а не «где-то при инициализации»). H6 — это ровно
  «busy_timeout поставили на одном соединении из трёх»; без таймаута мгновенный
  `SQLITE_BUSY` уходит наверх и (в C++ версии) оставлял транзакцию открытой (C4).
* `synchronous=NORMAL` — **сознательное отклонение** от § 10.2 `architecture.md`,
  где написано `FULL`. Под WAL `NORMAL` теряет только хвост WAL при пропаже
  питания, база при этом остаётся согласованной, а коммит атомарным. Пассивный
  путь — это много мелких записей; `FULL` превращает каждое зафиксированное
  удаление в fsync диска. Потеря последней незафлашенной пачки допустима: клиент
  её повторит, и именно для этого существует `content_hash`-идемпотентность.
  `self-test` принимает `1` (NORMAL) и `2` (FULL), но не `0` (OFF).
* `foreign_keys=ON` — иначе объявленные `FOREIGN KEY` декоративны.
* `auto_vacuum=INCREMENTAL` — `purge_by_size` возвращает страницы без полного
  `VACUUM` (который требует эксклюзивной блокировки и двойного места).

### 2.3 Миграции (`v1 → v2 → v3`)

`Store::open` всегда: (1) выполняет текущий `SCHEMA_SQL`, (2) выполняет
`SCHEMA_V2_SQL` (`CREATE ... IF NOT EXISTS`), (3) выполняет `SCHEMA_V3_SQL`
(`db_cursor`), (4) зовёт `store::migrate::run`, который **в одной транзакции**
добавляет недостающие колонки (`accounts.conflict_high_water`), создаёт
недостающие таблицы и поднимает `schema_meta.schema_version` до 3.

* идемпотентность проверяется по фактическим объектам (`PRAGMA table_info`,
  `sqlite_master`), а не по номеру: «DDL применён, версия не поднята» лечится
  следующим открытием;
* файл с версией **новее** — `Error::Storage("... newer than this agent")`, а не
  «попробуем открыть»: новый шаг мог добавить `NOT NULL`-колонку, которую эта
  сборка не заполнит;
* данные не переписываются: только `ALTER TABLE ADD COLUMN ... DEFAULT 0` и
  `CREATE TABLE IF NOT EXISTS`;
* `SCHEMA_V1_SQL` заморожен в `store/schema.rs` — это фикстура теста «старая БД →
  новая схема → данные целы», а не второй источник правды.

Шаги в `store::migrate::STEPS`: `1 → 2 "conflicts-and-conflict-counter"`,
`2 → 3 "db-cursor"`. Третий — курсор серверного ingest из общей БД
(`docs/db-ingest.md` § 4): `db_cursor(account_id, source, last_key, updated_at)`,
`last_key` монотонен в SQL и пишется в одной транзакции с `ops`.

Тесты: `store::migrate::tests::a_v1_file_is_upgraded_with_its_data_intact`,
`…::migrating_twice_changes_nothing`,
`…::an_interrupted_migration_is_repaired_by_the_next_open`,
`…::a_file_from_the_future_is_refused_not_guessed`,
`…::a_missing_version_row_is_repaired_by_the_schema_batch`,
`…::a_connection_without_the_marker_refuses_to_guess`,
`…::the_step_list_reaches_the_current_version`,
`…::a_v2_file_gains_the_db_cursor_and_keeps_its_data`,
`…::the_db_cursor_step_is_idempotent`,
`…::an_interrupted_v3_migration_is_repaired_by_the_next_open`,
`…::the_step_list_covers_db_cursor`. В `--self-test` это проверка
`store.migrate`.

---

## 3. Формат событий

```rust
Event {
    account_id:  Option<AccountId>,  // 16 байт; None/нули отбрасываются
    device_id:   String,             // [A-Za-z0-9._-], 1..=64
    kind:        Option<EventKind>,
    entity_id:   String,             // "message:<dialog>:<msg>" | "presence:<peer>" | ...
    field:       Option<String>,     // имя поля из закрытого набора для kind
    value:       Vec<u8>,            // непрозрачные байты (на клиенте — шифртекст)
    hlc:         Hlc,                // 8 байт BE physical_ms + 2 байта BE logical
    content_hash: [u8; 32],          // sha256, ключ дедупа
}
```

`Event` — это *вход*, а не строка БД: `account_id` и `kind` — `Option`, чтобы
«клиент забыл» было представимо и отбрасываемо, а не превращалось в аккаунт
`00…00`.

### 3.1 Виды событий

| `kind` | Код | `entity_id` | `field` | `value` |
|---|---|---|---|---|
| `MessageNew` | 1 | `message:<dialog_id>:<message_id>` | обязателен, из `MESSAGE_FIELDS` | байты поля (на клиенте — AEAD-конверт) |
| `MessageEdit` | 2 | то же | обязателен | то же |
| `MessageDelete` | 3 | то же | **запрещён** | пусто (или метаданные) |
| `PresenceOnline` | 4 | `presence:<peer_id>` | запрещён | ровно 8 байт BE: `online_till` (0 = неизвестно) |
| `PresenceOffline` | 5 | `presence:<peer_id>` | запрещён | ровно 8 байт BE: `last_seen` (0 = неизвестно) |
| `ProfileUpdate` | 6 | `profile:<peer_id>` | обязателен, из `PROFILE_FIELDS` | байты значения (≤ 4 КиБ) |
| `ArchiveObservation` | 7 | `archive:<peer_id>` | обязателен, из `ARCHIVE_SOURCES` | непрозрачный payload (≤ 8 КиБ) |

Закрытые наборы (опечатка становится `Rejected`, а не второй историей поля):

* `MESSAGE_FIELDS` (18): `text, entities, edit_date, views, media, media_path,
  mime_type, document, thumbs, attributes, reply, reply_markup, post_author,
  fwd_name, fwd_post_author, grouped_id, topic_id, flags`
* `PROFILE_FIELDS` (8): `first_name, last_name, username, bio, userpic_path,
  chat_title, self_in_chat, still_in_chat`
* `ARCHIVE_SOURCES` (5): `new_message, forward_author, participant,
  opened_chat, profile_update`

Лимиты: `value` ≤ 256 КиБ (сообщение), ≤ 4 КиБ (профиль), ≤ 8 КиБ (архив);
`entity_id` ≤ 128 байт; батч ≤ 2000 событий (`MAX_BATCH_EVENTS`).

### 3.2 `content_hash`

```
sha256( "tf/passive/event/v1\0" || kind_code ||
        len(entity_id) u32 BE || entity_id ||
        field_tag(0|1) [|| len(field) u32 BE || field] ||
        hlc[10] || len(value) u64 BE || value )
```

Считает клиент (только он знает, «одно наблюдение отправлено дважды» или «один и
тот же текст снят двумя устройствами в разные моменты»); `Event::with_computed_hash()`
даёт ту же функцию локально. Агент использует хеш **только** для идемпотентности
и не выводит из него никакой семантики, поэтому клиент, присылающий константу,
ломает только собственный ingest.

### 3.3 Порядок проверок и причины отказа

1. `account_id` есть → иначе `MissingAccount`;
2. `account_id` совпадает с аутентифицированным → иначе `ForeignAccount`;
3. `device_id` корректен → `MalformedDevice`;
4. `hlc != 0` → `MissingHlc`; `content_hash != 0` → `MissingContentHash`;
5. `kind` есть → `MissingKind`; `entity_id` разбирается → `MalformedEntityId`;
   префикс соответствует `kind` → `KindEntityMismatch`;
6. `field`/`value` по таблице выше → `MissingField`, `UnknownField`,
   `UnexpectedField`, `OversizedValue`, `MalformedPresencePayload`;
7. дубликат `content_hash` (в хранилище или в этом же батче) → `Duplicate`;
8. HLC не больше последнего от этого устройства → `NonMonotonicHlc`;
9. правка/новое сообщение с HLC ≤ HLC последнего удаления → `Tombstoned`.

`IngestResult` — на каждое событие, в порядке входа:

```rust
Applied   { op_seq: u64 }
Duplicate { op_seq: u64 }   // op_seq оригинала
Rejected  { reason: RejectReason }
```

`IngestOutcome` добавляет счётчики и `high_water`. `to_json()` —
`{"applied":N,"duplicates":N,"rejected":N,"high_water":N}`.

Отказ одного события **не** отменяет батч (это вердикт по событию); ошибка
хранилища отменяет весь батч: частично применённый батч выдал бы `op_seq` для
строк, которых у повтора не окажется.

---

## 4. Гарантии

### 4.1 Идемпотентность

Двойная отправка того же батча → `applied +0`, `rows +0`, все события
`Duplicate` с исходными `op_seq`. Обеспечена `UNIQUE (account_id, content_hash)`
плюс дедуп внутри батча. Тест:
`store::ingest::tests::replaying_the_same_batch_adds_no_rows`,
`store::query::tests::duplicate_verdicts_do_not_grow_the_history`.

### 4.2 Порядок

`op_seq` выдаётся `UPDATE accounts SET high_water = high_water + 1 … RETURNING`
в **той же** транзакции, что и вставка, поэтому «последовательность выдана,
строки нет» невозможно. Значение не переиспользуется: счётчик хранимый, а не
`rowid`. Тесты:
`store::conn::tests::op_seq_is_monotone_per_account_and_independent_across_accounts`,
`store::ingest::tests::op_seq_is_monotone_across_batches`.

Внутри одного устройства порядок дополнительно защищён монотонностью HLC:
событие с HLC ≤ предыдущего от того же `device_id` отклоняется. Устройства
независимы. Тесты:
`store::ingest::tests::a_device_that_goes_backwards_is_rejected`,
`…::two_devices_may_each_have_their_own_clock`.

### 4.3 Изоляция аккаунтов

Три уровня: `account_id` в каждой таблице, `FOREIGN KEY` на `accounts`,
и проверка области в `ingest` (событие для чужого аккаунта — `Rejected`).
Тесты: `store::ingest::tests::an_event_for_another_account_is_rejected_not_stored`,
`store::schema::tests::every_table_that_holds_account_data_declares_account_id`,
`store::conn::tests::a_foreign_key_violation_is_reported_not_silently_dropped`.

### 4.4 Без LWW

`ops` — append-only. Новое сообщение, две правки и удаление — четыре строки;
`entities.revision_count = 4`. «Текущее» значение профиля — запрос
`profile_current` (по максимальному `op_seq` на поле), а не колонка, которую
кто-то перезаписывает. Тесты:
`store::ingest::tests::history_is_append_only_never_last_write_wins`,
`store::query::tests::profile_current_returns_the_newest_value_per_field`.

Удаление — tombstone: правка с HLC ≤ HLC удаления отклоняется
(`Tombstoned`), поэтому «воскрешения» удалённого сообщения нет; при этом
*реально более новая* ревизия записывается, и проекция `deleted` снимается
(последнее слово за самым новым событием). Тесты:
`…::a_revision_older_than_the_delete_cannot_resurrect_the_message`,
`…::a_genuinely_newer_revision_after_a_delete_is_recorded`.

### 4.5 Транзакционная дисциплина (C4)

Единственный способ писать — `Store::transaction(|tx| …)`. `Tx::Drop` делает
`ROLLBACK`, если `commit()` не был вызван, поэтому `?` внутри блока физически не
может оставить соединение в открытой транзакции (в C++ это была Critical-находка
C4: `addEditedMessage`/`addDeletedMessage` в `catch` только логировали).
Тесты: `store::conn::tests::a_failed_transaction_rolls_back_and_the_connection_stays_usable`,
`…::a_panicking_transaction_still_rolls_back`, а также живая проверка в
`--self-test` (`store.txn`).

### 4.6 Ретенция

* `purge_by_age(cutoff_ms)` — удаляет `ops` по `applied_at_ms`, проекции по
  `observed_ms`. Возвращает `PurgeReport` по таблицам.
* `purge_by_size(max_ops_per_account, max_bytes)` — сначала ограничение по числу
  строк на аккаунт (граница по `op_seq`, а не по времени: `op_seq` уникален и
  монотонен, поэтому «последние N» однозначны даже при одинаковой миллисекунде),
  затем обрезка по бюджету байт: выбираются самые старые строки, покрывающие
  превышение, и удаляются вместе с проекциями (через `TEMP TABLE purge_keys`).
* `RetentionPolicy { max_age_ms, max_ops_per_account, max_bytes }` + `apply()`.
  `RetentionPolicy::default_for_vps()` — 90 дней.
* `prune_upto(account, upto_op_seq)` — ретенция по требованию клиента: удаляет
  журнал и проекции до указанного `op_seq`. `upto_op_seq > high_water` — это
  `Error::Conflict` (HTTP 409), а не «удалить всё»: агент не выдавал такой номер.
  `high_water` не сдвигается, а граница пишется в `schema_meta`
  (`pruned_upto:<account>`), чтобы `sync.pull` мог отличить «не было» от
  «удалено» и ответить явным `409` на устаревший курсор.
  Тесты: `store::retention::tests::prune_*`.
* Планировщик: `serve` поднимает поток `tf-retention`, который раз в
  `retention_interval_secs` (по умолчанию 3600) проходит по всем тенантам,
  логирует отчёт и **не роняет процесс** при ошибке одного тенанта; по сигналу
  останавливается вместе с listener'ом (`src/api/server.rs`,
  `src/api/signals.rs`).
* Ретенция **не** уменьшает `entities.revision_count` и **не** двигает
  `accounts.high_water`: оба монотонны, иначе курсор, который держит клиент,
  перестал бы быть валидным. Тест:
  `store::retention::tests::retention_never_rewrites_the_monotone_counters`.

Оценка размера: `SUM(LENGTH(value)) + 64 * COUNT(*)` по четырём таблицам
(`estimated_bytes`). Точность не нужна — важно, что ручка двигается в нужную
сторону и что сохраняются самые новые строки.

### 4.7 Граница шифрования

`value` — непрозрачные байты: AEAD-конверт на ключе
`subkey(DEK, LABEL_SYNC_DATA)`. Конверт создаёт **кто-то один**, и это зависит от
пути:

* **клиент → `sync.push`** — конверт создаёт клиент, агент упорядочивает записи,
  не читая их;
* **общая БД → `sync.pull`** — конверт создаёт серверный ingest
  (`db::ingest::seal_row`): он читает plaintext БД, запечатывает его тем же
  подключом и кладёт в `ops`. AAD — адрес строки (`entity_id`, `field`, `kind`,
  домен `tf/db-ingest/v1`), поэтому конверт нельзя перенести на другое поле или
  сущность.

То есть на пути «БД → журнал» агент держит DEK и видит открытые значения — это
меняет § 7.6 `architecture.md` и подтверждено заказчиком (`docs/transport.md`
§ 1.3, `docs/db-ingest.md` § 6). Метаданные (`entity_id`, `field`, `kind`, HLC,
`content_hash`) открыты — без них упорядочивание невозможно; это то же решение,
что в § 10.2 `architecture.md`.

**Исключение — присутствие.** `presence_online`/`presence_offline` хранят
`value` как один big-endian `i64` (так его валидирует `store::ingest` и
проецирует в `presence.detail_ms`), поэтому запечатать его нельзя: AEAD-конверт
длиннее 8 байт, и событие было бы отклонено как `malformed_presence_payload`.
Присутствие идёт открытым 8-байтным payload'ом — ровно так же, как в клиентском
`sync.push`: там timestamp, а не текст сообщения.

### 4.8 Конфликты без векторных часов

Событие несёт HLC и `device_id`, но не vclock, поэтому правило сформулировано
так, чтобы его можно было проверить и объяснить:

> Конфликт — это принятая запись, которую HLC-упорядоченный мерж **выбросил бы**:
> по тому же `(entity_id, field)` уже есть запись **другого устройства** со
> строго большим HLC.

Запись при этом применяется и остаётся в журнале (append-only), а пара
`local/remote` пишется в `conflicts` (v2). `sync.push` возвращает
`conflicts`/`conflict_ids`, `sync.conflicts` отдаёт список и принимает
`resolve`; агент **никогда** не выбирает победителя — это H7.

Одно устройство, правящее своё поле, конфликта не даёт (его HLC монотонен);
более новый HLC от другого устройства — тоже (это обычный мерж).

Тесты: `store::ingest::tests::a_newer_write_from_another_device_is_kept_and_recorded_as_a_conflict`,
`…::a_newer_write_from_another_device_is_not_a_conflict`,
`…::conflicts_are_per_field_and_per_entity`,
`…::a_replayed_batch_does_not_duplicate_its_conflicts`,
`store::query::tests::conflicts_are_listed_resolved_and_paged`.

---

## 5. Хранилище секретов (закрывает C1)

### 5.1 Что было в C++

`MasterMaterial() = userId + phone`, `DeriveSyncKey` = один `SHA-256` (функция
называлась `HkdfSha256`, но HKDF не вызывала: ни соли, ни итераций, ни секрета).
Любой, кто знал пару «id + телефон», выводил ключ к дампу.

### 5.2 Схема

```
salt   = 32 случайных байта, хранятся рядом с конвертом
KEK    = Argon2id(passphrase, salt, m_cost, t_cost, p_cost)      // 32 байта
DEK    = 32 случайных байта, генерируются один раз на аккаунт
wrap   = XChaCha20-Poly1305(KEK, aad = account_id || key_id || b'd', DEK)
check  = XChaCha20-Poly1305(KEK, aad = account_id || key_id || b'v',
                            b"teleforge-keystore-verifier-v1")
```

* Соль случайная и хранится: два аккаунта с одной парольной фразой получают
  разные ключи, предвычислить нечего.
* `Argon2id` с полом стоимости (`KdfCost::MIN_M_COST_KIB = 32768` КиБ,
  `MIN_T_COST = 2`, `p_cost ≥ 1`): заниженные параметры не проходят `validate()`
  ни при создании, ни при разборе конверта, ни при unlock. Дефолт для сервера —
  64 МиБ / 3 прохода (не 256 МиБ: агент живёт рядом с TDLib и SQLite на VPS).
* DEK случайный, а не выведенный: смена пароля — это пере-wrap 32 байт, без
  пере-шифрования данных и без старого открытого текста.
* Верификатор — **AEAD-константа**, а не хеш KEK: неверная фраза проваливает
  тег, а не сравнивается с сохранённым дайджестом (дайджест дал бы офлайн-оракул
  поверх Argon2id).
* AAD — ровно `account_id || key_id` из дизайна, плюс один завершающий байт
  назначения. Без него `wrap` и `check` были бы взаимозаменяемы: тот, кто может
  писать в каталог, подставил бы 34 известных байта верификатора в слот DEK.
  Тест `keystore::tests::a_verifier_moved_into_the_dek_slot_does_not_unwrap_a_key`
  это фиксирует.
* `key_id` версионируется: `rotate` (смена пароля) увеличивает его и меняет соль,
  DEK остаётся тем же.

### 5.3 Трейты и реализации

| Трейт | Реальная | Stub (только тесты) |
|---|---|---|
| `Kdf` | `Argon2Kdf` | `StubKdf` — детерминированная `SHA-256`-цепочка |
| `Aead` | `PrimitiveAead` (XChaCha20-Poly1305, AES-256-GCM для чтения) | `StubAead` — XOR-keystream + усечённый `SHA-256` тег, формат заголовка настоящий |

Stub-реализации:
* содержат явные `debug_assert!` (в том числе `!forbidden_in_release()`);
* **в релизной сборке возвращают ошибку** (`Error::Unsupported`), а не «защищают
  ничего»;
* `StubAead` несовместим с `PrimitiveAead` в обе стороны — тестовый конверт
  fail-closed против реального примитива
  (`keystore::tests::stub_aead_output_is_not_readable_by_the_real_aead`).

### 5.4 Файлы, права, память, логи

* Путь: `<dir>/accounts/<account_hex>.envelope.json`, запись через
  `fsperm::write_private_file_replace` (временный файл → `rename` → fsync
  каталога), режим `0600`.
* На Linux `0600` ставится и проверяется (`fsperm::posix_modes_enforced()`).
  На Windows режима нет; `fsperm` выставляет эквивалентный DACL (`icacls`
  `/inheritance:r /grant:r <user>:(OI)(CI)F`) и **честно сообщает**, что
  верифицировать его нельзя: `--self-test` печатает `warn`, а не «0700
  проверено».
* Ключи в памяти: `SecretBytes` (нет `Clone`, `Drop` зануляет через
  `write_volatile`, `Display` отсутствует, `Debug` — `<redacted, N bytes>`,
  `expose()` называется так, чтобы `grep` находил все места). В KDF стековый
  массив зануляется `zeroize` до возврата.
* Логи: `Debug` для `Envelope`/`UnlockedSecret`/`SecretStore` печатает длины и
  `<redacted>`; тест
  `keystore::tests::debug_output_never_contains_the_passphrase_or_the_dek`
  проверяет, что ни парольная фраза, ни hex DEK не попадают в отладочный вывод.
* `--tenant`-независимо: конверт привязан к аккаунту, копирование файла в чужой
  слот ловится проверкой владельца, а подмена поля `account_id` — тегом AEAD
  (`keystore::tests::an_envelope_cannot_be_moved_to_another_account`).

### 5.6 Recovery-код для account-level конверта

`vault` (tenant-level) умел восстановление по коду с самого начала; теперь то же
есть у конверта аккаунта:

```
code   = 16 случайных байт, показываются один раз
KEK_r  = Argon2id(code, salt' = sha256(salt || LABEL_RECOVERY), cost)
wrap_r = XChaCha20-Poly1305(KEK_r, aad = account_id || key_id || b'r', DEK)
```

| Вызов | Результат |
|---|---|
| `SecretStore::create_with_recovery(...)` | `(Envelope, UnlockedSecret, code)` — код показать один раз |
| `SecretStore::unlock_with_recovery(account, code, envelope)` | DEK; неверный код → `Error::Auth("wrong recovery code")` |
| `Envelope::has_recovery()` | есть ли вторая обёртка (клиенту нужно решить, предлагать ли «ввести код») |
| `rotate` при наличии recovery | `Error::Unsupported`: пере-обернуть recovery без самого кода нельзя, и молча потерять эту дверь — хуже, чем отказать |

Байт назначения `b'r'` в AAD не даёт перенести recovery-обёртку в слот
`wrapped_dek` (и наоборот): обе обёрнуты одним и тем же DEK, и без тега слота
подмена прошла бы. Тест
`keystore::tests::a_recovery_wrap_cannot_be_moved_into_the_passphrase_slot`.

Recovery-обёртка **всегда** использует настоящий `PrimitiveAead` и настоящий
Argon2id, даже если тест подставил stub-KDF/AEAD для основного пути: в продакшене
stub-recovery не существует, а «работает в тесте, падает у пользователя, который
потерял пароль» — худший вариант.

В `--self-test` это проверка `keystore.recovery` (создать конверт с кодом,
открыть верным, убедиться, что неверный отвергнут).

Пути и назначение переменных совпадают с § 7.2/§ 8.1 `architecture.md`
(tenant-level `vault` использует AAD `tenant_id || key_id` и живёт в
`vault.rs`; account-level `keystore` — AAD `account_id || key_id || purpose`).

---

## 6. `--self-test`

```bash
teleforge-agent self-test --data-dir /var/lib/teleforge-agent \
                          [--tenant <hex>]
```

Печатает **один** JSON-объект в stdout и ничего больше:

```json
{"agent_version":"0.1.0","protocol":1,"ok":true,"exit_code":0,
 "failures":[],
 "checks":[{"name":"permissions","status":"warn","detail":"…"},
           {"name":"store.schema","status":"pass","detail":"… is schema 2"},
           {"name":"store.migrate","status":"pass","detail":"schema 2 objects are present and the version row agrees"},
           {"name":"store.txn","status":"pass","detail":"ROLLBACK leaves the connection usable"},
           {"name":"keystore.kdf","status":"pass","detail":"argon2id floor cost derives deterministically"},
           {"name":"keystore.aead","status":"pass","detail":"xchacha20-poly1305 seals, opens, and rejects a wrong key"},
           {"name":"keystore.recovery","status":"pass","detail":"a recovery code unwraps the DEK and a wrong code is refused"},
           {"name":"fsperm","status":"warn","detail":"…"},
           {"name":"tdlib","status":"pass","detail":"minimum version constant is self-consistent"},
           {"name":"release-root-key","status":"warn","detail":"… (development build)"}]}
```

| Проверка | Что ловит |
|---|---|
| `permissions` | каталог данных доступен группе/остальным (M14: соль рядом с шифртекстом). Отсутствующий каталог — `warn`, не `fail` |
| `store.schema` | отсутствующая таблица/индекс, `journal_mode` не WAL, `synchronous=0`, `busy_timeout=0` (H6), несовпадение `schema_version` |
| `store.migrate` | `schema_version` не совпадает, отсутствует объект v2 (`conflicts`, `accounts.conflict_high_water`) — иначе `sync.conflicts` молча отвечал бы «конфликтов нет» |
| `store.txn` | соединение не может выполнить `BEGIN IMMEDIATE`/`ROLLBACK` или остаётся в транзакции после ошибки (C4) |
| `keystore.recovery` | recovery-обёртка не открывается своим кодом, открывается чужим или возвращает другой DEK |
| `keystore.kdf` | Argon2id отсутствует/заглушка/недетерминирован (иначе C1 возвращается молча) |
| `keystore.aead` | AEAD не аутентифицирует (принимает неверный ключ) |
| `fsperm` | платформа, где `0600` нельзя ни применить, ни проверить (`warn`) |
| `tdlib` | самосогласованность константы минимальной версии |
| `release-root-key` | релизный бинарь без вкомпилированного ключа (`fail` в релизе, `warn` в dev) |

Без `--tenant` проверки БД идут на in-memory хранилище (схема грузится, прагмы
кроме WAL проверяются, транзакция проверяется); с `--tenant` — на реальном файле
`<data_dir>/tenants/<tenant>/sync/data.db`.

Коды выхода: `0` — всё `pass`/`warn`, `7` (`SELF_TEST_FAILED`) — есть `fail`,
`5` (`CONFIG`) — не удалось разобрать конфигурацию до запуска проверок.

---

## 7. Как проверять

```bash
cd rust/teleforge-agent

cargo fmt --check
cargo clippy --all-targets -- -D warnings
cargo test                       # 412 тестов
cargo run --bin teleforge-agent -- self-test --data-dir /tmp/tf-self-test
```

Что доказывает каждая группа тестов:

| Свойство | Тесты |
|---|---|
| схема: аккаунт везде, `op_seq` на аккаунт, дедуп-хеш, прагмы | `store::schema::tests::*` (6) |
| транзакция с `ROLLBACK`, отсутствие «залипшей» транзакции | `store::conn::tests::a_failed_transaction_rolls_back…`, `a_panicking_transaction_still_rolls_back` |
| монотонность `op_seq` | `store::conn::tests::op_seq_is_monotone…`, `store::ingest::tests::op_seq_is_monotone_across_batches` |
| идемпотентность (двойная отправка → 0 новых строк) | `store::ingest::tests::replaying_the_same_batch_adds_no_rows`, `…one_event_repeated_inside_a_batch_applies_once` |
| отказ на чужом `account_id` и на событии без аккаунта | `store::ingest::tests::an_event_for_another_account_is_rejected_not_stored`, `…an_event_without_an_account_is_rejected` |
| HLC: назад нельзя, разные устройства независимы | `store::ingest::tests::a_device_that_goes_backwards_is_rejected`, `…two_devices_may_each_have_their_own_clock` |
| без LWW: new + edit + edit + delete = 4 строки | `store::ingest::tests::history_is_append_only_never_last_write_wins` |
| tombstone не воскрешает, но новая ревизия пишется | `store::ingest::tests::a_revision_older_than_the_delete…`, `…a_genuinely_newer_revision_after_a_delete_is_recorded` |
| истории присутствия/профиля, производное «текущее» | `store::query::tests::profile_current_returns_the_newest_value_per_field`, `store::ingest::tests::presence_history_keeps_every_transition` |
| ретенция по возрасту и по размеру | `store::retention::tests::*` (12) |
| `self_check` ловит поломки хранилища | `store::query::tests::self_check_*` |
| конверт: roundtrip, неверная фраза, чужая копия, rotate, права, `Debug` без секретов | `keystore::tests::*` |
| recovery-код: верный открывает DEK, неверный отвергнут, слоты не взаимозаменяемы, `rotate` не теряет обёртку | `keystore::tests::a_recovery_*`, `…::an_account_without_a_recovery_envelope_says_so`, `…::rotating_refuses_to_silently_drop_the_recovery_wrap` |
| примитивы: Argon2id детерминирован по соли, HKDF разделяет метки, AEAD не принимает чужой ключ/AAD | `crypto::kdf::tests::*`, `crypto::aead::tests::*` |
| миграции: v1 → v2 → v3 с сохранением данных, идемпотентность, отказ на «будущую» версию | `store::migrate::tests::*` (12) |
| курсор чтения БД: монотонность (назад нельзя), изоляция по `(account, source)`, регистрация аккаунта, отказ на битом `source` | `store::cursor::tests::*` (8) |
| серверный ingest: полный цикл «курсор → батч → запечатано → `ops` → `sync.pull`», перезапуск, ошибка БД, запертый vault, битая строка, размер страницы, health | `db::ingest::tests::*` (15), `api::server::tests::the_full_server_side_cycle_reaches_sync_pull` |
| allowlist: запрет `personality_core`/`openai_api_key`/`pgConnString`, default-deny, SQL только из allowlist, диалекты | `db::allowlist::tests::*` (7) |
| строка подключения: разбор, `verify-full` по умолчанию, отказ на `disable`, пароль не попадает в `Debug`/лог/ошибку | `db::conninfo::tests::*` (10) |
| vault-слот кредов БД: roundtrip, на диске шифртекст, чужой DEK/чужой тенант/чужой label, `0600` | `db::credentials::tests::*` (8) |
| `db.status` по сети: 200 с полями § 5.8, 401 без токена, `vault: locked` не ошибка, читатель не запускает ingest | `api::server::tests::db_status_*`, `…::a_reader_token_cannot_start_the_ingest`, `…::an_ingest_token_can_start_the_ingest` |
| роли токена: reader не несёт `db:ingest`, старый файл читается как reader, подделанный файл отвергнут | `auth::tests::a_reader_token_cannot_hold_the_ingest_scope`, `…::the_role_defaults_to_reader…`, `…::a_hand_edited_file_cannot_promote_a_reader_to_ingest` |
| конфликты: запись сохраняется, пара видна, повтор батча не дублирует конфликт | `store::ingest::tests::*`, `store::query::tests::conflicts_are_listed_resolved_and_paged` |
| ретенция по расписанию: пурж, лог, процесс жив | `api::server::tests::retention_runs_and_logs_without_stopping_the_process` |
| HTTP: границы до чтения, статусы, заголовки | `api::http::tests::*` (10) |
| TLS: сборка конфига, отказ на битом ключе, пиннинг, `Debug` без ключа | `api::tls::tests::*` (4) |
| JSON-RPC: порядок проверок, 401/403/409/429/400, идемпотентность ключа | `api::rpc::tests::*` (9) |
| полный цикл `push → pull` по реальному TLS-listener'у, идемпотентность, чужой аккаунт, пагинация, `prune`, конфликты, лимиты | `api::server::tests::*` (23) |
| `SyncService`: push/pull/stamp/prune/конфликты, изоляция тенантов, ретенция | `api::sync::tests::*` (9) |
| auth: hash токена, привязка tenant/account, scopes, revoke, rate limit, replay | `auth::tests::*` (10) |
| base64: каноничность, отказ на нестрогом входе | `api::base64::tests::*` (5) |
| PEM: метка, первый блок, незакрытый блок | `api::pem::tests::*` (5) |
| HLC: wire-форма и её строгий разбор | `sync::clock::tests::the_wire_*` (2) |
| `content_hash`: golden vectors для C++-стороны | `store::event::tests::golden_vectors_for_the_client` |
| `--self-test` отчёт/код выхода | `selftest::tests::*` (8) |
| CLI: `--tenant` для `self-test`/`pair`, `serve --help`, лимиты, `--ingest-*`, `--role` | `cli::tests::*` (14) |

---

## 8. Что не сделано и почему

| Не сделано | Почему / где |
|---|---|
| Приём событий от реального C++ клиента (`Agent::ShadowIngest`) | Шаг 3, клиентская половина: спецификация готова ([`client-integration.md`](client-integration.md)), но правки в `ayu/features/sync/**` под `TELEFORGE_WITH_AGENT` в этой задаче запрещены |
| `unix-socket` listener и `listen = network` + mTLS | `bind` возвращает `NotImplemented`, а не тихо переключается на TCP: фолбэк был бы другой экспозицией |
| `pair`/`unlock`/`lock` по сети | Enrollment пока через CLI `teleforge-agent pair`; vault-проводка — шаг 2 |
| `tunnel.*`, `account.*`, `update.check` | Шаги 1/2/6; методы объявлены и отвечают `501` |
| `mlock`/`MADV_DONTDUMP`, `RLIMIT_CORE`, `PR_SET_DUMPABLE` | Шаг 2; `SecretBytes::lock_pages()` честно возвращает `NotImplemented`, тест это фиксирует |
| ~~Серверное шифрование `value` (ingest из БД)~~ | **сделано**: `db::ingest::seal_row` запечатывает строки БД подключом `subkey(DEK, LABEL_SYNC_DATA)` (§ 4.7, `docs/db-ingest.md` § 1). Открытым остаётся отсутствие проводки `unlock` в `serve` |
| ~~Чтение общей БД (server-side ingest, `db.*`, курсор)~~ | **сделано** в объёме `sync_outbox`: `src/db/**`, курсор v3, `db.status`, поток `tf-db-ingest`. Не сделано отображение остальных таблиц § 2.6 (`docs/db-ingest.md` § 3.2, § 7) |
| Подпись Ed25519 на запрос (вместо bearer) | `crypto::sig` — заглушка (шаг `updater`); `ReplayWindow` уже написан как задел, обоснование — `transport.md` § 2.1 |
| Проверка прав на Windows | Нет POSIX-режимов; DACL выставляется `icacls`, но верифицировать его нельзя — `self-test` печатает `warn`, production-таргет Linux |
| `recovery-rotate` (перевыпуск кода) | Требует UX «показать новый код один раз»; сейчас `rotate` при наличии recovery честно отказывает |
| Удаление аккаунта целиком (GDPR-подобное) | Не определено в дизайне; `purge_*` покрывает данные, но не реестр |
| Импорт старых `TFSYNC1`-бандлов | Шаг 4; чтение AES-256-GCM в `crypto::aead` уже есть |
