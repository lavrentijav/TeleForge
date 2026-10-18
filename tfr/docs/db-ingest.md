# Серверный ingest из общей БД (pull-first)

Как агент читает PostgreSQL/MySQL пользователя, шифрует строки и публикует их в
журнал `ops`, из которого клиент тянет через `sync.pull`.

Код: `src/db/**` (`mod.rs`, `conninfo.rs`, `allowlist.rs`, `credentials.rs`,
`postgres.rs`, `mysql.rs`, `ingest.rs`, `mock.rs`), `src/store/cursor.rs`,
схема v3 в `src/store/schema.rs` + `src/store/migrate.rs`.

---

## 1. Что происходит

```
[общая БД на сервере: PostgreSQL / MySQL]        ← источник истины
        │  SELECT <allowlisted columns> FROM <table>
        │    WHERE <key> > cursor ORDER BY <key> ASC LIMIT n
        ▼
[teleforge-agent, поток tf-db-ingest]
        │  value → AEAD(subkey(DEK, LABEL_SYNC_DATA), aad = адрес строки)
        │  одна SQLite-транзакция: ops + db_cursor
        ├──► sync.pull ──► C++ клиент (только чтение)
        └──► db.status ──► наблюдаемость
```

Один проход (`db::ingest::DbIngest::run_pass`):

| Шаг | Что делает | Если не получилось |
|---|---|---|
| 1 | берёт `subkey(DEK, LABEL_SYNC_DATA)` и `subkey(DEK, LABEL_DB_CONN)` | `Error::Auth`, в лог `class=vault_locked`, курсор не трогаем |
| 2 | читает `db_cursor(account, source)` | `Error::Storage`, проход не начинается |
| 3 | лениво подключается: vault → `ConnFacts` → драйвер | `Error::Database(class)`, класс в `db.status.last_error` |
| 4 | `next_batch(cursor, limit)` | то же; соединение сбрасывается, следующий проход переподключится |
| 5 | запечатывает значения, считает `content_hash` строки | `class=seal`, ничего не записано |
| 6 | одна транзакция: `ops` + сдвиг `db_cursor` | откат целиком, курсор на месте |
| 7 | обновляет health (`last_ok_at`, `lag_ms`, счётчики) | — |

Строки журнала получают `device_id = "server-db"` (`db::SERVER_DEVICE_ID`) и
свой HLC, который монотонен на `(account, "server-db")` и **засевается из
`device_clock`** при каждом проходе: перезапуск не может выдать HLC, который
журнал уже видел.

---

## 2. Строка подключения: где лежит и как её положить

Пароль **не** в `agent.toml` и не в логах (finding M14: `pgConnString` рядом с
архивом). Строка лежит в vault-слоте тенанта:

```
<data_dir>/tenants/<tenant_hex>/db/credentials.enc     # 0600, каталог 0700
  = AEAD(subkey(DEK, LABEL_DB_CONN), aad = "tf/agent/v1/db-conn\0" || tenant, url)
```

* свой label (`LABEL_DB_CONN`), а не `LABEL_AT_REST`: конверт нельзя перенести
  из слота TDLib-логина в этот и обратно;
* AAD привязан к тенанту: `credentials.enc`, скопированный в каталог другого
  тенанта, не откроется;
* заменяется атомарно (`write_private_file_replace`), старых копий не остаётся.

API: `db::credentials::{store, load, load_with_key, remove, path}`. CLI-команды
`db-check` и `db-credentials set|rotate` пока нет (`docs/transport.md` § 12,
§ 15 п. 5): слот пишет установщик тем же SSH-каналом, что и `unlock`.

Формат строки — URL (key/value DSN вида `host=… password=…` **не** принимается,
чтобы «что здесь секрет» не было двусмысленным):

```text
postgresql://teleforge_agent:<пароль>@db.example.com:5432/teleforge?sslmode=verify-full&sslrootcert=/etc/ssl/pg-ca.pem
mysql://teleforge_agent:<пароль>@db.example.com:3306/teleforge?ssl-mode=verify-full&ssl-ca=/etc/ssl/mysql-ca.pem
```

| Параметр | Значение по умолчанию | Смысл |
|---|---|---|
| `sslmode` / `ssl-mode` / `ssl_mode` | `verify-full` | меньше `verify-full` → `warn` в лог (режим виден в `db.status.tls`); `disable`/`trust`/`off`/`none` → **отказ** на подключении (§ 2.6 п. 6) |
| `sslrootcert` / `ssl-ca` | нет (встроенный store Mozilla) | CA-бандл, PEM или DER |
| `connect_timeout` / `connect-timeout` | 5 с | ограничен 1..=300; нужен, потому что `db.status` подключается по требованию |

Что видно в `db.status`: `engine`, `database`, `reader_user`, `tls`. Пароля,
строки подключения и значений полей там нет и быть не может — в типе
`DbStatus` для них нет полей, а все ошибки несут только класс.

