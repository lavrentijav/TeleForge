# Транспорт: `sync.push` / `sync.pull` по JSON-RPC 2.0 поверх HTTP/TLS

Статус: **реализовано** (шаг 3, серверная половина). После уточнения заказчика
основным источником данных стал **сам сервер**: он подключён к общей БД
пользователя, читает изменения, шифрует и публикует, а клиент в основном читает.
Этот путь реализован в этой итерации — `db.*`, чтение БД по курсору, серверное
шифрование, поток ingest (§ 1.3, § 5.8, § 9.1, § 10, [`db-ingest.md`](db-ingest.md)).
Клиентская половина (`Agent::ShadowIngest` в C++) не трогалась; её спецификация —
[`client-integration.md`](client-integration.md).

Код: `src/api/{http,tls,pem,rpc,sync,server,signals,base64}.rs`, `src/auth.rs`,
`src/store/{ingest,query,retention,migrate,cursor}.rs`, `src/db/**`.
Тесты: `cargo test` — **412** (было 323), `cargo clippy --all-targets -D warnings`
и `cargo fmt --check` чистые.

---

## 1. Стек

```
SSH-канал клиента                     ← внешний, агент его не видит
  TLS 1.3, сертификат агента          ← клиент пиннит fingerprint при установке
    HTTP/1.1, один запрос на соединение
      JSON-конверт (метод + params)
        bearer-токен устройства       ← sha256 в auth/clients.json
          sync.*                      ← пассивный журнал, append-only
```

| Слой | Что решает | Модуль |
|---|---|---|
| TLS 1.3 | что это тот самый агент (пиннинг), шифрование токена на проводе | `api::tls` |
| HTTP/1.1 | статус до разбора тела (401/403/409/429/400), `curl` для поддержки | `api::http` |
| JSON | конверт и параметры; уже используется клиентом в трёх местах | `api::rpc` |
| Bearer | что запрос пришёл от зарегистрированного устройства | `auth`, `api::rpc` |
| `sync.*` | что именно читается/пишется в журнале | `api::sync`, `store` |

### 1.1 Почему HTTP, а не «сырой кадр с длиной»

`docs/architecture.md` § 9.1 фиксировал кадр `[len u32 BE][JSON]` и обещал HTTP
только как вариант. Здесь выбран HTTP, и это решение, а не вкус:

* **статус-код — часть контракта.** Задача требует 401/403/409/429/400 *на
  уровне транспорта*. Клиент, который не разобрал тело (потому что версия
  не совпала, потому что прокси съел ответ), всё равно должен показать «токен
  неверный», а не «ошибка разбора». С кадром длины эти классы пришлось бы
  кодировать внутри JSON, то есть тем же полем, которое не читается.
* **диагностика без клиента.** `curl --insecure --http1.1 -d @body
  https://127.0.0.1:8137/rpc -H 'Authorization: Bearer …'` воспроизводит любую
  жалобу. С кадром длины нужен отдельный инструмент, и он будет написан
  неправильно.
* **длина всё равно проверяется до чтения** — требование § 9.1 сохранено:
  `Content-Length` сверяется с `max_body_bytes` до `vec![0; n]`.

Что сознательно **не** реализовано: keep-alive, chunked, `Expect:
100-continue`, сжатие, HTTP/2. Каждый из них — парсер для пира, который уже
аутентифицирован TLS и токеном; ни один не нужен, потому что клиент отправляет
батчи по 2000 событий, а не по одному.

### 1.2 Один запрос на соединение

`Connection: close` в каждом ответе. Сервер обслуживает ровно один запрос и
закрывает соединение. Это убирает из поверхности атаки состояние keep-alive,
пайплайнинг и idle-таймауты, ценой одного TCP-handshake на батч (TLS-сессию
клиент переиспользует, `rustls` это поддерживает).

### 1.3 Направление обмена: два режима

Уточнение заказчика: **агент стоит на том же сервере, что и БД пользователя**,
подключается к ней (SSH-туннель или прямой TLS), читает изменения, шифрует их
DEK-ом аккаунта и публикует. Клиент в основном **читает**. `sync.push` из
клиента остаётся только для того, чего в БД нет.

```
[общая БД: PostgreSQL / MySQL]      ← источник истины (как сейчас)
        │ SELECT по монотонному курсору
        ▼
[teleforge-agent]   читает → шифрует subkey(DEK, sync-data) → ops (append-only)
        ├──► облако / канал: зашифрованные шарды
        └──► sync.pull ──► C++ клиент (только чтение)
                              └── sync.push (доп.): локальные ghost-данные
```

| | Server-side sync (основной) | Client-side push (дополнительный) |
|---|---|---|
| Источник | БД пользователя | клиент |
| Кто пишет в `ops` | агент | клиент |
| Статус в коде | **реализовано** (`src/db/**`, [`db-ingest.md`](db-ingest.md)) | реализовано |
| Метод клиента | `sync.pull`, `sync.stamp`, `sync.conflicts`, `db.status` | `sync.push` |
| Права | `SELECT` на таблицы, `INSERT/UPDATE` только на курсор | scope `sync:write` |
| Нужен unlock | да: для шифрования нужен DEK | нет: значение уже запечатано клиентом |

Что из этого следует:

* **`sync.pull` — главный путь.** Клиент не обязан ничего пушить, чтобы
  сойтись; `push` — только для локальных фактов.
* **Агент читает plaintext БД.** Для строк, прочитанных из БД, агент держит DEK
  и видит открытые значения до шифрования. Облако и канал по-прежнему получают
  только шифртекст, но утверждение «агент видит только метаданные»
  (`architecture.md` § 7.6) для этого пути больше не верно. Это решение
  заказчика; его надо подтвердить явно (`client-integration.md` § 12.2).
* **Нужен `device_id` серверного ingest'а** (предлагается `server-db`), чтобы
  клиент отличал серверные записи от своих, а монотонность HLC считалась
  отдельно от устройств.
* **`sync.push` и серверный ingest могут описать один факт дважды.** У них
  разные `device_id` и HLC, поэтому `content_hash` не совпадёт и это будет
  конфликт, а не дубликат. Политика — `client-integration.md` § 2, следствие 2.
* **`db.status`** — отдельный метод наблюдаемости (подключение к БД, курсор,
  лаг). Реализован: контракт в § 5.8, код `src/db/ingest.rs::DbIngest::status`,
  настройка — [`db-ingest.md`](db-ingest.md).

---

## 2. Аутентификация: клиент → агент и агент → БД

### 2.1 Выбран bearer-токен, а не подпись Ed25519 на запрос

`docs/architecture.md` § 6.1 допускает оба. Выбран токен, потому что:

1. **Подписи пока нет.** `crypto::sig` — заглушка (Ed25519 запланирован на шаг
   `updater`). Реализовывать Ed25519 *ради транспорта* — значит впервые вводить
   примитив в непроверенном месте.
