# Миграция C++ → Rust: пошаговый план

Область: замена `ayu/features/sync/**` (и частично `ayu/features/teleforge/**`)
на серверный агент из этого каталога.
Статус: **в работе.** Шаг 1a (пассивный модуль + хранилище секретов +
`--self-test`) и серверная половина шага 3 (транспорт `sync.push`/`sync.pull`,
планировщик ретенции, миграции `v1 → v2`, recovery-код account-level конверта,
`sync.conflicts` и `prune_upto`) реализованы в Rust; сборка C++ не менялась ни на
строку. Подробности — [`passive-module.md`](passive-module.md) и
[`transport.md`](transport.md); спецификация для C++ —
[`client-integration.md`](client-integration.md).

---

## Статус на сегодня

| Что | Статус | Где |
|---|---|---|
| Пассивный журнал: `accounts`, `entities`, `ops`, `presence`, `profiles`, `archive_obs` | **готово** | `src/store/**`, `docs/passive-module.md` § 2 |
| Приём событий: валидация, HLC-монотонность, дедуп по `content_hash`, `ingest_batch` в одной транзакции | **готово** | `src/store/ingest.rs` |
| RAII-транзакция с `ROLLBACK` в `Drop` (закрывает C4) | **готово** | `src/store/conn.rs` |
| Ретенция по возрасту и по размеру | **готово** | `src/store/retention.rs` |
| Хранилище секретов: Argon2id → KEK, случайный DEK, XChaCha20-Poly1305, AAD `account_id \|\| key_id`, AEAD-верификатор (закрывает C1) | **готово** | `src/keystore.rs`, `src/crypto/{kdf,aead}.rs` |
| `--self-test`: БД, права, KDF/AEAD, коды выхода | **готово** | `src/selftest.rs` |
| Vault (tenant-level): `init`/`unlock` без заглушек | **частично** — `create_tenant`/`unlock`/`change_passphrase` работают и протестированы; CLI-проводка и `mlock` — шаг 2 | `src/vault.rs` |
| Транспорт: JSON-RPC 2.0 поверх HTTP/1.1 + TLS 1.3 (rustls), bearer-токен устройства, `sync.push`/`sync.pull`/`sync.stamp`/`sync.conflicts`/`sync.prune` над `ingest_batch` | **готово** | `src/api/**`, [`transport.md`](transport.md) |
| Лимиты транспорта: тело, батч, заголовки, соединения, rate limit, таймауты; 400/401/403/409/413/429/501 | **готово** | `src/api/{http,rpc,server}.rs` |
| Планировщик ретенции в `serve` | **готово** | `src/api/server.rs` |
| Миграции схемы `v1 → v2` (идемпотентные, в одной транзакции) | **готово** | `src/store/migrate.rs` |
| Recovery-код для account-level конверта | **готово** | `src/keystore.rs`, § 5.6 `passive-module.md` |
| `sync.conflicts` и `prune_upto` | **готово** | `src/store/{query,retention}.rs`, `src/api/sync.rs` |
| Клиент C++ (`Agent::ShadowIngest`) | не начато; спецификация готова | [`client-integration.md`](client-integration.md) |
| mTLS на loopback, unix-socket listener, `pair`/`unlock` по сети | не начато (`501`) | шаг 2 |
| Импорт старых `TFSYNC1`-бандлов | не начато (чтение AES-256-GCM в `crypto::aead` уже есть) | шаг 4 |

Rust: `cargo fmt --check`, `cargo clippy --all-targets -D warnings`,
`cargo test` (323 теста, было 220) — всё зелёное.
C++ (`tdesktop/**`, `CMakeLists.txt`, `.gitmodules`, `cmake/**`) не изменялся.

---

## Шаг 1a. Пассивный модуль (реализовано)

**Цель:** серверная часть контура, который пассивно принимает и хранит события
клиента — сообщения (включая правки и удаления), присутствие/онлайн, профили,
архивные наблюдения — с привязкой к аккаунту, монотонной последовательностью и
без LWW-затирания. Плюс хранилище секретов вместо вывода ключа из
`userId + phone`.

### Что сделано