---

## 3. Какие таблицы и колонки читаются

Закрытый allowlist — `src/db/allowlist.rs`, а не соглашение. Всё, чего в нём
нет, отклоняется **до** построения SQL и до подключения (default-deny).

### 3.1 Читаемые сейчас

| Таблица | Колонки | Ключ курсора | Отображение |
|---|---|---|---|
| `sync_outbox` | `kind`, `entity_id`, `field`, `observed_ms`, `value` | `id` | `Outbox`: строка уже событие |

Именно этот shape рекомендует § 2.6 (`outbox-таблица, которую заполняет клиент
или триггер`), и только у него отображение не требует решения о моделировании.

DDL, который заполняет клиент (PostgreSQL; для MySQL — `BIGINT`, `BINARY(16)`,
`BLOB`):

```sql
CREATE TABLE sync_outbox (
    id          BIGSERIAL PRIMARY KEY,
    account_id  BYTEA   NOT NULL,
    kind        TEXT    NOT NULL,   -- "message_new" | "message_edit" | "message_delete"
                                    -- | "presence_online" | "presence_offline"
                                    -- | "profile_update" | "archive_observation"
    entity_id   TEXT    NOT NULL,   -- "message:<dialog>:<msg>" | "presence:<peer>" | …
    field       TEXT,               -- обязателен для message_new/edit, profile_update,
                                    -- archive_observation; запрещён для presence_*
    observed_ms BIGINT  NOT NULL,   -- время наблюдения, мс
    value       BYTEA   NOT NULL    -- открытые байты значения
);
CREATE INDEX sync_outbox_id_idx ON sync_outbox (id);
```

`id` должен быть монотонным и уникальным (`BIGSERIAL`/`AUTO_INCREMENT` или
отдельный счётчик). Одинаковые секунды в `updated_at` курсором быть не могут:
строки на границе страницы будут пропускаться.

### 3.2 Allowlisted, но пока без отображения

`messages`, `deleted_messages`, `edited_messages`, `online_events`,
`peer_archive_name`, `archive_observations` — колонки объявлены (по § 2.6), грант
можно готовить, но выбрать такую таблицу как источник ingest нельзя:
`class=unsupported_table`. Причина — § 7.

### 3.3 Запрещено всегда

Таблицы: `personality_core`, `account_secrets`, `config`, `settings`,
`pgconnstring`, `ssh_keys` — плюс любое имя, содержащее `personality`.
Колонки: `openai_api_key`, `openaiapikey`, `pgconnstring`, `password`,
`ssh_private_key`, `api_key` — сравнение регистронезависимое и по подстроке,
чтобы переименование в `openaiApiKey` не обходило проверку.

Отказ по такой таблице — `class=table_not_allowed`, до подключения к БД.

### 3.4 Гранты (§ 2.6)

```sql
CREATE ROLE teleforge_agent LOGIN PASSWORD '<из vault>';
GRANT CONNECT ON DATABASE teleforge TO teleforge_agent;
GRANT USAGE ON SCHEMA public TO teleforge_agent;

GRANT SELECT (id, kind, entity_id, field, observed_ms, value)
    ON sync_outbox TO teleforge_agent;
-- курсор публикации ведёт агент, в SQLite; в БД писать не нужно
```

`INSERT`/`UPDATE` в БД агенту не нужны вовсе: позиция чтения хранится в
локальном журнале (`db_cursor`), а не в таблице `sync_outbox`. Это меньше прав,
чем предполагает § 2.6 п. 3, и это осознанно.

Проверка прав выполняется на подключении: `SELECT 1`, затем
`SELECT <allowlisted columns> FROM <table> WHERE 1 = 0` (читает ноль строк, но
падает при отсутствии гранта на **колонку**). Результат — `db.status.tables_ok`.

---

## 4. Курсор чтения (схема v3)

```sql
CREATE TABLE db_cursor (
    account_id BLOB    NOT NULL,
    source     TEXT    NOT NULL,   -- таблица, к которой относится курсор
    last_key   INTEGER NOT NULL,
    updated_at INTEGER NOT NULL,
    PRIMARY KEY (account_id, source),
    FOREIGN KEY (account_id) REFERENCES accounts (account_id)
) WITHOUT ROWID;
```

Почему отдельная таблица, а не `device_clock`/`accounts.high_water`: те два
описывают устройства и журнал; перегрузка любого из них сделала бы один счётчик
прыгающим при движении другого и потеряла бы единственное нужное значение —
«какая строка какой таблицы уже опубликована». `source` в ключе, потому что
`messages.id = 4127` ничего не говорит про `online_events.id = 12`.

Свойства:

1. **Монотонность в SQL.** `advance_in_tx` делает upsert с
   `CASE WHEN excluded.last_key > db_cursor.last_key THEN excluded.last_key ELSE db_cursor.last_key END`
   и `RETURNING last_key`. Откат курсора назад невозможен даже при повторной
   попытке более старого батча.
2. **Атомарность с журналом.** `ops` и `db_cursor` пишутся в одной транзакции
   (`ingest_batch_in_tx` + `advance_in_tx`). Состояний «опубликовано, но курсор
   не сдвинут» (дубликат на следующем проходе) и «курсор сдвинут, записи
   потеряны» (дыра в журнале) не существует. Это правило C4, применённое в
   обратную сторону.
3. **Идемпотентность миграции.** v2→v3 — `CREATE TABLE IF NOT EXISTS` в одной
   транзакции с подъёмом `schema_version`; идемпотентность проверяется по факту
   (`sqlite_master`), а не по номеру версии, поэтому «DDL применён, версия не
   поднята» лечится следующим открытием.

Миграция: `store::migrate::STEPS` содержит `2 → 3 "db-cursor"`;
`Store::open` применяет `SCHEMA_V3_SQL` на каждом открытии. Тесты:
`store::migrate::tests::{a_v2_file_gains_the_db_cursor_and_keeps_its_data,
the_db_cursor_step_is_idempotent, an_interrupted_v3_migration_is_repaired_by_the_next_open,
the_step_list_covers_db_cursor}`, `store::cursor::tests::*`.

---

## 5. Как включить

Ingest выключен по умолчанию (`interval_secs = 0`), потому что включать
периодическое чтение БД без явной конфигурации — сюрприз на агенте, который
поставил старый клиент.

```bash
teleforge-agent serve \
    --data-dir /var/lib/teleforge-agent \
    --ingest-interval 30 \
    --ingest-tenant 00112233445566778899aabbccddeeff \
    --ingest-account ffeeddccbbaa99887766554433221100 \
    --ingest-table sync_outbox
```

| Флаг | Поле конфига | По умолчанию |
|---|---|---|
| `--ingest-interval <s>` | `db_ingest.interval_secs` | `0` (выключено) |
| `--ingest-tenant <hex>` | `db_ingest.tenant` | нет |
| `--ingest-account <hex>` | `db_ingest.account` | нет |
| `--ingest-table <name>` | `db_ingest.table` | `sync_outbox` |

`interval_secs > 0` без пары tenant+account — ошибка старта
(`AgentConfig::validate`), а не тихий no-op.

Поток `tf-db-ingest` (`spawn_db_ingest`): спит слайсами по 20 мс, поэтому
остановка мгновенная; ошибка прохода не роняет поток и не двигает курсор; в лог
уходит **только класс** (`class=connect`, `class=query`, …) и имя таблицы —
никогда значение, тело строки или cообщение драйвера.

Если vault заперт, проход возвращает `Error::Auth`, в лог идёт
`class=vault_locked`, `db.status` показывает `vault: "locked"`, а `sync.pull`
продолжает отдавать то, что уже в журнале (там шифртекст).

Health (доступен через `db.status`): `last_ok_at`, `lag_ms` (`now - last_ok_at`:
пока публикация стоит, лаг растёт), `fails`, `batches`, `applied`, `duplicates`,
`skipped`.

---

## 6. `db.status`

Контракт — `docs/transport.md` § 5.8. Ответ 200 всегда, даже когда БД
недоступна или vault заперт: клиент по нему решает, включать ли синхронизацию
(`client-integration.md` § 3.3).

```json
← {"result":{"connected":true,"latency_ms":3,"engine":"postgresql",
             "database":"teleforge","reader_user":"teleforge_agent",
             "tls":"verify-full",
             "cursor":{"table":"sync_outbox","column":"id","value":4127},
             "lag_ms":1250,"last_read_ms":1771000000000,
             "tables_ok":true,"vault":"unlocked","last_error":null,
             "last_ok_at":1771000000000,"fails":0,"batches":402,
             "applied":4127,"duplicates":0,"skipped":0}}
```

Двенадцать первых полей — § 5.8 дословно; `last_ok_at`, `fails`, `batches`,
`applied`, `duplicates`, `skipped` — health-счётчики (§ 5 задачи), аддитивные.

Доступ: `scope: sync:read` — тот же токен, что и `sync.pull`. Строку
подключения и креды метод не раскрывает: их нет в типе, а `last_error` — класс.

`connected` — результат `SELECT 1` на текущем соединении; `tables_ok` — грант на
все нужные колонки. «БД доступна, но прав недостаточно» — отдельное состояние
(§ 2.6 п. 7).

### Оговорка про `content_hash`