2. **Подпись не убирает trust anchor.** Проверка подписи требует
   зарегистрированного публичного ключа на устройство — это тот же шаг
   enrollment, тот же файл, та же отзываемость. Меняется только то, что защита
   от replay перестаёт быть задачей TLS и становится нашей (окно nonce,
   смещение часов, состояние на соединение).
3. **Токен привязан к каналу.** Он принимается только на пиннингованом
   TLS-listener'е, а listener доступен только через SSH-форвард клиента.

Слой mTLS остаётся в `config.tls.client_ca_pem` для явного режима
`listen = network`; по умолчанию он выключен, потому что SSH-туннель делает его
избыточным.

### 2.2 Enrollment

```bash
teleforge-agent pair --label "MacBook" \
    --tenant 00112233445566778899aabbccddeeff \
    --account ffeeddccbbaa99887766554433221100 \
    --data-dir /var/lib/teleforge-agent
```

Печатает **один раз**:

```json
{"device_id":"bc733612b593fdb12bb2d14b0fd287f1",
 "device_token":"e8ae9c...ab657",
 "label":"MacBook",
 "tenant":"00112233445566778899aabbccddeeff",
 "account":"ffeeddccbbaa99887766554433221100",
 "scopes":["status","sync:read","sync:write"],
 "clients_file":"…/auth/clients.json"}
```

На диск попадает только `sha256(token)` (`auth/clients.json`, `0600`,
атомарная замена). Токен не пишется в лог и не проходит через `Logger` — он
печатается в stdout, как recovery-код.

**Привязка к аккаунту.** `ClientRecord` содержит `tenant` и `account_id`.
Запрос с другим `account` — `403` **до** разбора `ops`; событие с чужим
`account_id` внутри журнала тоже невозможно (`ingest` отклоняет, а `account_id`
во всех таблицах имеет `FOREIGN KEY`). Это дешёвая, но настоящая граница: одна
опечатка в клиенте не должна писать в чужой аккаунт.

**Токен — это читатель.** Доступ к БД — отдельная учётная запись (§ 2.6),
которая к токену клиента отношения не имеет и по HTTP не передаётся.

**Роль токена.** У записи устройства есть явный `role` (`reader` | `ingest`,
`auth::TokenRole`). `pair` без `--role` выдаёт `reader`, и `reader` не может нести
scope `db:ingest`: это проверяется при `enroll_with_role`, при разборе
`clients.json` и в `ClientRecord::allows`. Файл без поля `role` (написанный
старым агентом) читается как `reader`, то есть сохраняет ровно те права, что были.

### 2.3 Scopes

| Scope | Методы |
|---|---|
| `status` | `hello`, `status` |
| `sync:read` | `sync.stamp`, `sync.pull`, `sync.conflicts`, `db.status` |
| `sync:write` | `sync.push`, `sync.prune` |
| `db:ingest` | `db.ingest` (внутренний: `pair` его не выдаёт, см. § 15 п. 11) |
| `admin` | всё вышеперечисленное (подразумевает любой scope) |

Токен без нужного scope → `403` с сообщением, какой scope отсутствует.

### 2.4 Почему токен хранится как SHA-256, а не Argon2id

Argon2id замедляет **угадывание**. 256-битный случайный токен не угадывается, и
медленный KDF только замедлил бы каждый запрос. Пароли → Argon2id, токены →
SHA-256. Это зафиксировано в `auth.rs`, чтобы никто потом не «исправил
консистентность».

### 2.5 Replay и rate limit

* Replay: TLS 1.3 даёт свежий ключ на каждое соединение, а токен не несёт
  состояния. `auth::ReplayWindow` реализован и протестирован, но включать его
  в этом шаге не нужно — это задел под подписанные запросы.
* Rate limit: token bucket на устройство (`rate_limit_burst` запросов,
  `rate_limit_per_sec` в секунду). Превышение → `429` / `1007`, `retryable:
  true`. Ответ 429 приходит *до* обработки, поэтому залипший клиент не может
  превратить ретраи в write amplification.
* `hello` разрешён без токена (это рукопожатие до enrollment) и лимитируется
  общим ведром `anonymous`.

### 2.6 Права БД для агента

Агент подключается к БД **отдельным пользователем**, у которого нет прав
владельца схемы. Это не удобство, а граница доверия: агент читает данные
пользователя, и его компромисс не должен стоить компромисса всей БД (T1 в
`architecture.md` § 5).

Требования:

1. **Отдельная роль:** `teleforge_agent`. Не `root`/`postgres`/`sa`, не владелец
   БД, не член `pg_read_all_data`.
2. **`SELECT` только на синхронизируемые таблицы**, по возможности только на
   нужные колонки. Никаких `GRANT ALL ON *.*`, `SUPER`, `CREATE`, `DROP`,
   `ALTER`, `GRANT OPTION`.
3. **`INSERT`/`UPDATE` — только на таблицу курсора/outbox** (если агент помечает
   «опубликовано»). Больше никуда.
4. **Никакого доступа к таблицам секретов:** `personality_core`, всё с
   `openai_api_key`, `pgConnString`, `ssh*`.
5. **Креды — в keystore, не в `agent.toml`.** `agent.toml` (`0640`) содержит
   только host/port/user/dbname/sslmode; пароль/ключ лежит в `vault` (`0600`,
   AEAD под KEK из пароля) и наружу не отдаётся. Это прямое закрытие M14.
6. **Транспорт:** `sslmode=verify-full` (PG) или `tls=true` + CA bundle (MySQL),
   либо SSH-туннель с пиннингом host key (`StrictHostKeyChecking=yes`, известный
   fingerprint в `~/.ssh/known_hosts` агента). `trust`/`sslmode=disable` — отказ
   при старте, а не предупреждение.
7. **Проверка при установке:** `SELECT 1` и
   `SELECT <cursor> FROM <sync-table> ORDER BY <cursor> DESC LIMIT 1`. Нет прав —
   установка останавливается с сообщением, какой таблицы/колонки не хватает.

PostgreSQL:

```sql
CREATE ROLE teleforge_agent LOGIN PASSWORD '<из keystore>';
GRANT CONNECT ON DATABASE teleforge TO teleforge_agent;
GRANT USAGE ON SCHEMA public TO teleforge_agent;
REVOKE ALL ON SCHEMA public FROM PUBLIC;   -- если ещё не сделано

-- только нужные таблицы и только нужные колонки
GRANT SELECT (id, chat_id, message_id, from_id, date, text, entities,
              edit_date, views, media, reply_to_msg_id, grouped_id, topic_id,
              flags)
    ON messages TO teleforge_agent;
GRANT SELECT ON deleted_messages, edited_messages TO teleforge_agent;
GRANT SELECT ON online_events TO teleforge_agent;
GRANT SELECT (peer_id, first_seen_at, last_seen_at)
    ON peer_archive_profile TO teleforge_agent;
GRANT SELECT (peer_id, observed_at, name)
    ON peer_archive_name TO teleforge_agent;
GRANT SELECT (peer_id, observed_at, username)
    ON peer_archive_username TO teleforge_agent;
GRANT SELECT (peer_id, observed_at, bio)
    ON peer_archive_bio TO teleforge_agent;
GRANT SELECT (peer_id, chat_id, observed_at, is_member)
    ON peer_archive_membership TO teleforge_agent;
GRANT SELECT (peer_id, chat_id, message_id, kind, observed_at)
    ON archive_observations TO teleforge_agent;

-- курсор публикации: агент пишет только сюда
GRANT SELECT, INSERT ON sync_outbox TO teleforge_agent;
GRANT UPDATE (published_at, op_seq) ON sync_outbox TO teleforge_agent;
```