* `src/store/schema.rs` — DDL одной константой: `accounts`, `entities`, `ops`,
  `presence`, `profiles`, `archive_obs` (+ служебные `schema_meta`,
  `device_clock`). `account_id` в каждой таблице, `FOREIGN KEY` на `accounts`,
  `UNIQUE (account_id, op_seq)`, `UNIQUE (account_id, content_hash)`, индексы
  `(account_id, entity_id/peer_id, время)` и по времени ретенции.
* `src/store/conn.rs` — `Store` (одно соединение, `!Sync`), прагмы
  (`WAL`, `busy_timeout`, `synchronous=NORMAL`, `foreign_keys`,
  `auto_vacuum=INCREMENTAL`) на каждом открытии, `Tx` — RAII-транзакция,
  `ROLLBACK` из `Drop` (C4).
* `src/store/event.rs` — `Event`, `EventKind`, `AccountId` (не бывает нулевым),
  парсер `entity_id` с закрытыми наборами полей, `content_hash`,
  `RejectReason` с 15 причинами.
* `src/store/ingest.rs` — `ingest`/`ingest_batch` в одной транзакции; отказ
  события не отменяет батч, ошибка хранилища откатывает весь батч.
* `src/store/query.rs` — истории (`entity_ops`, `presence_history`,
  `profile_history`, `archive_observations`), производные «текущие» значения
  (`profile_current`, `presence_latest`), `account_stamp`, `totals`,
  `self_check`.
* `src/store/retention.rs` — `purge_by_age`, `purge_by_size` (по числу строк на
  аккаунт и по бюджету байт), `RetentionPolicy`.
* `src/keystore.rs` — конверт аккаунта с трейтами `Kdf`/`Aead`, реальными и
  stub-реализациями (stub отказывает в релизной сборке), `rotate`,
  `write`/`read` с `0600`, `Debug` без секретов, `probe()` для self-test.
* `src/crypto/kdf.rs`, `src/crypto/aead.rs`, `src/secret.rs` — настоящие
  Argon2id, HKDF-SHA256, XChaCha20-Poly1305, AES-256-GCM, `OsRng`, `wipe()`.
* `src/selftest.rs` — отчёт с 8 проверками, `exit_code` в JSON.

### Готово, когда (все пункты выполнены)

* [x] повторная отправка того же батча даёт `applied == 0` и не создаёт строк
      (`store::ingest::tests::replaying_the_same_batch_adds_no_rows`);
* [x] `op_seq` монотонна на аккаунт и не переиспользуется
      (`store::conn::tests::op_seq_is_monotone_per_account_and_independent_across_accounts`);
* [x] событие с чужим `account_id` отклоняется и не создаёт записей
      (`store::ingest::tests::an_event_for_another_account_is_rejected_not_stored`);
* [x] правки не затирают историю: new + edit + edit + delete = 4 строки
      (`store::ingest::tests::history_is_append_only_never_last_write_wins`);
* [x] ошибка записи не оставляет открытую транзакцию
      (`store::conn::tests::a_failed_transaction_rolls_back_and_the_connection_stays_usable`);
* [x] ретенция по возрасту и по размеру работает и не ломает монотонные счётчики
      (`store::retention::tests::*`);
* [x] `--self-test` проверяет схему, права, KDF/AEAD и возвращает `7` при провале
      (`selftest::tests::*`);
* [x] `cargo fmt --check`, `cargo clippy --all-targets -D warnings`, `cargo test`
      зелёные; тестов 220 вместо 77.

### Что осталось по этому шагу

* [x] Проводка `ingest_batch` в JSON-RPC (`sync.push`) — сделано вместе с
      транспортом шага 3, см. [`transport.md`](transport.md);
* [x] Периодический вызов `RetentionPolicy::apply` из `serve` — сделано,
      ошибка одного тенанта не роняет процесс;
* [x] Миграции схемы `v1 → v2` — `store::migrate`, идемпотентно, с тестом
      «старая БД → новая схема → данные целы»;
* [x] Восстановление account-level конверта по recovery-коду — `keystore`
      `create_with_recovery`/`unlock_with_recovery`;
* [ ] `mlock`/`MADV_DONTDUMP` для ключей — шаг 2 (`SecretBytes::lock_pages`
      по-прежнему `NotImplemented`, тест это фиксирует);
* [ ] Клиентские правки C++ (`Agent::ShadowIngest`) — спецификация есть,
      реализация намеренно не делалась.

