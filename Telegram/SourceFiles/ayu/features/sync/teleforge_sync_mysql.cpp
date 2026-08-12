#include "ayu/features/sync/teleforge_sync_mysql.h"

#include "ayu/features/teleforge/teleforge_core.h"
#include "data/data_user.h"
#include "main/main_session.h"

#include <QtCore/QByteArray>
#include <QtCore/QCryptographicHash>

#ifdef TELEFORGE_WITH_MYSQL

#include "ayu/features/sync/teleforge_ssh_tunnel.h"
#include "ayu/features/sync/teleforge_sync_embeddings.h"
#include "ayu/features/sync/teleforge_sync_merger.h"
#include "ayu/features/sync/teleforge_sync_snapshot.h"

#include "logs.h"

#include <crl/crl.h>

#include <QtCore/QDateTime>
#include <QtCore/QRegularExpression>
#include <QtCore/QString>
#include <QtCore/QStringList>

#include <cstring>

#include <mysql.h>

#endif // TELEFORGE_WITH_MYSQL

namespace TeleForge::Sync {

namespace {

[[nodiscard]] QString MySqlTenantId(not_null<Main::Session*> session) {
	// Same opaque per-account identity scheme as the PostgreSQL backend.
	const auto material = QByteArray::number(session->user()->id.value)
		+ ':'
		+ session->user()->phone().toUtf8();
	return QString::fromLatin1(
		QCryptographicHash::hash(material, QCryptographicHash::Sha256)
			.toHex());
}

} // namespace

#ifdef TELEFORGE_WITH_MYSQL

namespace {

constexpr auto kCreateTableSql =
	"CREATE TABLE IF NOT EXISTS teleforge_sync ("
	"tenant_id VARCHAR(128) PRIMARY KEY,"
	"revision BIGINT NOT NULL,"
	"updated_at BIGINT NOT NULL,"
	"payload LONGBLOB NOT NULL,"
	"sha256 VARCHAR(128),"
	"date_from BIGINT,"
	"date_to BIGINT)";

// Last-writer-wins by revision. VALUES(col) refers to the would-be-inserted
// row; the plain column name refers to the existing row until reassigned, and
// revision is assigned last so the earlier IFs still see the old revision.
constexpr auto kUpsertSql =
	"INSERT INTO teleforge_sync"
	" (tenant_id,revision,updated_at,payload,sha256,date_from,date_to)"
	" VALUES (?,?,?,?,?,?,?)"
	" ON DUPLICATE KEY UPDATE"
	" payload=IF(VALUES(revision)>=revision,VALUES(payload),payload),"
	" updated_at=IF(VALUES(revision)>=revision,VALUES(updated_at),updated_at),"
	" sha256=IF(VALUES(revision)>=revision,VALUES(sha256),sha256),"
	" date_from=IF(VALUES(revision)>=revision,VALUES(date_from),date_from),"
	" date_to=IF(VALUES(revision)>=revision,VALUES(date_to),date_to),"
	" revision=IF(VALUES(revision)>=revision,VALUES(revision),revision)";

// Plaintext, queryable embeddings table (kept alongside the encrypted blob).
// The vector is stored as a JSON array for inspection with MySQL JSON funcs.
constexpr auto kCreateEmbeddingsTableSql =
	"CREATE TABLE IF NOT EXISTS teleforge_embeddings ("
	"tenant_id VARCHAR(128) NOT NULL,"
	"memory_id BIGINT NOT NULL,"
	"model_id VARCHAR(255),"
	"dims INT,"
	"embedding JSON,"
	"scope_type VARCHAR(64),"
	"chat_id BIGINT NULL,"
	"user_id BIGINT NULL,"
	"title TEXT,"
	"summary TEXT,"
	"updated_at BIGINT,"
	"PRIMARY KEY (tenant_id, memory_id))";

struct ConnParams {
	QByteArray host = QByteArray("127.0.0.1");
	QByteArray user;
	QByteArray password;
	QByteArray db;
	unsigned int port = 3306;
	bool valid = false;
};

// Parses space/';'-separated key=value tokens.
[[nodiscard]] ConnParams ParseConn(const QString &connString) {
	auto result = ConnParams();
	const auto tokens = connString.split(
		QRegularExpression(u"[\\s;]+"_q),
		Qt::SkipEmptyParts);
	for (const auto &token : tokens) {
		const auto eq = token.indexOf('=');
		if (eq <= 0) {
			continue;
		}
		const auto key = token.left(eq).trimmed().toLower();
		const auto value = token.mid(eq + 1).trimmed();
		if (key == u"host"_q || key == u"server"_q) {
			result.host = value.toUtf8();
		} else if (key == u"port"_q) {
			result.port = value.toUInt();
		} else if (key == u"user"_q || key == u"uid"_q) {
			result.user = value.toUtf8();
		} else if (key == u"password"_q || key == u"pwd"_q) {
			result.password = value.toUtf8();
		} else if (key == u"db"_q
			|| key == u"dbname"_q
			|| key == u"database"_q) {
			result.db = value.toUtf8();
		}
	}
	result.valid = !result.db.isEmpty() && !result.user.isEmpty();
	return result;
}

// Opens a connection; returns nullptr and fills `error` on failure. Caller
// owns the returned handle and must mysql_close() it. Runs off the main
// thread (see teleforge_ssh_tunnel.h), so it may block briefly while an
// `ssh -L` tunnel to the DB host is established.
[[nodiscard]] MYSQL *Connect(const QString &connStringIn, QString &error) {
	const auto connString = Ssh::ApplyTunnelIfConfigured(
		connStringIn,
		3306,
		error);
	if (connString.isEmpty() && !error.isEmpty()) {
		return nullptr;
	}
	const auto params = ParseConn(connString);
	if (!params.valid) {
		error = u"Строка подключения MySQL неполная (нужны user и dbname)."_q;
		return nullptr;
	}
	auto conn = mysql_init(nullptr);
	if (!conn) {
		error = u"Не удалось инициализировать MySQL."_q;
		return nullptr;
	}
	if (!mysql_real_connect(
			conn,
			params.host.constData(),
			params.user.constData(),
			params.password.isEmpty() ? nullptr : params.password.constData(),
			params.db.constData(),
			params.port,
			nullptr,
			0)) {
		error = QString::fromUtf8(mysql_error(conn)).trimmed();
		mysql_close(conn);
		return nullptr;
	}
	return conn;
}

[[nodiscard]] bool EnsureTable(MYSQL *conn, QString &error) {
	if (mysql_query(conn, kCreateTableSql) != 0) {
		error = QString::fromUtf8(mysql_error(conn)).trimmed();
		return false;
	}
	return true;
}

struct UploadPayload {
	QString connString;
	QByteArray tenantId;
	QByteArray encrypted;
	QByteArray sha256Hex;
	long long revision = 0;
	long long dateFrom = 0;
	long long dateTo = 0;
	std::vector<EmbeddingRow> embeddings;
};

// JSON array literal, e.g. [0.12,-0.34,...]
[[nodiscard]] QByteArray VectorToJson(const std::vector<float> &v) {
	auto parts = QStringList();
	parts.reserve(int(v.size()));
	for (const auto f : v) {
		parts.push_back(QString::number(f, 'g', 9));
	}
	return (u"["_q + parts.join(',') + u"]"_q).toUtf8();
}

[[nodiscard]] QByteArray MySqlQuote(MYSQL *conn, const QByteArray &in) {
	auto out = QByteArray(in.size() * 2 + 1, Qt::Uninitialized);
	const auto len = mysql_real_escape_string(
		conn,
		out.data(),
		in.constData(),
		(unsigned long)in.size());
	out.resize(int(len));
	return "'" + out + "'";
}

// Best-effort plaintext embeddings mirror; logs but never fails the snapshot.
void UploadEmbeddings(MYSQL *conn, const UploadPayload &up) {
	if (up.embeddings.empty()) {
		return;
	}
	if (mysql_query(conn, kCreateEmbeddingsTableSql) != 0) {
		LOG(("TeleForge Sync MySQL: embeddings table create failed: %1")
			.arg(QString::fromUtf8(mysql_error(conn)).trimmed()));
		return;
	}
	mysql_query(conn, "START TRANSACTION");
	const auto tenant = MySqlQuote(conn, up.tenantId);
	auto err = QString();
	for (const auto &row : up.embeddings) {
		const auto nullable = [](const std::optional<long long> &v) {
			return v ? QByteArray::number(*v) : QByteArray("NULL");
		};
		const auto query = QByteArray(
			"INSERT INTO teleforge_embeddings (tenant_id,memory_id,model_id,"
			"dims,embedding,scope_type,chat_id,user_id,title,summary,"
			"updated_at) VALUES (")
			+ tenant + ","
			+ QByteArray::number(row.memoryId) + ","
			+ MySqlQuote(conn, row.modelId.toUtf8()) + ","
			+ QByteArray::number(row.dims) + ","
			+ MySqlQuote(conn, VectorToJson(row.vector)) + ","
			+ MySqlQuote(conn, row.scopeType.toUtf8()) + ","
			+ nullable(row.chatId) + ","
			+ nullable(row.userId) + ","
			+ MySqlQuote(conn, row.title.toUtf8()) + ","
			+ MySqlQuote(conn, row.summary.toUtf8()) + ","
			+ QByteArray::number(row.updatedAt)
			+ ") ON DUPLICATE KEY UPDATE"
			" model_id=VALUES(model_id), dims=VALUES(dims),"
			" embedding=VALUES(embedding), scope_type=VALUES(scope_type),"
			" chat_id=VALUES(chat_id), user_id=VALUES(user_id),"
			" title=VALUES(title), summary=VALUES(summary),"
			" updated_at=VALUES(updated_at)";
		if (mysql_real_query(conn, query.constData(), query.size()) != 0) {
			err = QString::fromUtf8(mysql_error(conn)).trimmed();
			break;
		}
	}
	if (err.isEmpty()) {
		mysql_query(conn, "COMMIT");
	} else {
		mysql_query(conn, "ROLLBACK");
		LOG(("TeleForge Sync MySQL: embeddings upload failed: %1").arg(err));
	}
}

// Blocking; runs on a worker thread.
[[nodiscard]] bool DoUpload(const UploadPayload &up, QString &error) {
	auto conn = Connect(up.connString, error);
	if (!conn) {
		return false;
	}
	auto connGuard = gsl::finally([&] { mysql_close(conn); });
	if (!EnsureTable(conn, error)) {
		return false;
	}

	auto stmt = mysql_stmt_init(conn);
	if (!stmt) {
		error = QString::fromUtf8(mysql_error(conn)).trimmed();
		return false;
	}
	auto stmtGuard = gsl::finally([&] { mysql_stmt_close(stmt); });
	if (mysql_stmt_prepare(stmt, kUpsertSql, (unsigned long)strlen(kUpsertSql))
			!= 0) {
		error = QString::fromUtf8(mysql_stmt_error(stmt)).trimmed();
		return false;
	}

	auto revision = up.revision;
	auto updatedAt = (long long)QDateTime::currentSecsSinceEpoch();
	auto dateFrom = up.dateFrom;
	auto dateTo = up.dateTo;
	auto payloadLen = (unsigned long)up.encrypted.size();
	auto tenantLen = (unsigned long)up.tenantId.size();
	auto shaLen = (unsigned long)up.sha256Hex.size();

	MYSQL_BIND bind[7];
	memset(bind, 0, sizeof(bind));

	bind[0].buffer_type = MYSQL_TYPE_STRING;
	bind[0].buffer = const_cast<char*>(up.tenantId.constData());
	bind[0].buffer_length = tenantLen;
	bind[0].length = &tenantLen;

	bind[1].buffer_type = MYSQL_TYPE_LONGLONG;
	bind[1].buffer = &revision;

	bind[2].buffer_type = MYSQL_TYPE_LONGLONG;
	bind[2].buffer = &updatedAt;

	bind[3].buffer_type = MYSQL_TYPE_LONG_BLOB;
	bind[3].buffer = const_cast<char*>(up.encrypted.constData());
	bind[3].buffer_length = payloadLen;
	bind[3].length = &payloadLen;

	bind[4].buffer_type = MYSQL_TYPE_STRING;
	bind[4].buffer = const_cast<char*>(up.sha256Hex.constData());
	bind[4].buffer_length = shaLen;
	bind[4].length = &shaLen;

	bind[5].buffer_type = MYSQL_TYPE_LONGLONG;
	bind[5].buffer = &dateFrom;

	bind[6].buffer_type = MYSQL_TYPE_LONGLONG;
	bind[6].buffer = &dateTo;

	if (mysql_stmt_bind_param(stmt, bind) != 0
		|| mysql_stmt_execute(stmt) != 0) {
		error = QString::fromUtf8(mysql_stmt_error(stmt)).trimmed();
		return false;
	}
	// Best-effort plaintext embeddings mirror; never fails the snapshot.
	UploadEmbeddings(conn, up);
	return true;
}

// Blocking; runs on a worker thread. Returns the encrypted payload (may be
// empty when the tenant has no stored snapshot yet).
[[nodiscard]] bool DoDownload(
		const QString &connString,
		const QByteArray &tenantId,
		QByteArray &payloadOut,
		QString &error) {
	auto conn = Connect(connString, error);
	if (!conn) {
		return false;
	}
	auto connGuard = gsl::finally([&] { mysql_close(conn); });
	if (!EnsureTable(conn, error)) {
		return false;
	}

	// tenant_id is a SHA-256 hex string, so escaping is belt-and-braces.
	auto escaped = QByteArray(tenantId.size() * 2 + 1, Qt::Uninitialized);
	const auto escLen = mysql_real_escape_string(
		conn,
		escaped.data(),
		tenantId.constData(),
		(unsigned long)tenantId.size());
	escaped.resize(int(escLen));
	const auto query = QByteArray("SELECT payload FROM teleforge_sync"
		" WHERE tenant_id='") + escaped + "'";

	if (mysql_real_query(conn, query.constData(), (unsigned long)query.size())
			!= 0) {
		error = QString::fromUtf8(mysql_error(conn)).trimmed();
		return false;
	}
	auto res = mysql_store_result(conn);
	if (!res) {
		error = QString::fromUtf8(mysql_error(conn)).trimmed();
		return false;
	}
	auto resGuard = gsl::finally([&] { mysql_free_result(res); });
	if (auto row = mysql_fetch_row(res)) {
		const auto lengths = mysql_fetch_lengths(res);
		if (row[0] && lengths) {
			payloadOut = QByteArray(row[0], int(lengths[0]));
		}
	}
	return true;
}

} // namespace

void RunMySqlUpload(
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

	const auto core = LoadPersonalityCore();
	auto up = UploadPayload{
		.connString = core ? core->pgConnString : QString(),
		.tenantId = MySqlTenantId(session).toUtf8(),
		.encrypted = dataShard->encrypted,
		.sha256Hex = dataShard->sha256Hex.toUtf8(),
		.revision = QDateTime::currentMSecsSinceEpoch(),
		.dateFrom = dataShard->dateFrom,
		.dateTo = dataShard->dateTo,
		.embeddings = CollectEmbeddingRows(),
	};
	const auto sessionPtr = session.get();
	crl::async([up = std::move(up), done, sessionPtr] {
		auto error = QString();
		const auto ok = DoUpload(up, error);
		crl::on_main(sessionPtr, [=] {
			if (!ok) {
				LOG(("TeleForge Sync MySQL: upload failed: %1").arg(error));
			}
			if (done) {
				done(ok, error);
			}
		});
	});
}

void RunMySqlDownload(
		not_null<Main::Session*> session,
		const QByteArray &syncKey,
		Fn<void(bool ok, QString error)> done) {
	const auto core = LoadPersonalityCore();
	const auto connString = core ? core->pgConnString : QString();
	const auto tenantId = MySqlTenantId(session).toUtf8();
	const auto sessionPtr = session.get();
	const auto key = syncKey;
	crl::async([=] {
		auto payload = QByteArray();
		auto error = QString();
		const auto ok = DoDownload(connString, tenantId, payload, error);
		crl::on_main(sessionPtr, [=] {
			if (!ok) {
				LOG(("TeleForge Sync MySQL: download failed: %1").arg(error));
				if (done) {
					done(false, error);
				}
				return;
			}
			if (payload.isEmpty()) {
				if (done) {
					done(true, QString());
				}
				return;
			}
			const auto applied = ApplyEncryptedDatabaseShard(
				SyncDatabaseKind::Data,
				payload,
				key);
			if (done) {
				done(applied, applied
					? QString()
					: u"Не удалось применить снапшот из MySQL."_q);
			}
		});
	});
}

void MySqlTestConnection(
		const QString &connString,
		Fn<void(bool ok, QString error)> done) {
	crl::async([connString, done] {
		auto error = QString();
		auto conn = Connect(connString, error);
		const auto ok = (conn != nullptr) && EnsureTable(conn, error);
		if (conn) {
			mysql_close(conn);
		}
		crl::on_main([=] {
			if (done) {
				done(ok, error);
			}
		});
	});
}

#else // TELEFORGE_WITH_MYSQL

namespace {

[[nodiscard]] QString NotCompiledError() {
	return u"Поддержка MySQL не включена в этой сборке."_q;
}

} // namespace

void RunMySqlUpload(
		not_null<Main::Session*> session,
		const QByteArray &syncKey,
		Fn<void(bool ok, QString error)> done) {
	if (done) {
		done(false, NotCompiledError());
	}
}

void RunMySqlDownload(
		not_null<Main::Session*> session,
		const QByteArray &syncKey,
		Fn<void(bool ok, QString error)> done) {
	if (done) {
		done(false, NotCompiledError());
	}
}

void MySqlTestConnection(
		const QString &connString,
		Fn<void(bool ok, QString error)> done) {
	if (done) {
		done(false, NotCompiledError());
	}
}

#endif // TELEFORGE_WITH_MYSQL

} // namespace TeleForge::Sync