MySQL:

```sql
CREATE USER 'teleforge_agent'@'127.0.0.1' IDENTIFIED BY '<из keystore>'
  REQUIRE SSL;
GRANT SELECT (id, chat_id, message_id, from_id, date, text, entities,
              edit_date, views, media, reply_to_msg_id, grouped_id, topic_id,
              flags)
  ON teleforge.messages TO 'teleforge_agent'@'127.0.0.1';
GRANT SELECT ON teleforge.deleted_messages TO 'teleforge_agent'@'127.0.0.1';
GRANT SELECT ON teleforge.edited_messages  TO 'teleforge_agent'@'127.0.0.1';
GRANT SELECT ON teleforge.online_events    TO 'teleforge_agent'@'127.0.0.1';
GRANT SELECT, INSERT ON teleforge.sync_outbox TO 'teleforge_agent'@'127.0.0.1';
GRANT UPDATE (published_at, op_seq) ON teleforge.sync_outbox
  TO 'teleforge_agent'@'127.0.0.1';
FLUSH PRIVILEGES;
```

Оговорки, которые надо проговорить оператору:

* **Имена таблиц/колонок выше — пример.** Реальная облачная схема (PG/MySQL)
  определяется тем, что пишет клиент; точный список нужен до реализации
  (`client-integration.md` § 12.9).
* **Курсор обязателен.** Без монотонной колонки (автоинкремент `id`, отдельный
  outbox или `updated_at` с гарантией уникальности) инкрементальное чтение
  невозможно: одинаковые секунды в `updated_at` пропускают строки. Рекомендуемый
  вариант — outbox-таблица, которую заполняет клиент или триггер.
* **Публикация в облако/канал** — отдельный шаг и к правам на БД отношения не
  имеет.
* **Ротация кредов:** команды `teleforge-agent db-credentials rotate` пока нет;
  смена пароля сейчас = обновить запись в vault, что в коде тоже не проведено
  (§ 13).

---

## 3. HTTP-уровень

| Что | Значение |
|---|---|
| Метод | `POST` (единственный) |
| Путь | `/` или `/rpc`; остальное — `404` |
| `Content-Type` запроса | не проверяется (тело должно быть JSON-объектом) |
| `Authorization` | `Bearer <64 hex>` (32 байта) |
| Тело | JSON-объект конверта, UTF-8 |
| Ответ | `application/json; charset=utf-8`, всегда `Connection: close`, `Cache-Control: no-store` |
| `Transfer-Encoding` | запрещён (`400`), нужен `Content-Length` |
| Дубли `Content-Length` | первое значение выигрывает (anti-smuggling) |

### 3.1 Статусы

| Статус | Когда | Код в теле |
|---|---|---|
| `200` | успех, включая «событие отклонено» (это вердикт, не ошибка запроса) | — |
| `400` | битый JSON, неизвестный метод, протокол вне окна, нет `idempotency_key`, плохая форма `ops` | `1000` / `1001` |
| `401` | нет/неверный/отозванный токен | `1002` |
| `403` | чужой аккаунт или тенант, нет scope, vault заперт для метода, который его требует | `1003` / `1004` |
| `404` | неизвестный путь | — |
| `405` | `GET` на `/rpc` | — |
| `409` | конфликт версии/курсора: `since_seq > high_water`, курсор ниже pruned-границы, `prune` за `high_water`, `UPDATE_REJECTED` | `1006` / `1011` |
| `413` | тело/строка больше лимита (проверяется до чтения) | — |
| `429` | rate limit | `1007` |
| `500` | внутренняя ошибка (SQLite, TLS, IO) | `1099` |
| `501` | метод объявлен в протоколе, но не реализован в этой сборке (`tunnel.*`, `unlock`, `pair` по сети, `update.check`) | `1099` |

Тело ошибки — всегда конверт с `ok:false` и `error`, поэтому клиент может
разбирать только тело и всё равно получить код.

---

## 4. Конверт

Запрос:

```json
{"protocol":1,
 "request_id":"01JA01",
 "idempotency_key":"01JA01",
 "method":"sync.push",
 "params":{ }}
```

| Поле | Обязательно | Правила |
|---|---|---|
| `protocol` | нет (по умолчанию 1) | принимается `1..=1`; иначе `400`/`1001` |
| `request_id` | да | 1..=64 символа, возвращается как есть |
| `idempotency_key` | для мутирующих | 1..=128 символов; без него `sync.push`/`sync.prune`/`sync.conflicts` → `400` |
| `method` | да | см. § 5 |
| `params` | да (объект) | `{}` для `hello`/`status`/`shutdown` |

Ответ:

```json
{"protocol":1,"request_id":"01JA01","ok":true,"result":{ },"error":null}
```

Ошибка:

```json
{"protocol":1,"request_id":"01JA01","ok":false,"result":{},
 "error":{"code":1006,"message":"since_seq 42 is beyond high_water 3","retryable":false}}
```

---

## 5. Методы

| Метод | Scope | Мутирующий | Реализован |
|---|---|---|---|
| `hello` | — (без токена) | нет | да |
| `status` | `status` | нет | да |
| `sync.stamp` | `sync:read` | нет | да |
| `sync.pull` | `sync:read` | нет | да — **основной путь клиента** |
| `sync.push` | `sync:write` | да | да — **дополнительный путь** (локальные данные) |
| `sync.conflicts` | `sync:read` | да (при `resolve`) | да |
| `sync.prune` | `sync:write` | да | да |
| `db.status` | `sync:read` | нет | да — контракт § 5.8, гейт «включить синхронизацию» |
| `db.ingest` | `db:ingest` | да | да — внутренний триггер одного прохода (§ 15 п. 11) |
| `db.check`, `db.credentials.rotate` | `admin` | — | **нет**: даже контракта пока нет |
| `shutdown` | `admin` | да | да |
| `pair`, `unlock`, `lock` | — | — | **501**: enrollment только CLI, vault CLI |
| `tunnel.*`, `account.*`, `accounts`, `update.check` | — | — | **501**: шаги 1–2 |

Роли методов после уточнения архитектуры: `sync.pull` — то, что клиент делает
постоянно; `sync.push` — только для данных, которых нет в БД; `db.status` —
гейт «включить синхронизацию» (клиент не включает её, пока БД не подтверждена).

### 5.1 `hello`