---

## 0. Принципы миграции

1. **Контракт раньше кода.** Граница между клиентом и агентом — это
   `docs/architecture.md` § 9 (протокол) и § 10 (модель данных). Пока контракт не
   зафиксирован тестами на обеих сторонах, никакой C++ не удаляется.
2. **Старое остаётся рабочим до последнего шага.** Каждый шаг обратим одним
   переключателем.
3. **Никаких «больших переключений».** Ни один шаг не меняет одновременно способ
   хранения и способ передачи.
4. **Данные важнее кода.** Перед каждым шагом, который пишет в
   `tdata/data.tforge`, делается проверяемый бэкап и есть процедура сверки.
5. **Ничего не ломать в сборке.** Все изменения клиента — за
   feature-флагом/`#ifdef TELEFORGE_WITH_AGENT`, который по умолчанию выключен до
   шага 5. Это правило существует потому, что сборку только что подняли.

### 0.1 Переключатели

| Флаг | Значения | Кто читает |
|---|---|---|
| `TELEFORGE_WITH_AGENT` (compile) | on/off | подключает `ayu/features/sync/agent/*` в сборку |
| `TeleForge::Agent::Mode` (runtime) | `off` \| `tunnel` \| `shadow` \| `dual-write` \| `primary` | клиент |
| `TF_SYNC_ENGINE` (env, debug) | `legacy` \| `agent` \| `shadow` | тесты и support |
| `TeleForge::Agent::KillSwitch` | если агент не отвечает N раз — возврат на `legacy` | клиент |

`KillSwitch` обязателен: сервер пользователя может умереть в любой момент, и
приложение не должно из-за этого терять запись удалённых сообщений.

---

## Шаг 1. Агент как «только туннель» (parity)

**Цель:** заменить `TeleForge::Ssh` на агентский эндпоинт, не меняя больше ничего.

### Что делается

* Rust: `tunnel.up` / `tunnel.down` / `tunnel.status` (уже описаны в
  `src/tunnel.rs`), `russh`-путь, пиннинг host key, валидация identity file
  (0600), отказ от `StrictHostKeyChecking=accept-new`.
* C++: новый тонкий клиент, **только новые файлы**:
  `ayu/features/sync/agent/teleforge_agent_client.{h,cpp}`,
  `ayu/features/sync/agent/teleforge_agent_tunnel.{h,cpp}`.
  `ApplyTunnelIfConfigured` получает ветку:
  `if (Agent::Mode() == tunnel) return Agent::TunnelUp(...)`.
  Существующий `EnsureTunnelBlocking` **не удаляется** — он fallback.
* Никаких изменений в `CMakeLists.txt` до тех пор, пока шаг не готов целиком;
  на время разработки файлы подключаются тем же механизмом, что и остальной
  `ayu/**` (он уже собирается глобом/списком), а `TELEFORGE_WITH_AGENT`
  добавляется в шаг 2 вместе с остальными правками сборки — одной правкой, а не
  тремя.

### Готово, когда

* [ ] `tunnel.up` возвращает local port, `tunnel.status` — `Ready`;
* [ ] `RewriteConnStringHostPort`-эквивалент на стороне агента даёт тот же
      результат, что C++ (сверка на 20 реальных connstring, включая
      `password=`, `host=`, `server=`, отсутствующий `port=`);
* [ ] неверный identity file (0644) отклоняется **до** запуска SSH;
* [ ] неизвестный host key → `1003 forbidden`, не «попробуем»;
* [ ] при падении агента клиент за ≤ 2 попытки уходит в `legacy` и продолжает
      работать (проверяется тестом с убитым агентом);
* [ ] TTFB туннеля ≤ времени `EnsureTunnelBlocking` на том же хосте;
* [ ] C++ сборка с `TELEFORGE_WITH_AGENT=off` байт-в-байт ведёт себя как раньше
      (регресс-тест: туннель поднимается, sync работает).

### Риски

`russh` может не пройти на хостингах, режущих нестандартные каналы. Поэтому
`--tunnel-backend=ssh-binary` остаётся доступным, а шаг не считается готовым,
пока хотя бы один реальный прогон не прошёл на каждом из {Hetzner, DigitalOcean,
домашний NAT}.

### Откат

`Agent::Mode = off`. C++-путь не тронут, откат мгновенный.