Для строки из БД хэш считается по **строке** (`db::ingest::row_content_hash`:
kind, entity, field, открытое значение, `observed_ms`), а не по HLC журнала:
иначе повторное чтение того же диапазона давало бы новый хэш и дубликат в `ops`
(§ 7). Хэшируется открытое значение, а не шифртекст (у AEAD случайный nonce).
Следствие: у того, кто украл `ops` без DEK, но знает содержимое БД, появляется
оракул «было ли это значение в БД» для догадок. Это тот же компромисс, что уже
есть у клиентского `content_hash` для того, кто знает plaintext; отмечен здесь,
чтобы его не открывали заново.

---

## 7. Что не реализовано и почему

| Не сделано | Почему / что нужно |
|---|---|
| Отображение `messages`/`deleted_messages`/`edited_messages`/`online_events`/`profile`/`archive` в события | Решение о модели: одно событие на колонку или одно на строку; какие имена `field` попадут в журнал; как считать `content_hash` (§ 12.9 `client-integration.md`). Таблица и колонки в allowlist уже объявлены — не хватает только отображения; выбирать такую таблицу сейчас → `class=unsupported_table` |
| `teleforge-agent db-check` | Нужен CLI-путь к `SELECT 1` + probe + `db.status`, § 12 `transport.md` |
| `db-credentials set/rotate` | Нужен UX «показать/заменить строку один раз»; сейчас слот пишет установщик |
| Несколько таблиц одновременно | Один `DbSource` = одна таблица = один курсор; несколько — это N проходов с N курсорами (таблица уже готова, `source` в ключе) |
| Детерминированный nonce | Не нужен: атомарность курсора+журнала закрывает дубликаты надёжнее, чем детерминированное AEAD |
| Публикация в облако/канал | Шаг 4/6 (`sync::transport`, `updater`) |

---

## 8. Как проверить

```bash
cd rust/teleforge-agent
cargo fmt --check
cargo clippy --all-targets -- -D warnings
cargo test                 # 412 тестов
```

Что покрыто тестами (без живой БД — через `db::mock::MockSource`):

| Проверка | Тест |
|---|---|
| полный цикл: курсор → батч → запечатано → `ops` → `sync.pull` | `db::ingest::tests::a_pass_publishes_sealed_rows_into_the_journal`, `api::server::tests::the_full_server_side_cycle_reaches_sync_pull` |
| перезапуск продолжает с курсора, дубликатов нет | `db::ingest::tests::a_restart_continues_from_the_cursor_and_does_not_duplicate`, `…::a_row_read_twice_hashes_the_same_and_is_a_duplicate` |
| ошибка БД: курсор не движется, `db.status.last_error`, процесс жив | `…::a_database_error_leaves_the_cursor_alone_and_keeps_working`, `…::a_transient_failure_does_not_skip_the_rows_it_covered`, `api::server::tests::the_ingest_scheduler_is_off_by_default_and_survives_a_failed_pass` |
| vault заперт → `Auth`, ничего не пишется | `…::a_locked_vault_refuses_the_pass_without_touching_the_cursor`, `api::server::tests::db_status_reports_a_locked_vault_without_failing` |
| запрет `personality_core` / `openai_api_key` / `pgConnString` | `db::allowlist::tests::*`, `db::ingest::tests::a_config_that_names_a_secret_or_unknown_table_is_refused` |
| `db.status` по сети: 200 с полями, 401 без токена | `api::server::tests::db_status_answers_with_the_contract_fields`, `…::db_status_requires_a_token_on_the_wire` |
| читатель не может запустить ingest | `api::server::tests::a_reader_token_cannot_start_the_ingest`, `auth::tests::a_reader_token_cannot_hold_the_ingest_scope` |
| монотонность курсора и HLC | `store::cursor::tests::*`, `db::ingest::tests::the_server_clock_is_strictly_monotonic_across_rows_and_passes` |
| утечка строки подключения | `db::credentials::tests::the_file_on_disk_is_not_the_connection_string`, `db::conninfo::tests::the_password_never_appears_in_the_redacted_form`, `api::server::tests::db_status_answers_with_the_contract_fields` |

Ручная проверка на живом сервере:

```sql
-- от имени teleforge_agent
SELECT 1;
SELECT id, kind, entity_id, field, observed_ms, value
  FROM sync_outbox ORDER BY id ASC LIMIT 5;
```

```bash
# 1. положить строку подключения (пока API-вызовом из установщика или тестом)
# 2. запустить с ingest и посмотреть статус
curl -s https://127.0.0.1:8137/rpc \
  -H "Authorization: Bearer <token>" --cacert agent.crt \
  -d '{"protocol":1,"request_id":"r-db","method":"db.status",
       "params":{"tenant":"0011…","account":"ffee…"}}'
# 3. один проход вручную (нужен токен роли ingest)
curl -s … -d '{"protocol":1,"request_id":"r-ing","idempotency_key":"r-ing",
               "method":"db.ingest","params":{"tenant":"0011…","account":"ffee…"}}'
```