Без токена. Возвращает версию, окно протокола и время сервера — этого хватает,
чтобы клиент показал «агент жив» до enrollment.

```json
→ {"protocol":1,"request_id":"r1","method":"hello","params":{}}
← {"protocol":1,"request_id":"r1","ok":true,
   "result":{"agent_version":"0.1.0","protocol":1,"min_client_version":"0.1.0",
             "server_time_ms":1771000000000,"sync":"available"},"error":null}
```

### 5.2 `status`

Требует токен со scope `status`.

```json
← {"result":{"agent_version":"0.1.0","protocol":1,"min_client_version":"0.1.0",
             "vault":"locked","sync":"available","store_schema":2,
             "clients":2,"uptime_ms":12345,"server_time_ms":1771000000000}}
```

`vault: "locked"` — нормальное состояние для `sync.pull`: пассивный журнал
хранит **шифртекст клиента/сервера**, поэтому серверу не нужен DEK, чтобы его
отдавать. Vault запирает сессионный материал (TDLib), а не этот путь. Это
осознанное отклонение от таблицы «`sync.*` нельзя при запертом vault» из
`architecture.md` § 9.4, и оно записано здесь, а не спрятано.

Отдельно: **серверный ingest из БД DEK требует** (§ 1.3), то есть при
`vault: "locked"` публикация стоит, хотя `sync.pull` работает. Это должно быть
видно клиенту через `db.status.vault` (§ 5.8).

### 5.3 `sync.push` (дополнительный путь)

После уточнения архитектуры это **не основной** путь: данные, которые есть в
общей БД, читает и публикует сервер (§ 1.3), а клиент получает их через
`sync.pull`. `sync.push` остаётся для локальных фактов, которых в БД нет
(ghost-настройки, «прочитано в госте»), и их формат не меняется.

Параметры:

| Поле | Тип | Правила |
|---|---|---|
| `tenant` | hex 32 | должен совпадать с привязкой токена; иначе `403` |
| `account` | hex 32 | то же |
| `ops` | массив | ≤ 2000 (`MAX_BATCH_EVENTS`); пустой массив — валидный no-op |

Один op:

| Поле | Тип | Обязательно | Примечание |
|---|---|---|---|
| `kind` | строка или число | да | `"message_new"`/`1` … `"archive_observation"`/`7`; строка предпочтительна |
| `entity_id` | строка | да | `message:<dialog>:<msg>` \| `presence:<peer>` \| `profile:<peer>` \| `archive:<peer>` |
| `field` | строка | по виду | закрытые наборы `MESSAGE_FIELDS`/`PROFILE_FIELDS`/`ARCHIVE_SOURCES` |
| `value` | base64 | нет | байты значения; на клиенте это AEAD-конверт |
| `hlc` | строка `"<physical:016x>-<logical:04x>"` или объект `{"physical_ms":N,"logical":M}` | да | монотонен на устройство |
| `content_hash` | hex 64 | нет | если нет — агент считает сам по алгоритму `store::event::content_hash` |

`device_id` **не передаётся**: агент берёт его из токена. Клиент не может
двигать часы другого устройства.

Пример:

```json
→ {"protocol":1,"request_id":"01JA01","idempotency_key":"01JA01",
   "method":"sync.push",
   "params":{"tenant":"00112233445566778899aabbccddeeff",
             "account":"ffeeddccbbaa99887766554433221100",
             "ops":[{"kind":"message_edit",
                     "entity_id":"message:-1001234567890:42",
                     "field":"text",
                     "value":"aGVsbG8gZWRpdGVk",
                     "hlc":"0000018f3c1a2b40-0000"}]}}

← {"protocol":1,"request_id":"01JA01","ok":true,
   "result":{"account":"ffeeddccbbaa99887766554433221100",
             "results":[{"index":0,"verdict":"applied","op_seq":1}],
             "applied":1,"duplicates":0,"rejected":0,
             "conflicts":0,"conflict_ids":[],
             "high_water":1,
             "account_stamp":{"high_water":1,"last_write_ms":1771000000000,
                              "op_count":1}},
   "error":null}
```

`results` — по одному элементу на вход, в порядке входа:

| `verdict` | Поля | Значение |
|---|---|---|
| `applied` | `op_seq` | событие записано и получило номер |
| `duplicate` | `op_seq` | уже было (тот же `content_hash`); `op_seq` — исходный |
| `rejected` | `reason` | отказ по событию; батч продолжается |

`reason` — стабильные имена из `RejectReason::as_str`: `missing_account`,
`foreign_account`, `malformed_device`, `missing_kind`, `missing_hlc`,
`non_monotonic_hlc`, `malformed_entity_id`, `kind_entity_mismatch`,
`unknown_field`, `missing_field`, `unexpected_field`, `oversized_value`,
`malformed_presence_payload`, `missing_content_hash`, `tombstoned`.

`conflicts`/`conflict_ids` — сколько пар «другое устройство уже записало это поле
с более новым HLC» обнаружено и записано (см. § 8). Конфликт — **не** отказ:
событие применяется и остаётся в журнале.

### 5.4 `sync.pull` (основной путь клиента)

| Поле | Тип | По умолчанию | Правила |
|---|---|---|---|
| `tenant`, `account` | hex | — | как в `push` |
| `since_seq` | число | 0 | `> high_water` → `409`; `< pruned_upto` → `409` |
| `limit` | число | 200 | зажимается в `1..=2000` |

Пример:

```json
→ {"protocol":1,"request_id":"01JA02","method":"sync.pull",
   "params":{"tenant":"0011…","account":"ffee…","since_seq":0,"limit":2}}

← {"protocol":1,"request_id":"01JA02","ok":true,
   "result":{"account":"ffee…","since_seq":0,"next_seq":2,"high_water":3,
             "has_more":true,"pruned_upto":null,
             "ops":[{"op_seq":1,"kind":2,"kind_name":"message_edit",
                     "entity_id":"message:-1001234567890:42","field":"text",
                     "value":"aGVsbG8gZWRpdGVk",
                     "hlc":"0000018f3c1a2b40-0000",
                     "device_id":"bc733612b593fdb12bb2d14b0fd287f1",
                     "content_hash":"9f0e…","applied_at_ms":1771000000000}]},
   "error":null}
```

* `next_seq` — курсор для следующего запроса (последний отданный `op_seq`, при
  пустой странице — `since_seq`);
* `has_more` — есть ли ещё (`next_seq < high_water`); клиент не обязан угадывать
  по размеру страницы;
* `pruned_upto` — граница, до которой история удалена (`sync.prune`).

Пагинация стабильна: `op_seq` уникален и монотонен на аккаунт, страница — это
`WHERE op_seq > since_seq ORDER BY op_seq ASC LIMIT n`, поэтому параллельная
запись не сдвигает уже отданные элементы.

### 5.5 `sync.conflicts`