---

## Шаг 2. Vault: пароль, ключи, сессии

**Цель:** агент умеет хранить секреты правильно. Пока **ничего не
синхронизируется через агента** — это шаг про криптографию и файлы.

**Статус: частично.** Криптопримитивы настоящие (`argon2`, `chacha20poly1305`,
`aes-gcm`, `hkdf`, `sha2`, `rand`, `zeroize`), `create_tenant`/`unlock`/
`change_passphrase` работают и покрыты тестами, account-level конверт закрывает
C1 (`docs/passive-module.md` § 5). Осталось: CLI-проводка `init/unlock/lock`,
`mlock`/`MADV_DONTDUMP`, восстановление после смены пароля с recovery-кодом,
замеры времени и `strace`/`gcore`.

### Что делается

* Rust: `vault.rs`, `crypto/{kdf,aead,sig}.rs` — реальные примитивы вместо
  заглушек (`argon2`, `chacha20poly1305`, `aes-gcm`, `hkdf`, `sha2`,
  `ed25519-dalek`, `zeroize`, `rand`).
* Rust: `secret.rs::lock_pages`, `main`-хардненинг (`RLIMIT_CORE`,
  `PR_SET_DUMPABLE`), `fsperm` с `O_NOFOLLOW`.
* C++: **не меняется**. Шаг полностью серверный.
* Появляется CLI `teleforge-agent init/unlock/lock/status` и `self-test`.

### Готово, когда

* [ ] `init` → `unlock` → `lock` → `unlock` проходит на Linux;
* [ ] неверный пароль даёт `1002` и **не** даёт отличимого по времени ответа
      (замер: 100 попыток, разброс не больше дисперсии самого Argon2id);
* [ ] `change_passphrase` меняет `key_id` и не пере-шифровывает данные;
      с recovery-кодом тенант открывается после смены;
* [ ] потеря пароля + recovery-код → доступ есть; потеря обоих → `1002` и
      внятное сообщение (тест);
* [ ] `self-test` проходит без root на чистой Ubuntu 22.04/24.04 и Debian 12;
* [ ] `strace -f -e trace=openat` не показывает ни одного открытия файла с
      секретами в `/tmp` или с режимом шире 0600;
* [ ] `gcore` процесса с разблокированным vault **не содержит** DEK (проверка
      `grep` по дампу на 32-байтный паттерн из теста);
* [ ] тест «забыли занулить»: unit-тест на `SecretBytes::Drop` (после drop буфер
      нулевой) — уже есть каркас, нужен реальный `mlock`.

### Риски

`mlock` может быть недоступен в контейнере. Тогда `lock_pages` возвращает
ошибку, `self-test` показывает предупреждение, а не провал: деградация, а не
отказ. Явно показывать в UI.

### Откат

Шаг ничего не блокирует: агент просто не используется. Удалить каталог
`tenants/` — потерять только то, чего ещё нет.

---

## Шаг 3. Sync в режиме shadow (агент считает, но не влияет)

**Цель:** проверить новую модель данных на реальном потоке, ничего не меняя для
пользователя.

**Статус: серверная половина готова, клиентская — нет.** Журнал, приём событий,
дедуп, порядок, проекции, ретенция **и транспорт** реализованы и протестированы
(`src/store/**`, `src/api/**`, [`transport.md`](transport.md)); не хватает
`Agent::ShadowIngest` в C++ (см. [`client-integration.md`](client-integration.md))
и 72-часового прогона.

**Уточнение архитектуры (после шага 3).** Основной источник данных — не клиент,
а **сервер**: агент стоит рядом с общей БД (PostgreSQL/MySQL) и читает изменения
сам, шифрует их DEK-ом аккаунта и публикует; клиент в основном вызывает
`sync.pull`, а `sync.push` остаётся для локальных фактов. Это меняет объём шага:
к нему добавляется чтение БД, `db.status` и серверное шифрование — их в коде ещё
нет. См. `transport.md` § 1.3, § 2.6, § 15 и `client-integration.md` § 2.

### Что делается

* Rust: `sync::engine` с настоящим SQLite (`rusqlite`, bundled), `op_log`,
  `entity_field`, `seq`, `entity_root`; `sync.stamp`, `sync.pull`, `sync.push`.
