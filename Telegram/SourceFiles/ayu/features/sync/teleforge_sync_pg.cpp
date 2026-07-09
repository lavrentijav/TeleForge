#include "ayu/features/sync/teleforge_sync_pg.h"

#include "ayu/features/teleforge/teleforge_core.h"
#include "data/data_user.h"
#include "main/main_session.h"

#include <QtCore/QByteArray>
#include <QtCore/QCryptographicHash>

#ifdef TELEFORGE_WITH_POSTGRES

#include "ayu/features/sync/teleforge_sync_merger.h"
#include "ayu/features/sync/teleforge_sync_snapshot.h"

#include "logs.h"

#include <crl/crl.h>

#include <QtCore/QDateTime>
#include <QtCore/QString>

#include <libpq-fe.h>

#endif // TELEFORGE_WITH_POSTGRES

namespace TeleForge::Sync {

QString PgTenantId(not_null<Main::Session*> session) {
	// Opaque, stable per-account identity. Any device logged into the same
	// account derives the same tenant id; other accounts derive a different
	// one, and the encrypted payload keeps them cryptographically isolated.
	const auto material = QByteArray::number(session->user()->id.value)
		+ ':'
		+ session->user()->phone().toUtf8();
	return QString::fromLatin1(
		QCryptographicHash::hash(material, QCryptographicHash::Sha256)
			.toHex());
}

#ifdef TELEFORGE_WITH_POSTGRES

namespace {

// Single latest-state row per tenant. The payload column holds the encrypted +
// compressed snapshot bundle produced by BuildEncryptedDatabaseShards().
constexpr auto kCreateTableSql =
	"CREATE TABLE IF NOT EXISTS teleforge_sync ("
	"tenant_id TEXT PRIMARY KEY,"
	"revision BIGINT NOT NULL,"
	"updated_at BIGINT NOT NULL,"
	"payload BYTEA NOT NULL,"
	"sha256 TEXT,"
	"date_from BIGINT,"
	"date_to BIGINT)";

constexpr auto kUpsertSql =
	"INSERT INTO teleforge_sync"
	" (tenant_id,revision,updated_at,payload,sha256,date_from,date_to)"
	" VALUES ($1,$2,$3,$4,$5,$6,$7)"
	" ON CONFLICT (tenant_id) DO UPDATE SET"
	" revision=EXCLUDED.revision,"
	" updated_at=EXCLUDED.updated_at,"
	" payload=EXCLUDED.payload,"
	" sha256=EXCLUDED.sha256,"
	" date_from=EXCLUDED.date_from,"
	" date_to=EXCLUDED.date_to"
	" WHERE teleforge_sync.revision <= EXCLUDED.revision";

constexpr auto kSelectSql =
	"SELECT payload FROM teleforge_sync WHERE tenant_id=$1";

[[nodiscard]] QString ConnStringFromSettings() {
	const auto core = LoadPersonalityCore();
	return core ? core->pgConnString : QString();
}

// Opens a libpq connection; returns nullptr and fills `error` on failure.
// Caller owns the returned connection and must PQfinish() it.
[[nodiscard]] PGconn *Connect(const QString &connString, QString &error) {
	if (connString.trimmed().isEmpty()) {
		error = u"Строка подключения PostgreSQL не задана."_q;
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

[[nodiscard]] bool EnsureTable(PGconn *conn, QString &error) {
	auto res = PQexec(conn, kCreateTableSql);
	const auto ok = res && (PQresultStatus(res) == PGRES_COMMAND_OK);
	if (!ok) {
		error = QString::fromUtf8(PQerrorMessage(conn)).trimmed();
	}
	if (res) {
		PQclear(res);
	}
	return ok;
}

struct UploadPayload {
	QString connString;
	QString tenantId;
	QByteArray encrypted;
	QString sha256Hex;
	qint64 revision = 0;
	int dateFrom = 0;
	int dateTo = 0;
};

// Blocking; runs on a worker thread.
[[nodiscard]] bool DoUpload(const UploadPayload &up, QString &error) {
	auto conn = Connect(up.connString, error);
	if (!conn) {
		return false;
	}
	auto guard = gsl::finally([&] { PQfinish(conn); });
	if (!EnsureTable(conn, error)) {
		return false;
	}

	const auto tenant = up.tenantId.toUtf8();
	const auto revision = QByteArray::number(up.revision);
	const auto updatedAt = QByteArray::number(
		QDateTime::currentSecsSinceEpoch());
	const auto sha = up.sha256Hex.toUtf8();
	const auto dateFrom = QByteArray::number(up.dateFrom);
	const auto dateTo = QByteArray::number(up.dateTo);

	const char *values[7] = {
		tenant.constData(),
		revision.constData(),
		updatedAt.constData(),
		up.encrypted.constData(),
		sha.constData(),
		dateFrom.constData(),
		dateTo.constData(),
	};
	const int lengths[7] = {
		0, 0, 0, int(up.encrypted.size()), 0, 0, 0,
	};
	// Only the payload ($4) is passed in binary format.
	const int formats[7] = { 0, 0, 0, 1, 0, 0, 0 };

	auto res = PQexecParams(
		conn,
		kUpsertSql,
		7,
		nullptr,
		values,
		lengths,
		formats,
		0);
	const auto ok = res && (PQresultStatus(res) == PGRES_COMMAND_OK);
	if (!ok) {
		error = QString::fromUtf8(PQerrorMessage(conn)).trimmed();
	}
	if (res) {
		PQclear(res);
	}
	return ok;
}

// Blocking; runs on a worker thread. Returns the encrypted payload (may be
// empty when the tenant has no stored snapshot yet).
[[nodiscard]] bool DoDownload(
		const QString &connString,
		const QString &tenantId,
		QByteArray &payloadOut,
		QString &error) {
	auto conn = Connect(connString, error);
	if (!conn) {
		return false;
	}
	auto guard = gsl::finally([&] { PQfinish(conn); });
	if (!EnsureTable(conn, error)) {
		return false;
	}

	const auto tenant = tenantId.toUtf8();
	const char *values[1] = { tenant.constData() };
	const int lengths[1] = { 0 };
	const int formats[1] = { 0 };
	// Request the result in binary so BYTEA comes back as raw bytes.
	auto res = PQexecParams(
		conn,
		kSelectSql,
		1,
		nullptr,
		values,
		lengths,
		formats,
		1);
	if (!res || PQresultStatus(res) != PGRES_TUPLES_OK) {
		error = QString::fromUtf8(PQerrorMessage(conn)).trimmed();
		if (res) {
			PQclear(res);
		}
		return false;
	}
	if (PQntuples(res) > 0) {
		const auto len = PQgetlength(res, 0, 0);
		payloadOut = QByteArray(PQgetvalue(res, 0, 0), len);
	}
	PQclear(res);
	return true;
}

} // namespace

void RunPgUpload(
		not_null<Main::Session*> session,
		const QByteArray &syncKey,
		Fn<void(bool ok, QString error)> done) {
	// Build the encrypted snapshot on the main thread (touches SQLite).
	const auto shards = BuildEncryptedDatabaseShards(syncKey);
	const EncryptedDbShard *dataShard = nullptr;
	for (const auto &shard : shards) {
		if (shard.kind == SyncDatabaseKind::Data) {
			dataShard = &shard;
			break;
		}
	}
	if (!dataShard || dataShard->encrypted.isEmpty()) {
		if (done) {
			done(false, u"Не удалось подготовить снапшот базы."_q);
		}
		return;
	}

	auto up = UploadPayload{
		.connString = ConnStringFromSettings(),
		.tenantId = PgTenantId(session),
		.encrypted = dataShard->encrypted,
		.sha256Hex = dataShard->sha256Hex,
		.revision = QDateTime::currentMSecsSinceEpoch(),
		.dateFrom = dataShard->dateFrom,
		.dateTo = dataShard->dateTo,
	};
	const auto sessionPtr = session.get();
	crl::async([up = std::move(up), done, sessionPtr] {
		auto error = QString();
		const auto ok = DoUpload(up, error);
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
		auto payload = QByteArray();
		auto error = QString();
		const auto ok = DoDownload(connString, tenantId, payload, error);
		crl::on_main(sessionPtr, [=] {
			if (!ok) {
				LOG(("TeleForge Sync PG: download failed: %1").arg(error));
				if (done) {
					done(false, error);
				}
				return;
			}
			if (payload.isEmpty()) {
				// No snapshot stored yet for this account — not an error.
				if (done) {
					done(true, QString());
				}
				return;
			}
			// Decrypt + decompress + merge on the main thread (touches SQLite).
			const auto applied = ApplyEncryptedDatabaseShard(
				SyncDatabaseKind::Data,
				payload,
				key);
			if (done) {
				done(applied, applied
					? QString()
					: u"Не удалось применить снапшот из PostgreSQL."_q);
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
		const auto ok = (conn != nullptr) && EnsureTable(conn, error);
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
		Fn<void(bool ok, QString error)> done) {
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

#endif // TELEFORGE_WITH_POSTGRES

} // namespace TeleForge::Sync