| Поле | Тип | По умолчанию | Примечание |
|---|---|---|---|
| `since_id` | число | 0 | отдаются `conflict_id > since_id` |
| `limit` | число | 200 | зажимается |
| `include_resolved` | bool | `true` | `false` — только неразрешённые |
| `resolve` | массив | нет | `[{"conflict_id":7,"resolved_by":"device-hex"}]` |

```json
← {"result":{"account":"ffee…",
             "conflicts":[{"conflict_id":1,"entity_id":"profile:900",
                           "field":"username","kind":6,"kind_name":"profile_update",
                           "local_op_seq":1,"local_hlc":"0000018f3c1a2b40-0000",
                           "local_device":"dev-a",
                           "remote_op_seq":2,"remote_hlc":"0000018f3c1a2000-0000",
                           "remote_device":"dev-b","detected_at_ms":…,
                           "resolved_by":null}],
             "next_since_id":1,"resolved":0}}
```

`local_*` — что уже лежало в журнале, `remote_*` — пришедшая запись. Обе
остаются в `ops`; агент **никогда** не выбирает победителя (это H7). Решение
записывается в `resolved_by` и не удаляет ни одну из версий.

### 5.6 `sync.prune`

| Поле | Тип | Правила |
|---|---|---|
| `upto_seq` | число | `> high_water` → `409` (не «удалить всё») |

Удаляет `ops` и проекции (`presence`/`profiles`/`archive_obs`) с `op_seq <=
upto_seq` для одного аккаунта. `high_water` **не** сдвигается, а граница
запоминается в `schema_meta` как `pruned_upto:<account>`. После этого
`sync.pull` с курсором ниже границы отвечает `409` с указанием границы — иначе
клиент принял бы «пусто» за «я в актуальном состоянии».

```json
← {"result":{"account":"ffee…","upto_seq":1,
             "pruned":{"ops":1,"presence":0,"profiles":1,"archive":0,"total":2},
             "high_water":2}}
```

### 5.7 `shutdown`

`admin`-токен, ставит флаг остановки: accept-цикл перестаёт принимать, текущие
запросы доигрываются, сторы закрываются (WAL checkpoint). В проде lifecycle
принадлежит systemd, поэтому `shutdown` — это «попросить вежливо», а не
единственный путь.

### 5.8 `db.status` — контракт

**СТАТУС: реализовано.** Метод есть в `api::protocol::Method` (`db.status`), обвязан в
`api::rpc::dispatch`, делает реальный запрос к БД (или отвечает состоянием, если
соединения нет) и возвращает § 5.8-поля целиком. Код —
`src/db/ingest.rs::DbIngest::status`, подключение — `src/db/{postgres,mysql}.rs`,
настройка — [`db-ingest.md`](db-ingest.md).

Зачем отдельный метод, а не поле в `status`: `status` обязан отвечать даже при
недоступной БД (иначе клиент не отличает «агент мёртв» от «агент жив, БД лежит»),
а `db.status` делает реальный запрос к БД и потому может быть медленным или
недоступным.

Запрос (`scope: sync:read`):

```json
→ {"protocol":1,"request_id":"r-db","method":"db.status",
   "params":{"tenant":"00112233445566778899aabbccddeeff",
             "account":"ffeeddccbbaa99887766554433221100"}}
```

Ответ:

```json
← {"protocol":1,"request_id":"r-db","ok":true,
   "result":{"connected":true,"latency_ms":3,"engine":"postgresql",
             "database":"teleforge","reader_user":"teleforge_agent",
             "tls":"verify-full",
             "cursor":{"table":"sync_outbox","column":"id","value":4127},
             "lag_ms":1250,"last_read_ms":1771000000000,
             "tables_ok":true,"last_error":null,"vault":"locked"},
   "error":null}
```

| Поле | Смысл |
|---|---|
| `connected` | есть живое соединение (`SELECT 1` прошёл) |
| `latency_ms` | время этого запроса или подключения |
| `engine`, `database`, `reader_user` | куда и кем подключён (не секрет; пароль не отдаётся никогда) |
| `tls` | `verify-full` / `verify-ca` / `require` / `tunnel` (loopback через SSH-форвард) |
| `cursor` | таблица/колонка и последнее прочитанное значение (`db_cursor`, § 10) |
| `lag_ms` | насколько отстаёт публикация: `now - last_ok_at`; пока проходы падают, `last_ok_at` не движется и лаг растёт |
| `last_read_ms` | время последнего успешного прохода |
| `tables_ok` | хватает ли прав на все нужные колонки (probe `WHERE 1 = 0`) |
| `last_error` | класс ошибки без кредов и без тела строк (`vault_locked`, `connect`, `auth`, `query`, `config`, `table_not_allowed`, `unsupported_table`, …) |
| `vault` | при `locked` серверный ingest не может шифровать: `connected` может быть `true`, а публикация стоять |
| `last_ok_at`, `fails`, `batches`, `applied`, `duplicates`, `skipped` | health-счётчики потока ingest (аддитивны к § 5.8) |

Коды: `200` — состояние известно (в том числе `connected:false` с причиной);
`401` — нет/неверный токен; `403` — нет scope `sync:read` или токен привязан к
другому аккаунту; `501` — метод не реализован (старая сборка).

### 5.9 Порядок запуска (клиент)

Клиент включает синхронизацию только после трёх успешных проверок:

```
1. hello        (без токена)  агент жив, версия и окно протокола
2. status       (токен)       vault, components, listener_fingerprint
3. db.status    (токен)       агент подключён к БД, курсор, lag
   ↓ только если все три зелёные
4. sync.stamp   → high_water
5. sync.pull    страницами до has_more == false
```

Если любой шаг не зелёный — клиент работает **локально** и показывает статус;
никакой синхронизации «в фоне, на всякий случай». Поведение по каждому отказу
(нет агента, `vault: locked`, `connected: false`, `401`, `501`) — в
`client-integration.md` § 3.3.

---

## 6. Лимиты и таймауты

| Лимит | По умолчанию | Флаг | Где проверяется |
|---|---|---|---|
| тело запроса | 8 МиБ | `--max-body-bytes` | до аллокации, `413` |
| строка заголовка | 8 КиБ | — | при чтении, `413` |
| заголовков | 64 | — | при чтении, `400` |
| событий в батче | 2000 | — | до разбора значений, `400` |
| страница `pull` | 200 (≤2000) | `limit` | до SQL |
| одновременных соединений | 32 | `--max-connections` | на `accept`, `503` |
| запросов на устройство | 120 burst / 20 в с | конфиг | после auth, `429` |
| чтение/запись сокета | 10 с / 10 с | конфиг | `TcpStream::set_*_timeout` |
| `value` на событие | 256 КиБ / 4 КиБ / 8 КиБ | — | `ingest`, вердикт `oversized_value` |

Все лимиты — часть контракта: агент говорит клиенту, сколько стоит один запрос,
и клиент пагинирует, а не «попробует и посмотрит».

---

## 7. Идемпотентность и порядок

Два уровня, как в `architecture.md` § 9.3:

| Уровень | Ключ | Что защищает | Где |
|---|---|---|---|
| запрос | `idempotency_key` | повтор после обрыва: агент вернёт сохранённый ответ | `server::IdempotencyStore` (bounded, TTL 10 мин, ключ `device:key`) |
| событие | `content_hash` | даже другой запрос с уже применённым событием безопасен | `UNIQUE(account_id, content_hash)` + дедуп внутри батча |

Повторный `push` с тем же `idempotency_key` возвращает **байт-в-байт** прошлый
ответ; с другим ключом — тот же батч даёт `duplicates: N, applied: 0`.

Порядок:

* `op_seq` выдаётся `UPDATE accounts SET high_water = high_water + 1 RETURNING` в
  той же транзакции, что и вставка → «номер выдан, строки нет» невозможно;
* HLC монотонен на **(account, device)**; устройство, ушедшее назад, получает
  `non_monotonic_hlc`;
* удаление — tombstone: ревизия с HLC ≤ HLC удаления отклоняется
  (`tombstoned`), действительно более новая — записывается.

Серверный ingest из БД должен вести **свой** `device_id` (используется
`server-db`, `db::SERVER_DEVICE_ID`) и свою монотонность HLC, иначе он будет
конкурировать с часами клиентских устройств (§ 15, п. 10). HLC засевается из
`device_clock` при каждом проходе, поэтому перезапуск не выдаёт уже виденный
штамп. `content_hash` для DB-строк считается по строке, а не по HLC журнала
(`docs/db-ingest.md` § 6).

---

## 8. Конфликты без векторных часов

Пассивное событие несёт HLC и `device_id`, но не vclock. Поэтому правило
сформулировано так, чтобы его можно было проверить и объяснить:

> Конфликт — это принятая запись, которую HLC-упорядоченный мерж **выбросил бы**:
> по тому же `(entity, field)` уже есть запись **другого устройства** со строго
> большим HLC.

Что происходит:

1. запись применяется и остаётся в `ops` (журнал append-only — ничего не
   теряется);
2. пара пишется в `conflicts` (v2): оба `op_seq`, оба HLC, оба устройства;
3. `sync.push` возвращает `conflicts`/`conflict_ids`;
4. клиент показывает «две версии», человек решает, `sync.conflicts` c `resolve`
   фиксирует решение.

Одно устройство, правящее своё поле, конфликта не даёт (его HLC монотонен);
более новый HLC от другого устройства — тоже (это просто мерж).

**Два источника данных.** Записи, прочитанные сервером из БД (`device_id` вида
`server-db`), и записи, присланные клиентом, для одного и того же поля почти
всегда получат разные HLC — значит, по определению выше это конфликт, и он
должен быть виден. Политика «клиент пушит только локальные факты» снижает
частоту, но не отменяет её; как считать `content_hash` для DB-строк, чтобы
повтор схлопывался в `duplicate`, — решено в `docs/db-ingest.md` § 6 (хэш по
строке, не по HLC). Верхнеуровневый вопрос «что считать источником истины для
конкретного поля» остаётся за `client-integration.md` § 12.1.

Тесты:
`store::ingest::tests::{a_newer_write_from_another_device_is_kept_and_recorded_as_a_conflict,
a_newer_write_from_another_device_is_not_a_conflict, conflicts_are_per_field_and_per_entity,
a_replayed_batch_does_not_duplicate_its_conflicts}`,
`store::query::tests::conflicts_are_listed_resolved_and_paged`.

---

## 9. Планировщик ретенции

`serve` поднимает один поток `tf-retention`:

* интервал — `retention_interval_secs` (по умолчанию 3600, `0` — выключить);
* политика — `RetentionPolicy` (`max_age_ms` по умолчанию 90 дней,
  `max_ops_per_account`, `max_bytes`); CLI: `--retention-max-age <days>`;
* проходит по всем тенантам на диске плюс по уже открытым сторам;
* **ошибка одного тенанта не останавливает остальные** и не роняет процесс:
  пишется `error`-строка с тенантом и продолжается следующий интервал;
* суммарный отчёт (`ops/presence/profiles/archive/total`) пишется в лог на
  уровне `info`, пустой прогон — на `debug`;
* завершение — по флагу `running`, который ставит `shutdown`, SIGTERM/SIGINT или
  конец `serve`; поток спит слайсами по 20 мс, поэтому остановка мгновенная.

Сигналы (`src/api/signals.rs`): `SIGTERM`/`SIGINT` → `AtomicBool`, unsafe
ровно в одном `libc::signal` на сигнал, обработчик только пишет атомик
(async-signal-safe). На не-Unix — no-op: там `SIGTERM` нет, а прод-таргет —
Linux/systemd.

Тесты: `api::server::tests::retention_runs_and_logs_without_stopping_the_process`,
`store::retention::tests::*`.

### 9.1 Планировщик ingest из БД

`serve` поднимает второй поток `tf-db-ingest` (`spawn_db_ingest`):

* интервал — `db_ingest.interval_secs` / `--ingest-interval` (`0` — выключить,
  это значение по умолчанию); без пары `--ingest-tenant`/`--ingest-account`
  старт с ненулевым интервалом — ошибка конфигурации;
* один проход — `DbIngest::run_pass`: курсор → страница → запечатать → одна
  транзакция (`ops` + `db_cursor`) → health;
* **ошибка прохода не двигает курсор и не роняет поток**: в лог уходит только
  класс (`class=connect|auth|query|vault_locked|…`), кампания повторяется на
  следующем тике; соединение после ошибки сбрасывается, следующий проход
  переподключается;
* завершение — по флагу `running` (те же 20-мс слайсы, что у ретенции);
* наблюдаемость — `db.status` (`last_ok_at`, `lag_ms`, `fails`, `batches`,
  `applied`, `duplicates`, `skipped`).

Тесты: `api::server::tests::{the_ingest_scheduler_is_off_by_default_and_survives_a_failed_pass,
the_full_server_side_cycle_reaches_sync_pull}`, `db::ingest::tests::*`
(см. `docs/db-ingest.md` § 8).

---

## 10. Миграции схемы (v1 → v2 → v3)

`serve`/`self-test` открывают стор через `Store::open`, который:

1. выполняет `SCHEMA_SQL` (текущие таблицы, `INSERT OR IGNORE` версии);
2. выполняет `SCHEMA_V2_SQL` (`CREATE ... IF NOT EXISTS`);
3. выполняет `SCHEMA_V3_SQL` (`db_cursor`);
4. вызывает `store::migrate::run`, который **в одной транзакции** добавляет
   недостающие колонки (`accounts.conflict_high_water`), создаёт недостающие
   таблицы и поднимает `schema_version` до 3.

Свойства:

* идемпотентность проверяется по фактическим объектам (`PRAGMA table_info`,
  `sqlite_master`), а не по номеру версии: «DDL применён, версия не поднята»
  лечится следующим открытием;
* файл с версией **новее** нашей — `Error::Storage("... newer than this
  agent ...")`, а не «попробуем открыть»;
