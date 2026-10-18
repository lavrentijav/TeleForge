#include "tfc/features/sync/teleforge_sync_pg.h"

#include "tfc/features/teleforge/teleforge_core.h"
#include "data/data_user.h"
#include "main/main_session.h"

#include <QtCore/QByteArray>
#include <QtCore/QCryptographicHash>

#ifdef TELEFORGE_WITH_POSTGRES

#include "tfc/features/sync/teleforge_ssh_tunnel.h"
#include "tfc/features/sync/teleforge_sync_crypto.h"
#include "tfc/features/sync/teleforge_sync_embeddings.h"
#include "tfc/features/sync/teleforge_sync_rows.h"
#include "tfc/features/teleforge/teleforge_embeddings.h"

#include "logs.h"

#include <crl/crl.h>

#include <algorithm>
#include <string>
#include <unordered_set>

#include <QtCore/QDateTime>
#include <QtCore/QString>
#include <QtCore/QByteArrayList>
#include <QtCore/QStringList>

#include <libpq-fe.h>

#endif // TELEFORGE_WITH_POSTGRES

namespace TeleForge::Sync {

QString PgTenantId(not_null<Main::Session*> session) {
	// Opaque, stable per-account identity. Any device logged into the same
	// account derives the same tenant id; other accounts derive a different
	// one, and the blinded ids + encrypted payloads keep them isolated.
	const auto material = QByteArray::number(session->user()->id.value)
		+ ':'
		+ session->user()->phone().toUtf8();
	return QString::fromLatin1(
		QCryptographicHash::hash(material, QCryptographicHash::Sha256)
			.toHex());
}

#ifdef TELEFORGE_WITH_POSTGRES

namespace {

// Relational schema. Every captured message is its own row with an encrypted,
// text-sized payload; ids are blinded (HMAC) so they can be indexed without
// leaking real values. `teleforge_events` is an append-only, idempotent audit
// log (created / deleted / edited). The plaintext-projected embeddings table is
// kept alongside for server-side similarity search.
constexpr auto kSchemaSql =
	"CREATE TABLE IF NOT EXISTS teleforge_messages ("
	"tenant_id TEXT NOT NULL,"
	"content_hash TEXT NOT NULL,"
	"chat_id_h TEXT,"
	"account_id_h TEXT,"
	"message_id_h TEXT,"
	"deleted BOOLEAN,"
	"edited BOOLEAN,"
	"date BIGINT,"
	"edit_date BIGINT,"
	"payload TEXT NOT NULL,"
	"updated_at BIGINT,"
	"PRIMARY KEY (tenant_id, content_hash));"
	"CREATE INDEX IF NOT EXISTS idx_tf_msg_chat ON teleforge_messages(tenant_id, chat_id_h);"
	"CREATE INDEX IF NOT EXISTS idx_tf_msg_account ON teleforge_messages(tenant_id, account_id_h);"
	"CREATE INDEX IF NOT EXISTS idx_tf_msg_msgid ON teleforge_messages(tenant_id, message_id_h);"
	"CREATE INDEX IF NOT EXISTS idx_tf_msg_deleted ON teleforge_messages(tenant_id, deleted);"
	"CREATE INDEX IF NOT EXISTS idx_tf_msg_date ON teleforge_messages(tenant_id, date);"
	"CREATE TABLE IF NOT EXISTS teleforge_events ("
	"tenant_id TEXT NOT NULL,"
	"content_hash TEXT NOT NULL,"
	"event_type TEXT NOT NULL,"
	"chat_id_h TEXT,"
	"account_id_h TEXT,"
	"message_id_h TEXT,"
	"payload TEXT,"
	"created_at BIGINT,"
	"PRIMARY KEY (tenant_id, content_hash, event_type));"
	"CREATE INDEX IF NOT EXISTS idx_tf_evt_type ON teleforge_events(tenant_id, event_type);"
	"CREATE INDEX IF NOT EXISTS idx_tf_evt_chat ON teleforge_events(tenant_id, chat_id_h);"
	"CREATE INDEX IF NOT EXISTS idx_tf_evt_msgid ON teleforge_events(tenant_id, message_id_h);"
	"CREATE INDEX IF NOT EXISTS idx_tf_evt_created ON teleforge_events(tenant_id, created_at);";

// Freshness fingerprint: one tiny row per tenant so a peer can tell whether it
// is behind without pulling the whole dataset. Refreshed by every upload.
constexpr auto kCreateStampTableSql =
	"CREATE TABLE IF NOT EXISTS teleforge_sync_state ("
	"tenant_id TEXT PRIMARY KEY,"
	"revision BIGINT NOT NULL,"
	"updated_at BIGINT NOT NULL,"
	"sha256 TEXT NOT NULL,"
	"date_from BIGINT,"
	"date_to BIGINT)";

constexpr auto kUpsertStampSql =
	"INSERT INTO teleforge_sync_state"
	" (tenant_id,revision,updated_at,sha256,date_from,date_to)"
	" VALUES ($1,$2,$3,$4,$5,$6)"
	" ON CONFLICT (tenant_id) DO UPDATE SET"
	" revision=EXCLUDED.revision,"
	" updated_at=EXCLUDED.updated_at,"
	" sha256=EXCLUDED.sha256,"
	" date_from=EXCLUDED.date_from,"
	" date_to=EXCLUDED.date_to"
	" WHERE teleforge_sync_state.revision <= EXCLUDED.revision";

constexpr auto kSelectStampSql =
	"SELECT sha256, revision FROM teleforge_sync_state WHERE tenant_id=$1";

// Plaintext, queryable embeddings table (kept alongside the encrypted blob).
// `embedding` is a real[] array — portable and inspectable without pgvector;
// pgvector can be layered on top later if server-side ANN search is wanted.
constexpr auto kCreateEmbeddingsTableSql =
	"CREATE TABLE IF NOT EXISTS teleforge_embeddings ("
	"tenant_id TEXT NOT NULL,"
	"memory_id_h TEXT NOT NULL,"
	"model_id TEXT,"
	"dims INT,"
	"embedding REAL[],"
	"scope_type TEXT,"
	"chat_id_h TEXT,"
	"user_id_h TEXT,"
	"title_enc TEXT,"
	"summary_enc TEXT,"
	"updated_at BIGINT,"
	"PRIMARY KEY (tenant_id, memory_id_h));"
	"CREATE INDEX IF NOT EXISTS idx_tf_emb_scope ON teleforge_embeddings(tenant_id, scope_type);"
	"CREATE INDEX IF NOT EXISTS idx_tf_emb_chat ON teleforge_embeddings(tenant_id, chat_id_h);";

// Rows per multi-row INSERT batch: one round-trip per batch, not per row.
constexpr auto kUploadBatch = 100;

// Single-quoted SQL literal (values are HMAC hex / base64 / numbers; escape
// defensively anyway).
[[nodiscard]] QByteArray PgQuote(QByteArray in) {
	in.replace('\'', "''");
	return "'" + in + "'";
}

constexpr auto kSelectMessagesSql =
	"SELECT content_hash,deleted,edited,payload"
	" FROM teleforge_messages WHERE tenant_id=$1";

constexpr auto kEmbeddingUpsertSql =
	"INSERT INTO teleforge_embeddings (tenant_id,memory_id_h,model_id,dims,"
	"embedding,scope_type,chat_id_h,user_id_h,title_enc,summary_enc,updated_at)"
	" VALUES ($1,$2,$3,$4,$5::real[],$6,$7,$8,$9,$10,$11)"
	" ON CONFLICT (tenant_id,memory_id_h) DO UPDATE SET"
	" model_id=EXCLUDED.model_id, dims=EXCLUDED.dims,"
	" embedding=EXCLUDED.embedding, scope_type=EXCLUDED.scope_type,"
	" chat_id_h=EXCLUDED.chat_id_h, user_id_h=EXCLUDED.user_id_h,"
	" title_enc=EXCLUDED.title_enc, summary_enc=EXCLUDED.summary_enc,"
	" updated_at=EXCLUDED.updated_at";

// PostgreSQL array literal, e.g. {0.12,-0.34,...}
[[nodiscard]] QByteArray VectorToPgArray(const std::vector<float> &v) {
	auto parts = QStringList();
	parts.reserve(int(v.size()));
	for (const auto f : v) {
		parts.push_back(QString::number(f, 'g', 9));
	}
	return (u"{"_q + parts.join(',') + u"}"_q).toUtf8();
}

[[nodiscard]] QString ConnStringFromSettings() {
	const auto core = LoadPersonalityCore();
	return core ? core->pgConnString : QString();
}

// Opens a libpq connection; returns nullptr and fills `error` on failure.
// Caller owns the returned connection and must PQfinish() it. Runs off the
// main thread (see teleforge_ssh_tunnel.h), so it may block briefly while an
// `ssh -L` tunnel to the DB host is established.
[[nodiscard]] PGconn *Connect(const QString &connStringIn, QString &error) {
	if (connStringIn.trimmed().isEmpty()) {
		error = u"Строка подключения PostgreSQL не задана."_q;
		return nullptr;
	}
	const auto connString = Ssh::ApplyTunnelIfConfigured(
		connStringIn,
		5432,
		error);
	if (connString.isEmpty()) {
		return nullptr;
	}
	auto conn = PQconnectdb(connString.toUtf8().constData());
	if (!conn || PQstatus(conn) != CONNECTION_OK) {
		error = conn
			? QString::fromUtf8(PQerrorMessage(conn)).trimmed()
			: u"Не удалось выделить подключение."_q;
		if (conn) {
			PQfinish(conn);
		}
		return nullptr;
	}
	return conn;
}

[[nodiscard]] bool EnsureSchema(PGconn *conn, QString &error) {
	for (const auto *ddl : {
			kSchemaSql,
			kCreateStampTableSql,
			kCreateEmbeddingsTableSql }) {
		auto res = PQexec(conn, ddl);
		const auto ok = res && (PQresultStatus(res) == PGRES_COMMAND_OK);
		if (!ok) {
			error = QString::fromUtf8(PQerrorMessage(conn)).trimmed();
		}
		if (res) {
			PQclear(res);
		}
		if (!ok) {
			return false;
		}
	}
	return true;
}

// Refreshes the tenant fingerprint inside the open transaction: a sorted hash of
// the replicated rows changes exactly when this account's dataset changes.
[[nodiscard]] bool DoUpdateStamp(
		PGconn *conn,
		const QString &tenantId,
		const std::vector<SyncMessageRow> &rows,
		const QByteArray &now) {
	auto hashes = std::vector<QByteArray>();
	hashes.reserve(rows.size());
	auto dateFrom = rows.front().date;
	auto dateTo = rows.front().date;
	for (const auto &row : rows) {
		hashes.push_back(row.contentHash.toUtf8()
			+ (row.deleted ? 'D' : 'c')
			+ (row.edited ? 'E' : 'e'));
		if (row.date < dateFrom) {
			dateFrom = row.date;
		}
		if (row.date > dateTo) {
			dateTo = row.date;
		}
	}
	std::sort(hashes.begin(), hashes.end());
	auto material = QByteArray();
	for (const auto &hash : hashes) {
		material += hash;
		material += '\n';
	}
	const auto sha = QCryptographicHash::hash(
		material,
		QCryptographicHash::Sha256).toHex();
	const auto revision = QByteArray::number(QDateTime::currentSecsSinceEpoch());
	const auto from = QByteArray::number(dateFrom);
	const auto to = QByteArray::number(dateTo);
	const auto tenant = tenantId.toUtf8();
	const char *values[6] = {
		tenant.constData(),
		revision.constData(),
		now.constData(),
		sha.constData(),
		from.constData(),
		to.constData(),
	};
	auto res = PQexecParams(
		conn,
		kUpsertStampSql,
		6,
		nullptr,
		values,
		nullptr,
		nullptr,
		0);
	const auto ok = res && (PQresultStatus(res) == PGRES_COMMAND_OK);
	if (res) {
		PQclear(res);
	}
	return ok;
}

// Blocking; runs on a worker thread. Upserts messages and appends audit events
// in multi-row batches (kUploadBatch rows per statement) inside one transaction.
[[nodiscard]] bool DoUploadMessages(
		PGconn *conn,
		const QString &tenantId,
		const std::vector<SyncMessageRow> &rows,
		QString &error,
		const Fn<void(int)> &onRow) {
	if (rows.empty()) {
		return true;
	}
	PQclear(PQexec(conn, "BEGIN"));
	const auto tenant = PgQuote(tenantId.toUtf8());
	const auto now = QByteArray::number(QDateTime::currentSecsSinceEpoch());
	const auto fail = [&] {
		error = QString::fromUtf8(PQerrorMessage(conn)).trimmed();
		PQclear(PQexec(conn, "ROLLBACK"));
		return false;
	};
	const auto exec = [&](const QByteArray &sql) {
		auto res = PQexec(conn, sql.constData());
		const auto ok = res && (PQresultStatus(res) == PGRES_COMMAND_OK);
		if (res) {
			PQclear(res);
		}
		return ok;
	};
	auto seen = std::unordered_set<std::string>();
	for (auto start = std::size_t(0); start < rows.size(); start += kUploadBatch) {
		const auto stop = std::min(start + std::size_t(kUploadBatch), rows.size());
		auto msgValues = QByteArrayList();
		auto evtValues = QByteArrayList();
		for (auto i = start; i != stop; ++i) {
			const auto &row = rows[i];
			if (!seen.insert(row.contentHash.toStdString()).second) {
				continue; // avoids "ON CONFLICT affects row a second time"
			}
			const auto ch = PgQuote(row.contentHash.toUtf8());
			const auto chat = PgQuote(row.chatIdH.toUtf8());
			const auto acc = PgQuote(row.accountIdH.toUtf8());
			const auto mid = PgQuote(row.messageIdH.toUtf8());
			const auto payload = PgQuote(row.payload.toBase64());
			const auto date = QByteArray::number(row.date);
			const auto editDate = QByteArray::number(row.editDate);
			const auto del = QByteArray(row.deleted ? "true" : "false");
			const auto edt = QByteArray(row.edited ? "true" : "false");
			msgValues.push_back("(" + tenant + "," + ch + "," + chat + ","
				+ acc + "," + mid + "," + del + "," + edt + "," + date + ","
				+ editDate + "," + payload + "," + now + ")");
			const auto addEvt = [&](const char *type) {
				evtValues.push_back("(" + tenant + "," + ch + ",'"
					+ QByteArray(type) + "'," + chat + "," + acc + "," + mid
					+ "," + payload + "," + now + ")");
			};
			addEvt("created");
			if (row.deleted) {
				addEvt("deleted");
			}
			if (row.edited) {
				addEvt("edited");
			}
		}
		if (!msgValues.isEmpty()) {
			const auto sql = QByteArray(
				"INSERT INTO teleforge_messages (tenant_id,content_hash,"
				"chat_id_h,account_id_h,message_id_h,deleted,edited,date,"
				"edit_date,payload,updated_at) VALUES ")
				+ msgValues.join(',')
				+ " ON CONFLICT (tenant_id,content_hash) DO UPDATE SET"
				" deleted = teleforge_messages.deleted OR EXCLUDED.deleted,"
				" edited = teleforge_messages.edited OR EXCLUDED.edited,"
				" edit_date = EXCLUDED.edit_date, payload = EXCLUDED.payload,"
				" updated_at = EXCLUDED.updated_at";
			if (!exec(sql)) {
				return fail();
			}
		}
		if (!evtValues.isEmpty()) {
			const auto sql = QByteArray(
				"INSERT INTO teleforge_events (tenant_id,content_hash,"
				"event_type,chat_id_h,account_id_h,message_id_h,payload,"
				"created_at) VALUES ")
				+ evtValues.join(',')
				+ " ON CONFLICT (tenant_id,content_hash,event_type) DO NOTHING";
			if (!exec(sql)) {
				return fail();
			}
		}
		if (onRow) {
			onRow(int(stop));
		}
	}
	if (!DoUpdateStamp(conn, tenantId, rows, now)) {
		return fail();
	}
	PQclear(PQexec(conn, "COMMIT"));
	return true;
}

// Blocking; runs on a worker thread. Ids are blinded (HMAC), the vector is
// orthogonally projected (cosine preserved, raw vector hidden) and title/
// summary are encrypted — the row stays useful for same-account similarity
// search but opaque to anyone without the key.
void UploadEmbeddings(
		PGconn *conn,
		const QString &tenantId,
		const std::vector<EmbeddingRow> &embeddings,
		const QByteArray &key) {
	if (embeddings.empty()) {
		return;
	}
	auto err = QString();
	PQclear(PQexec(conn, "BEGIN"));
	const auto tenant = tenantId.toUtf8();
	for (const auto &row : embeddings) {
		const auto memoryIdH = HmacId(row.memoryId, key).toUtf8();
		const auto modelId = row.modelId.toUtf8();
		const auto dims = QByteArray::number(row.dims);
		const auto vec = VectorToPgArray(
			ProjectEmbedding(DeserializeEmbedding(row.vectorBlob), key));
		const auto scope = row.scopeType.toUtf8();
		const auto chatIdH = row.chatId
			? HmacId(*row.chatId, key).toUtf8()
			: QByteArray();
		const auto userIdH = row.userId
			? HmacId(*row.userId, key).toUtf8()
			: QByteArray();
		const auto titleEnc = EncryptBundle(row.title.toUtf8(), key).toBase64();
		const auto summaryEnc = EncryptBundle(
			row.summary.toUtf8(), key).toBase64();
		const auto updatedAt = QByteArray::number(row.updatedAt);
		const char *values[11] = {
			tenant.constData(),
			memoryIdH.constData(),
			modelId.constData(),
			dims.constData(),
			vec.constData(),
			scope.constData(),
			row.chatId ? chatIdH.constData() : nullptr,
			row.userId ? userIdH.constData() : nullptr,
			titleEnc.constData(),
			summaryEnc.constData(),
			updatedAt.constData(),
		};
		auto res = PQexecParams(
			conn, kEmbeddingUpsertSql, 11, nullptr, values, nullptr, nullptr, 0);
		if (!res || PQresultStatus(res) != PGRES_COMMAND_OK) {
			err = QString::fromUtf8(PQerrorMessage(conn)).trimmed();
			if (res) {
				PQclear(res);
			}
			break;
		}
		PQclear(res);
	}
	if (err.isEmpty()) {
		PQclear(PQexec(conn, "COMMIT"));
	} else {
		PQclear(PQexec(conn, "ROLLBACK"));
		LOG(("TeleForge Sync PG: embeddings upload failed: %1").arg(err));
	}
}

// Blocking; runs on a worker thread.
[[nodiscard]] bool DoDownloadMessages(
		PGconn *conn,
		const QString &tenantId,
		std::vector<SyncMessageRow> &out,
		QString &error) {
	const auto tenant = tenantId.toUtf8();
	const char *values[1] = { tenant.constData() };
	auto res = PQexecParams(
		conn, kSelectMessagesSql, 1, nullptr, values, nullptr, nullptr, 0);
	if (!res || PQresultStatus(res) != PGRES_TUPLES_OK) {
		error = QString::fromUtf8(PQerrorMessage(conn)).trimmed();
		if (res) {
			PQclear(res);
		}
		return false;
	}
	const auto n = PQntuples(res);
	out.reserve(out.size() + n);
	for (auto i = 0; i != n; ++i) {
		auto row = SyncMessageRow{};
		row.contentHash = QString::fromUtf8(PQgetvalue(res, i, 0));
		row.deleted = (QByteArray(PQgetvalue(res, i, 1)) == "t");
		row.edited = (QByteArray(PQgetvalue(res, i, 2)) == "t");
		row.payload = QByteArray::fromBase64(QByteArray(PQgetvalue(res, i, 3)));
		out.push_back(std::move(row));
	}
	PQclear(res);
	return true;
}

[[nodiscard]] bool DoFetchStamp(
		const QString &connString,
		const QString &tenantId,
		QString &shaOut,
		qint64 &revisionOut,
		QString &error) {
	auto conn = Connect(connString, error);
	if (!conn) {
		return false;
	}
	auto guard = gsl::finally([&] { PQfinish(conn); });
	if (!EnsureSchema(conn, error)) {
		return false;
	}

	const auto tenant = tenantId.toUtf8();
	const char *values[1] = { tenant.constData() };
	const int lengths[1] = { 0 };
	const int formats[1] = { 0 };
	auto res = PQexecParams(
		conn,
		kSelectStampSql,
		1,
		nullptr,
		values,
		lengths,
		formats,
		0);
	if (!res || PQresultStatus(res) != PGRES_TUPLES_OK) {
		error = QString::fromUtf8(PQerrorMessage(conn)).trimmed();
		if (res) {
			PQclear(res);
		}
		return false;
	}
	if (PQntuples(res) > 0) {
		shaOut = QString::fromUtf8(PQgetvalue(res, 0, 0)).trimmed();
		revisionOut = QString::fromUtf8(PQgetvalue(res, 0, 1)).toLongLong();
	}
	PQclear(res);
	return true;
}

} // namespace

void PgFetchRemoteStamp(
		not_null<Main::Session*> session,
		Fn<void(bool ok, QString sha256, qint64 revision, QString error)> done) {
	const auto connString = ConnStringFromSettings();
	const auto tenantId = PgTenantId(session);
	const auto sessionPtr = session.get();
	crl::async([=] {
		auto sha = QString();
		auto revision = qint64(0);
		auto error = QString();
		const auto ok = DoFetchStamp(
			connString,
			tenantId,
			sha,
			revision,
			error);
		crl::on_main(sessionPtr, [=] {
			if (done) {
				done(ok, sha, revision, error);
			}
		});
	});
}

void RunPgUpload(
		not_null<Main::Session*> session,
		const QByteArray &syncKey,
		Fn<void(bool ok, QString error)> done,
		Fn<void(int done, int total)> onProgress) {
	// Build encrypted, id-blinded rows on the main thread (touches SQLite).
	const auto rows = CollectMessageRows(syncKey);
	const auto embeddings = CollectEmbeddingRows();
	const auto connString = ConnStringFromSettings();
	const auto tenantId = PgTenantId(session);
	const auto sessionPtr = session.get();
	const auto total = int(rows.size());
	crl::async([=] {
		auto error = QString();
		auto conn = Connect(connString, error);
		auto ok = false;
		if (conn) {
			const auto onRow = [=](int doneCount) {
				crl::on_main(sessionPtr, [=] {
					if (onProgress) {
						onProgress(doneCount, total);
					}
				});
			};
			ok = EnsureSchema(conn, error)
				&& DoUploadMessages(conn, tenantId, rows, error, onRow);
			if (ok) {
				UploadEmbeddings(conn, tenantId, embeddings, syncKey);
			}
			PQfinish(conn);
		}
		crl::on_main(sessionPtr, [=] {
			if (!ok) {
				LOG(("TeleForge Sync PG: upload failed: %1").arg(error));
			}
			if (done) {
				done(ok, error);
			}
		});
	});
}

void RunPgDownload(
		not_null<Main::Session*> session,
		const QByteArray &syncKey,
		Fn<void(bool ok, QString error)> done) {
	const auto connString = ConnStringFromSettings();
	const auto tenantId = PgTenantId(session);
	const auto sessionPtr = session.get();
	const auto key = syncKey;
	crl::async([=] {
		auto error = QString();
		auto rows = std::vector<SyncMessageRow>();
		auto conn = Connect(connString, error);
		auto ok = false;
		if (conn) {
			ok = EnsureSchema(conn, error)
				&& DoDownloadMessages(conn, tenantId, rows, error);
			PQfinish(conn);
		}
		crl::on_main(sessionPtr, [=] {
			if (!ok) {
				LOG(("TeleForge Sync PG: download failed: %1").arg(error));
				if (done) {
					done(false, error);
				}
				return;
			}
			// Merge into the local DB on the main thread (touches SQLite). The
			// remote is only a replica — this never removes local data.
			ApplyMessageRows(rows, key);
			if (done) {
				done(true, QString());
			}
		});
	});
}

void PgTestConnection(
		const QString &connString,
		Fn<void(bool ok, QString error)> done) {
	crl::async([connString, done] {
		auto error = QString();
		auto conn = Connect(connString, error);
		const auto ok = (conn != nullptr) && EnsureSchema(conn, error);
		if (conn) {
			PQfinish(conn);
		}
		crl::on_main([=] {
			if (done) {
				done(ok, error);
			}
		});
	});
}

#else // TELEFORGE_WITH_POSTGRES

namespace {

[[nodiscard]] QString NotCompiledError() {
	return u"Поддержка PostgreSQL не включена в этой сборке."_q;
}

} // namespace

void RunPgUpload(
		not_null<Main::Session*> session,
		const QByteArray &syncKey,
		Fn<void(bool ok, QString error)> done,
		Fn<void(int done, int total)> onProgress) {
	if (done) {
		done(false, NotCompiledError());
	}
}

void RunPgDownload(
		not_null<Main::Session*> session,
		const QByteArray &syncKey,
		Fn<void(bool ok, QString error)> done) {
	if (done) {
		done(false, NotCompiledError());
	}
}

void PgTestConnection(
		const QString &connString,
		Fn<void(bool ok, QString error)> done) {
	if (done) {
		done(false, NotCompiledError());
	}
}

void PgFetchRemoteStamp(
		not_null<Main::Session*> session,
		Fn<void(bool ok, QString sha256, qint64 revision, QString error)> done) {
	if (done) {
		done(false, QString(), 0, NotCompiledError());
	}
}

#endif // TELEFORGE_WITH_POSTGRES

} // namespace TeleForge::Sync