* C++: **dual-read, без dual-write.** Поток остаётся прежним
  (`scheduleUploadDebounced` → PG/MySQL/Telegram-канал). Дополнительно, за
  `Agent::Mode == shadow`:
  * те же события, которые уже пишутся в `DeletedMessage`/`EditedMessage`,
    отправляются в агент как `Op`;
  * раз в N минут клиент делает `sync.pull` с `since_seq=0` и **сравнивает**
    содержимое с локальной БД, считая расхождения;
  * расхождения только логируются (`LOG` с количествами, без текстов), ничего не
    применяется.

Точки отправки выбираются так, чтобы не дублировать логику: одна функция
`Agent::ShadowIngest(const AyuMessageBase&, kind)`, вызываемая рядом с
`AyuDatabase::addDeletedMessage` / `addEditedMessage` (в вызывающем коде, а не
внутри, чтобы не менять сигнатуры существующих функций).

### Готово, когда

* [ ] на тестовом аккаунте с ≥ 5 000 удалённых/изменённых сообщений число
      расхождений shadow-сверки = 0 (кроме известных: правки собственных
      сообщений, которые C++ не пишет — см. M8);
* [ ] `sync.pull` отдаёт 5 000 операций за ≤ 5 с на 1 vCPU;
* [ ] повторный `sync.push` той же пачки даёт `duplicates == N`, `applied == 0`;
* [ ] искусственная конкурентная правка одного поля даёт ровно один `conflict`,
      и он виден в `sync.conflicts`;
* [ ] 72 часа shadow-режима без роста RSS больше 30 МиБ и без роста `data.db`
      больше, чем на объём операций;
* [ ] ни одного секрета в `data.db` (проверка: `strings` по файлу не содержит
      ни одного значения из `PersonalityCore.openAiApiKey`/`pgConnString`).

### Риски

Расхождение из-за M8 (правки собственных сообщений не сохраняются в C++).
Ожидаемо; список исключений фиксируется в тесте, а не «объясняется вручную».

### Откат

`Agent::Mode = off`. Никаких записей в локальную БД шаг не делает.

---

## Шаг 4. Dual-write: агент становится вторым хранилищем

**Цель:** научиться писать в агент как в полноценный бэкенд и читать оттуда, но
источником истины остаётся локальная SQLite.

### Что делается

* Rust: полный набор `FieldId` из модели, конвертер старых `TFSYNC1`-бандлов в
  `op_log` (одноразовый импорт). **`prune_upto` и `sync.conflicts` уже готовы**
  (серверная часть шага сделана раньше, чем клиентская: они не зависят от C++).
* C++: `Agent::Mode == dual-write`:
  * запись идёт в локальную БД **и** в агент; ошибка агента не влияет на
    локальную запись (это уже правильная семантика существующего кода:
    «локальная БД — источник истины, облако — best-effort»);
  * чтение — из локальной БД; `sync.pull` используется только для сверки;
  * конфликты складываются в новую таблицу `AgentConflict` (локально) и
    показываются в UI как «требует решения», без автовыбора.
* Импорт существующих облачных данных: `RunPgUpload`/`sync-канал` больше не
  пишут новое, но **читаются один раз** конвертером.
* Секреты из `PersonalityCore` переезжают в vault: `pgConnString` разделяется на
  параметры (в `agent.toml`) и пароль (в vault). Это первый шаг, который
  **трогает** `teleforge_core.*` — и он делается отдельным коммитом с отдельным
  ревью.

### Готово, когда

* [ ] два устройства (десктоп + второй десктоп) с одним аккаунтом сходятся к
      одинаковому набору за ≤ 2 цикла;
* [ ] правка разных полей одной сущности на двух устройствах **не** даёт потерь
      (в отличие от `MergeDataDb`, где `INSERT OR REPLACE` затирал строку);
* [ ] конкурентная правка одного поля даёт видимый конфликт, а не молчаливую
      потерю;
* [ ] отключение сети на 30 минут не теряет ни одной операции (проверяется
      сверкой счётчиков);
* [ ] `pgConnString` в `data.tforge` больше не содержит `password=`
      (регресс-тест на строку);
* [ ] импорт старого `TFSYNC1`-бандла даёт те же строки, что были в
      `DeletedMessage`/`EditedMessage` (сверка по `contentHash`);