* данные не переписываются: `ALTER TABLE ADD COLUMN ... DEFAULT 0` и
  `CREATE TABLE IF NOT EXISTS`;
* тест «старая БД → новая схема → данные целы»:
  `store::migrate::tests::a_v1_file_is_upgraded_with_its_data_intact`, плюс
  `migrating_twice_changes_nothing`, `an_interrupted_migration_is_repaired_by_the_next_open`,
  `a_file_from_the_future_is_refused_not_guessed`.

Курсор чтения БД (§ 1.3) — это **v3**: таблица `db_cursor(account_id, source,
last_key, updated_at)`, ключ `(account_id, source)`, `last_key` монотонен в SQL.
Шаг `2 → 3 "db-cursor"`; тесты `store::migrate::tests::{a_v2_file_gains_the_db_cursor_and_keeps_its_data,
the_db_cursor_step_is_idempotent, an_interrupted_v3_migration_is_repaired_by_the_next_open}`,
подробности — `docs/db-ingest.md` § 4.

---

## 11. Recovery-код account-level конверта

`keystore` теперь умеет второй обёрткой закрывать тот же DEK кодом
восстановления:

```
code   = 16 случайных байт, показываются один раз
KEK_r  = Argon2id(code, salt' = sha256(salt || LABEL_RECOVERY), cost)
wrap_r = XChaCha20-Poly1305(KEK_r, aad = account || key_id || b'r', DEK)
```

* `create_with_recovery(...)` → `(Envelope, UnlockedSecret, code)`;
* `unlock_with_recovery(account, code, envelope)` → DEK; неверный код →
  `Error::Auth("wrong recovery code")`, отсутствие обёртки →
  `Error::Auth("this account has no recovery envelope")` — это разные сообщения,
  потому что клиент действует по-разному;
* `rotate` (смена пароля) отказывается молча потерять recovery-обёртку:
  `Error::Unsupported` с указанием, что сначала нужен `recovery-rotate`;
* байт назначения `b'r'` в AAD не даёт перенести recovery-обёртку в слот
  `wrapped_dek` (и наоборот) — тест это фиксирует.

Тесты: `keystore::tests::{a_recovery_code_unlocks_the_dek_without_the_passphrase,
a_wrong_recovery_code_is_refused, an_account_without_a_recovery_envelope_says_so,
a_recovery_wrap_cannot_be_moved_into_the_passphrase_slot,
a_recovery_envelope_survives_a_json_roundtrip,
rotating_refuses_to_silently_drop_the_recovery_wrap,
two_recovery_codes_are_independent_and_debug_hides_them}`,
живая проверка в `--self-test` (`keystore.recovery`).

---

## 12. CLI

```bash
teleforge-agent serve [--data-dir DIR] [--listen 127.0.0.1:PORT | socket]
                      [--tls-cert PATH] [--tls-key PATH] [--allow-plaintext]
                      [--max-connections N] [--max-body-bytes N]
                      [--retention-interval SECS] [--retention-max-age DAYS]
                      [--ingest-interval SECS] [--ingest-tenant HEX]
                      [--ingest-account HEX] [--ingest-table NAME]
teleforge-agent pair --label NAME [--tenant HEX] [--account HEX] [--role reader|ingest]
teleforge-agent self-test [--tenant HEX]
teleforge-agent status
teleforge-agent version | help
```

* `serve --help` печатает общую справку и **не** падает на «неизвестном флаге» —
  установщик зовёт его на незнакомой сборке;
* без TLS-идентичности и без `--allow-plaintext` `serve` отказывается стартовать
  (иначе bearer-токен уходит в открытом виде любому локальному процессу);
* `--allow-plaintext` — для тестов и первого запуска до установки identity;
  в лог уходит `warn`;
* `pair` пишет `auth/clients.json` и печатает токен один раз (см. § 2.2).
  `--role ingest` — внутренний токен для `db.ingest`; по умолчанию `reader`.

Коды выхода: `0` ok, `2` usage/invalid, `3` locked, `4` auth, `5` config,
`6` update, `7` self-test failed, `70` internal.

Планируемые команды (в коде нет): `db-check` (проверить подключение и права,
§ 2.6) и `db-credentials rotate` (перевыпустить пароль, § 15, п. 5).
Scheduler-DB (ingest) настраивается флагами `serve`, см. § 9.1 и
[`db-ingest.md`](db-ingest.md) § 5.

---

## 13. Что не реализовано и почему

| Не сделано | Почему / где |
|---|---|
| Разбор облачной схемы: `messages`/`edited_messages`/`deleted_messages`/`online_events`/`peer_archive_*` как источник ingest | Таблицы и колонки в allowlist объявлены, но отображение «строка → события» — решение о модели (`client-integration.md` § 12.9). Сейчас читается `sync_outbox`; выбор другой таблицы → `class=unsupported_table`. `docs/db-ingest.md` § 3.2, § 7 |
| `db.check`, `db.credentials set/rotate` | Не начато; сейчас слот пишет установщик (`docs/db-ingest.md` § 2) |
| Несколько таблиц за один проход | Один `DbSource` — одна таблица и один курсор; N таблиц = N проходов (ключ `db_cursor` уже это допускает) |
| Публикация зашифрованных шардов в облако/канал | шаг 4/6: `updater` и `sync::transport` — заглушки |
| Креды БД из `agent.toml` | Осознанно нет: строка подключения только в vault (§ 2.6 п. 5) |
| «vault разблокирован» как состояние процесса после `serve` | `unlock` по сети — `501`, CLI `unlock` не отдаёт ключ демону; пока демон не разблокирован, ingest стоит с `class=vault_locked` (§ 15 п. 6) |
| `tunnel.*` | шаг 1; нужен `russh`, не транспорт |
| `unlock`/`lock` по сети, `pair` по сети | vault CLI-проводка — шаг 2; enrollment пока через `pair` на сервере |
| `accounts`, `account.*`, `update.check` | TDLib и updater — шаги 2/6 |
| unix-socket listener | `bind` возвращает `NotImplemented` вместо тихого фолбэка на TCP: фолбэк был бы другой экспозицией |
| `listen = network` + mTLS | конфиг валидируется, listener — `NotImplemented`: агент ставится через SSH-туннель |
| keep-alive, chunked, HTTP/2, сжатие | см. § 1.1: поверхность парсера без выгоды при батчах |
| подпись Ed25519 на запрос | см. § 2.1: токен + TLS закрывают модель; `ReplayWindow` — задел |
| TLS-сертификат от CA | пиннинг self-signed fingerprint'а; CA для IP на VPS пользователя — лишняя утечка и лишний шаг |

---

## 14. Как проверить

```bash
cd rust/teleforge-agent
cargo fmt --check
cargo clippy --all-targets -- -D warnings
cargo test                       # 412 тестов
cargo run --bin teleforge-agent -- self-test --data-dir /tmp/tf
cargo run --bin teleforge-agent -- serve --help
```

End-to-end (реальный listener, `127.0.0.1:0`, TLS с пиннингом сертификата из
`api::tls::test_identity`):

```bash
cargo test --lib api::server
```

Покрыто: полный цикл `push → pull` по TLS, идемпотентность повторного `push`
(и по `idempotency_key`, и по `content_hash`), отказ чужому аккаунту/тенанту
(403), пагинация, `409` на курсор за `high_water` и на pruned-курсор, `400` на
битый JSON, `401` на неверный токен, `413` на большое тело, `429` на rate limit,
конфликты и `prune` по сети, лимит соединений, отказ стартовать без TLS.

Серверный контур (`src/db/**`) покрыт отдельно, через `db::mock::MockSource`
(живой PostgreSQL/MySQL в `cargo test` не нужен): `db.status` по сети с полями
контракта и `401` без токена, читатель не может запустить `db.ingest`, ингест-
токен может, полный цикл «БД → `ops` → `sync.pull`», перезапуск с курсора,
ошибка БД/запертый vault без потери курсора, запрет `personality_core` /
`openai_api_key` / `pgConnString`. Полный список — [`db-ingest.md`](db-ingest.md) § 8.

Ручная проверка без клиента:

```bash
cargo run --bin teleforge-agent -- pair --label test \
  --tenant 00112233445566778899aabbccddeeff \
  --account ffeeddccbbaa99887766554433221100 --data-dir /tmp/tf
cargo run --bin teleforge-agent -- serve --data-dir /tmp/tf --allow-plaintext &
curl -s http://127.0.0.1:8137/rpc \
  -H 'Authorization: Bearer <token>' \
  -d '{"protocol":1,"request_id":"r1","method":"sync.stamp",
       "params":{"tenant":"00112233445566778899aabbccddeeff",
                 "account":"ffeeddccbbaa99887766554433221100"}}'
```

### 14.1 Что в коде расходится с уточнённой моделью

Полный список — § 15. Серверный путь «агент читает БД» покрыт тестами через
`db::mock::MockSource` (оркестрация, курсор, транзакции, `db.status`,
`sync.pull`), но **не** против живой PostgreSQL/MySQL: для этого нужен
интеграционный стенд, а не `cargo test` (`docs/db-ingest.md` § 8). Отображение
облачных таблиц (`messages` и др.) остаётся открытым (§ 15 п. 2).

---

## 15. Расхождения кода с моделью «сервер читает БД, клиент читает сервер»

Список ведётся здесь, чтобы следующая итерация началась с известного объёма
работы. **Колонка «статус» обновлена в этой итерации**: `закрыто` — сделано и
покрыто тестами, `частично` — работает, но с оговоркой из «где»/«что осталось».

| # | Что в коде | Как должно быть по уточнению | Где | Статус |
|---|---|---|---|---|
| 1 | Нет подключения к PG/MySQL | Агент подключается по TLS, читает изменения по монотонному курсору | `src/db/**` | **закрыто**: `db::{postgres,mysql}`, `sslmode=verify-full`, `SELECT` по allowlist, probe грантов |
| 2 | Единственный источник `ops` — HTTP `sync.push` | Основной источник — БД; push только для локальных фактов | `src/db/ingest.rs`, `src/api/server.rs` | **частично**: поток `tf-db-ingest` публикует из `sync_outbox`; отображение остальных таблиц § 2.6 — открыто (`db-ingest.md` § 3.2) |
| 3 | Нет `db.status` | Метод § 5.8, гейт «включить синхронизацию» | `src/api/protocol.rs`, `src/api/rpc.rs` | **закрыто**: § 5.8 целиком, `sync:read`, `401`/`403` по общим правилам |
| 4 | Нет курсора чтения БД | Таблица/колонка курсора + outbox-политика | `src/store/schema.rs` (v3), `src/store/cursor.rs` | **закрыто**: `db_cursor(account_id, source, last_key)`, монотонный в SQL, пишется в одной транзакции с `ops` |
| 5 | Параметров БД нет в `AgentConfig`; `agent.toml` не парсится | host/port/user/dbname/sslmode в конфиге, пароль — в vault | `src/config.rs`, `src/db/conninfo.rs` | **частично**: параметры читаются из строки подключения в vault (`--ingest-*` задают только tenant/account/таблицу); `agent.toml` по-прежнему не парсится |
| 6 | У `vault` нет API для строки подключения; `unlock` не проводён в `serve` | Креды БД кладутся в keystore, демон обязан быть разблокирован для ingest | `src/db/credentials.rs` | **частично**: слот `db/credentials.enc` под `LABEL_DB_CONN` есть (`store`/`load`); CLI-команд нет, `unlock` в `serve` не проводён → без in-process unlock ингест стоит с `class=vault_locked` |
| 7 | Транспорт не использует `subkey(LABEL_SYNC_DATA)` | Серверный ingest шифрует DB-строки тем же ключом | `src/db/ingest.rs` | **закрыто**: `aead::seal(subkey(LABEL_SYNC_DATA), aad = адрес строки)`; присутствие (`presence_*`) идёт 8-байтным payload'ом — так его валидирует `store::ingest` (`db-ingest.md` § 4) |
| 8 | Нет публикации в облако/канал | Зашифрованные шарды публикуются (шаг 4/6) | `src/sync/*`, `src/updater.rs` | открыто |
| 9 | `serve` запускает только ретенцию | Плюс периодический ingest из БД и его health | `src/api/server.rs` | **закрыто**: `spawn_db_ingest` + `run_db_ingest_once`, health в `db.status` |
| 10 | `Method::scope()` не знает `db.status`; `sync.push` и `sync.pull` не различают источник | `db.*` со scope `sync:read`/`db:ingest`, `device_id = server-db` | `src/api/protocol.rs`, `src/db/mod.rs` | **закрыто**: `db.status` → `sync:read`, `db.ingest` → `db:ingest`, `SERVER_DEVICE_ID = "server-db"` |
| 11 | Токен `pair` привязан только к tenant+account | Явная модель: токен клиента — только чтение/локальный push; ingest — внутренний | `src/auth.rs`, `src/cli.rs` | **закрыто**: `TokenRole{reader,ingest}`, `pair --role`, reader не может нести `db:ingest` (проверяется при enroll, при разборе файла и в `allows`) |
| 12 | `docs/architecture.md` § 7.6, § 9.4, § 10 и `migration.md` шаг 3 описывают клиент как источник | Обновить под два режима и новую модель доверия | `docs/architecture.md`, `docs/migration.md` | открыто (документы вне этой итерации) |

Пункты 1, 3, 4, 7, 9, 10 и 11 закрыты: чтение БД, `db.status`, курсор v3,
серверное шифрование, поток ingest, разделение scope/device и роли токенов есть
и покрыты тестами. Шаг 3 (`shadow`) разблокирован в pull-first варианте: клиенту
есть что сверять (`db.status` + `sync.pull`). Открытым остаётся **отображение
облачных таблиц** (§ 2.6) — без него публикуется только `sync_outbox`
(`docs/db-ingest.md` § 3.2, § 7).