* [ ] `Agent::Mode = tunnel` всё ещё работает (шаг 1 не сломан).

### Откат

`Agent::Mode = tunnel` или `off`. Данные в агенте остаются, их можно удалить.

---

## Шаг 5. Primary: агент — основное хранилище синхронизации

**Цель:** убрать облачные бэкенды из C++ как путь записи.

### Что делается

* C++: `cloudBackend` в `PersonalityCore` деприкейтится. Ветки
  `Backend::Postgres`, `Backend::MySql`, `Backend::TelegramChat` в
  `BackendUpload`/`BackendDownload` (`teleforge_sync.cpp`) остаются **только для
  чтения при импорте** и помечаются как deprecated.
* Новый `TeleForge::Agent::Sync` становится единственным путём:
  `scheduleUploadDebounced` → `Agent::Push`, `tryDownloadOnStartup` →
  `Agent::PullSince`, `checkRemoteFreshness` → `sync.stamp`.
* `Telegram/SourceFiles/ayu/features/sync/teleforge_sync_transport.cpp`,
  `teleforge_sync_files.cpp`, `teleforge_sync_pg.cpp`,
  `teleforge_sync_mysql.cpp`, `teleforge_sync_merger.cpp`,
  `teleforge_sync_snapshot.cpp` — выводятся из сборки (не удаляются из дерева до
  шага 6, чтобы был откат).
* `teleforge_ssh_tunnel.cpp` остаётся только как fallback для
  `--tunnel-backend=ssh-binary` в самом агенте; C++-копия удаляется на шаге 6.

### Готово, когда

* [ ] неделя на `primary` без потери операций (сверка счётчиков на 3 аккаунтах);
* [ ] «удалить из облака» реализовано и проверено: после операции
      `sync.stamp.high_water` не растёт от старых данных, а сам тенант можно
      снести;
* [ ] регресс: `checkRemoteFreshness`-эквивалент не делает полный скан (замер
      времени ответа `sync.stamp` ≤ 50 мс на 100 000 операций);
* [ ] ни один тест не требует сети к PostgreSQL/MySQL (CI зелёный без БД);
* [ ] UI: экран согласия объясняет, что уходит на сервер, и что не уходит
      (сессии — да, значения sync — нет).

### Откат

`Agent::Mode = dual-write`: старые бэкенды всё ещё в сборке, ничего не потеряно.

---

## Шаг 6. Уборка

**Цель:** удалить мёртвый код, чтобы его не «починили» обратно.

### Что удаляется

| Файл | Причина |
|---|---|
| `teleforge_sync_pg.cpp`, `teleforge_sync_pg.h` | заменено `sync.push`/`sync.pull` |
| `teleforge_sync_mysql.cpp`, `.h` | то же |
| `teleforge_sync_transport.cpp`, `.h` | канал Telegram больше не транспорт |
| `teleforge_sync_files.cpp`, `.h` | загрузка документов в канал |
| `teleforge_sync_merger.cpp`, `.h` | заменено `engine::classify`/`plan` |
| `teleforge_sync_snapshot.cpp`, `.h` | снимки всей БД не нужны |
| `teleforge_sync_rows.cpp`, `.h` | заменено `Op`/`FieldId` |
| `teleforge_ssh_tunnel.cpp`, `.h` | туннель в агенте |
| `teleforge_sync_crypto.cpp`, `.h` | **но** `DecryptBundle` нужен для импорта старых бандлов — удаляется после последнего импорта; до тех пор остаётся в `agent/legacy_bundle.cpp` |

### Готово, когда

* [ ] `grep -r "DeriveSyncKey\|HkdfSha256\|HkdfSha256\|MasterMaterial"` по дереву
      не находит ничего;
* [ ] `grep -r "ProjectEmbedding"` не находит ничего (или находит только
      документированный opt-in режим с другим ключом);
* [ ] `grep -r "personality_core.tforge"` не находит ничего;
* [ ] сборка C++ зелёная и **меньше** по количеству TU, чем до шага;
* [ ] `docs/audits/passive-sync-audit.md`: C1, C2, C3, C4, C5, H7, H8, H9, H10,
      M14 отмечены как закрытые со ссылкой на этот каталог.

---

## Dual-run: как C++ и Rust работают одновременно

### Механика

```
                    ┌─────────────────────────────┐
  событие в клиенте │ AyuDatabase::addDeleted…    │  ← источник истины
                    └──────────┬──────────────────┘
                               │
              ┌────────────────┴─────────────────┐
              ▼                                  ▼
     существующий путь                 Agent::ShadowIngest
   (PG / MySQL / канал)                (sync.push в агент)
              │                                  │
              ▼                                  ▼
         пользователь видит это        shadow: только лог
                                       dual-write: ещё и чтение при сверке
                                       primary: это и есть путь
```

Три правила dual-run:

1. **Локальная запись никогда не зависит от агента.** Агент недоступен → пишем
   локально, ставим в очередь, повторяем с backoff. Ровно та семантика, что уже
   заявлена в комментарии `teleforge_sync_rows.h` («remote DB is only a replica»),
   и она сохраняется.
2. **Ни одна ветка не «решает» конфликт двух путей.** В shadow — только
   сравнение и лог. В dual-write — обе записи независимы, разрешение конфликтов
   живёт в агенте (и только он его делает).
3. **KillSwitch.** N подряд неудачных обращений к агенту (по умолчанию 5) →
   `Agent::Mode` понижается до `legacy`/`tunnel` до перезапуска, с записью в лог
   и видимым (не модальным) уведомлением в UI.

### Как проверяется, что dual-run не врёт

Отдельный инструмент (не в продукте): `tools/agent-diff` — читает локальную
`data.tforge` и `sync.pull` из агента, сверяет по `EntityKey` и печатает:

```
entity            field          local      agent      verdict
msg -1001:42 r1   Text           present    present    ok
msg -1001:42 r1   EditDate       0          0          ok
peer 900          PeerUsername   set(3)     set(3)     ok
memory #17        MemoryTitle    17         -          id-collision (ожидаемо, H7)
```

Он же — основа теста «5 000 сообщений, 0 расхождений» из шага 3.

### Что делать, если шаг не прошёл

Откат всегда один и тот же: переключатель. Ни один шаг не удаляет данные
локальной БД и не меняет её схему до шага 4, а на шаге 4 изменения схемы —
только добавление таблицы `AgentConflict` (`ADD TABLE`, не `ALTER`), что
безопасно для старых версий: `sync_schema` их просто не увидит.

---

## Порядок работ и оценки

| Шаг | Что блокирует | Оценка |
|---|---|---|
| **1a (пассивный модуль)** | **сделан** | — |
| 1 | нет | 1–2 недели (включая `russh` на реальных хостингах) |
| 2 | нет | 1–2 недели (крипто сделано; остались `mlock`, CLI, замеры) |
| 3 | шаг 2 (нужен vault) | 2 недели + 72 часа прогона |
| 4 | шаг 3, плюс решение по `PersonalityCore` | 3 недели |
| 5 | шаг 4, плюс неделя наблюдения | 1 неделя |
| 6 | шаг 5 | 2–3 дня |

Итого ≈ 8–10 недель инженерного времени, из них ~1 неделя — прогоны, которые
нельзя ускорить.

---

## Что делать первым

1. **Зафиксировать контракт тестами на обеих сторонах.** Rust-сторона имеет
   220 тестов (после шага 1a); нужен C++-эквивалент на JSON-примеры из
   `docs/architecture.md` § 9.5, чтобы обе стороны не разошлись молча.
   Это единственная работа, которая должна идти **до** шага 3.
2. **Шаг 1 (туннель).** Наименьший риск, проверяет всю цепочку «клиент ставит
   агента → агент отвечает → клиент откатывается при сбое», и не требует ни
   одной строки изменений в модели данных.
3. **Параллельно — не зависящее от шагов:** реализовать примитивы из шага 2
   (Argon2id, AEAD, Ed25519) и заменить заглушки. Они не блокируются ничем и
   сразу снимают главный вопрос дизайна (C1).
   **Сделано:** Argon2id, HKDF-SHA256, XChaCha20-Poly1305 и AES-256-GCM
   реализованы (`src/crypto/**`), account-level конверт закрывает C1
   (`src/keystore.rs`). Ed25519 (`src/crypto/sig.rs`) остаётся заглушкой —
   он нужен шагу `updater`, а не пассивному модулю.
