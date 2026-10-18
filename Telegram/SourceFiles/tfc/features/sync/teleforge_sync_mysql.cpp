#include "tfc/features/sync/teleforge_sync_mysql.h"

#include "tfc/features/teleforge/teleforge_core.h"
#include "data/data_user.h"
#include "main/main_session.h"

#include <QtCore/QByteArray>
#include <QtCore/QCryptographicHash>

#ifdef TELEFORGE_WITH_MYSQL

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
#include <QtCore/QRegularExpression>
#include <QtCore/QString>
#include <QtCore/QByteArrayList>
#include <QtCore/QStringList>

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

// Relational schema mirroring the PostgreSQL backend: one row per captured
// message with an encrypted, text-sized payload; ids are blinded with HMAC and
// indexed; teleforge_events is an idempotent audit log.
constexpr auto kCreateMessagesSql =
	"CREATE TABLE IF NOT EXISTS teleforge_messages ("
	"tenant_id VARCHAR(128) NOT NULL,"
	"content_hash VARCHAR(128) NOT NULL,"
	"chat_id_h VARCHAR(64),"
	"account_id_h VARCHAR(64),"
	"message_id_h VARCHAR(64),"
	"deleted TINYINT(1),"
	"edited TINYINT(1),"
	"`date` BIGINT,"
	"edit_date BIGINT,"
	"payload LONGTEXT NOT NULL,"
	"updated_at BIGINT,"
	"PRIMARY KEY (tenant_id, content_hash),"
	"INDEX idx_tf_msg_chat (tenant_id, chat_id_h),"
	"INDEX idx_tf_msg_account (tenant_id, account_id_h),"
	"INDEX idx_tf_msg_msgid (tenant_id, message_id_h),"
	"INDEX idx_tf_msg_deleted (tenant_id, deleted),"
	"INDEX idx_tf_msg_date (tenant_id, `date`))";

constexpr auto kCreateEventsSql =
	"CREATE TABLE IF NOT EXISTS teleforge_events ("
	"tenant_id VARCHAR(128) NOT NULL,"
	"content_hash VARCHAR(128) NOT NULL,"
	"event_type VARCHAR(32) NOT NULL,"
	"chat_id_h VARCHAR(64),"
	"account_id_h VARCHAR(64),"
	"message_id_h VARCHAR(64),"
	"payload LONGTEXT,"
	"created_at BIGINT,"
	"PRIMARY KEY (tenant_id, content_hash, event_type),"
	"INDEX idx_tf_evt_type (tenant_id, event_type),"
	"INDEX idx_tf_evt_chat (tenant_id, chat_id_h),"
	"INDEX idx_tf_evt_msgid (tenant_id, message_id_h),"
	"INDEX idx_tf_evt_created (tenant_id, created_at))";

constexpr auto kCreateEmbeddingsSql =
	"CREATE TABLE IF NOT EXISTS teleforge_embeddings ("
	"tenant_id VARCHAR(128) NOT NULL,"
	"memory_id_h VARCHAR(64) NOT NULL,"
	"model_id VARCHAR(255),"
	"dims INT,"
	"embedding JSON,"
	"scope_type VARCHAR(64),"
	"chat_id_h VARCHAR(64) NULL,"
	"user_id_h VARCHAR(64) NULL,"
	"title_enc LONGTEXT,"
	"summary_enc LONGTEXT,"
	"updated_at BIGINT,"
	"PRIMARY KEY (tenant_id, memory_id_h),"
	"INDEX idx_tf_emb_scope (tenant_id, scope_type),"
	"INDEX idx_tf_emb_chat (tenant_id, chat_id_h))";

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

[[nodiscard]] bool EnsureSchema(MYSQL *conn, QString &error) {
	for (const auto *sql : {
			kCreateMessagesSql,
			kCreateEventsSql,
			kCreateEmbeddingsSql }) {
		if (mysql_query(conn, sql) != 0) {
			error = QString::fromUtf8(mysql_error(conn)).trimmed();
			return false;
		}
	}
	return true;
}

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

[[nodiscard]] bool Exec(MYSQL *conn, const QByteArray &query) {
	return mysql_real_query(
		conn,
		query.constData(),
		(unsigned long)query.size()) == 0;
}

// Rows per multi-row INSERT batch: one round-trip per batch, not per row.
constexpr auto kUploadBatch = 100;

// Blocking; runs on a worker thread. Upserts messages and appends audit events
// in multi-row batches (kUploadBatch rows per statement) inside one transaction.
[[nodiscard]] bool DoUploadMessages(
		MYSQL *conn,
		const QByteArray &tenant,
		const std::vector<SyncMessageRow> &rows,
		QString &error,
		const Fn<void(int)> &onRow) {
	if (rows.empty()) {
		return true;
	}
	mysql_query(conn, "START TRANSACTION");
	const auto tenantQ = MySqlQuote(conn, tenant);
	const auto now = QByteArray::number(
		(long long)QDateTime::currentSecsSinceEpoch());
	const auto fail = [&] {
		error = QString::fromUtf8(mysql_error(conn)).trimmed();
		mysql_query(conn, "ROLLBACK");
		return false;
	};
	auto seen = std::unordered_set<std::string>();
	for (auto start = std::size_t(0); start < rows.size(); start += kUploadBatch) {
		const auto stop = std::min(start + std::size_t(kUploadBatch), rows.size());
		auto msgValues = QByteArrayList();
		auto evtValues = QByteArrayList();
		for (auto i = start; i != stop; ++i) {
			const auto &row = rows[i];
			if (!seen.insert(row.contentHash.toStdString()).second) {
				continue;
			}
			const auto ch = MySqlQuote(conn, row.contentHash.toUtf8());
			const auto chat = MySqlQuote(conn, row.chatIdH.toUtf8());
			const auto acc = MySqlQuote(conn, row.accountIdH.toUtf8());
			const auto mid = MySqlQuote(conn, row.messageIdH.toUtf8());
			const auto payload = MySqlQuote(conn, row.payload.toBase64());
			msgValues.push_back("(" + tenantQ + "," + ch + "," + chat + ","
				+ acc + "," + mid + "," + (row.deleted ? "1" : "0") + ","
				+ (row.edited ? "1" : "0") + "," + QByteArray::number(row.date)
				+ "," + QByteArray::number(row.editDate) + "," + payload + ","
				+ now + ")");
			const auto addEvt = [&](const char *type) {
				evtValues.push_back("(" + tenantQ + "," + ch + ","
					+ MySqlQuote(conn, QByteArray(type)) + "," + chat + ","
					+ acc + "," + mid + "," + payload + "," + now + ")");
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
				"chat_id_h,account_id_h,message_id_h,deleted,edited,`date`,"
				"edit_date,payload,updated_at) VALUES ")
				+ msgValues.join(',')
				+ " ON DUPLICATE KEY UPDATE"
				" deleted=deleted|VALUES(deleted), edited=edited|VALUES(edited),"
				" edit_date=VALUES(edit_date), payload=VALUES(payload),"
				" updated_at=VALUES(updated_at)";
			if (!Exec(conn, sql)) {
				return fail();
			}
		}
		if (!evtValues.isEmpty()) {
			const auto sql = QByteArray(
				"INSERT IGNORE INTO teleforge_events (tenant_id,content_hash,"
				"event_type,chat_id_h,account_id_h,message_id_h,payload,"
				"created_at) VALUES ") + evtValues.join(',');
			if (!Exec(conn, sql)) {
				return fail();
			}
		}
		if (onRow) {
			onRow(int(stop));
		}
	}
	mysql_query(conn, "COMMIT");
	return true;
}

// Blocking; runs on a worker thread. Encrypted / id-blinded / projected mirror.
void UploadEmbeddings(
		MYSQL *conn,
		const QByteArray &tenant,
		const std::vector<EmbeddingRow> &embeddings,
		const QByteArray &key) {
	if (embeddings.empty()) {
		return;
	}
	mysql_query(conn, "START TRANSACTION");
	const auto tenantQ = MySqlQuote(conn, tenant);
	auto err = QString();
	for (const auto &row : embeddings) {
		const auto nullableH = [&](const std::optional<long long> &v) {
			return v
				? MySqlQuote(conn, HmacId(*v, key).toUtf8())
				: QByteArray("NULL");
		};
		const auto q = QByteArray(
			"INSERT INTO teleforge_embeddings (tenant_id,memory_id_h,model_id,"
			"dims,embedding,scope_type,chat_id_h,user_id_h,title_enc,"
			"summary_enc,updated_at) VALUES (")
			+ tenantQ + ","
			+ MySqlQuote(conn, HmacId(row.memoryId, key).toUtf8()) + ","
			+ MySqlQuote(conn, row.modelId.toUtf8()) + ","
			+ QByteArray::number(row.dims) + ","
			+ MySqlQuote(conn, VectorToJson(
				ProjectEmbedding(DeserializeEmbedding(row.vectorBlob), key))) + ","
			+ MySqlQuote(conn, row.scopeType.toUtf8()) + ","
			+ nullableH(row.chatId) + ","
			+ nullableH(row.userId) + ","
			+ MySqlQuote(conn, EncryptBundleForWrite(row.title.toUtf8(), key).toBase64()) + ","
			+ MySqlQuote(conn, EncryptBundleForWrite(row.summary.toUtf8(), key).toBase64()) + ","
			+ QByteArray::number(row.updatedAt)
			+ ") ON DUPLICATE KEY UPDATE"
			" model_id=VALUES(model_id), dims=VALUES(dims),"
			" embedding=VALUES(embedding), scope_type=VALUES(scope_type),"
			" chat_id_h=VALUES(chat_id_h), user_id_h=VALUES(user_id_h),"
			" title_enc=VALUES(title_enc), summary_enc=VALUES(summary_enc),"
			" updated_at=VALUES(updated_at)";
		if (!Exec(conn, q)) {
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
[[nodiscard]] bool DoDownloadMessages(
		MYSQL *conn,
		const QByteArray &tenant,
		std::vector<SyncMessageRow> &out,
		QString &error) {
	const auto query = QByteArray(
		"SELECT content_hash,deleted,edited,payload FROM teleforge_messages"
		" WHERE tenant_id=") + MySqlQuote(conn, tenant);
	if (!Exec(conn, query)) {
		error = QString::fromUtf8(mysql_error(conn)).trimmed();
		return false;
	}
	auto res = mysql_store_result(conn);
	if (!res) {
		error = QString::fromUtf8(mysql_error(conn)).trimmed();
		return false;
	}
	auto guard = gsl::finally([&] { mysql_free_result(res); });
	while (auto r = mysql_fetch_row(res)) {
		const auto lengths = mysql_fetch_lengths(res);
		if (!lengths) {
			continue;
		}
		auto row = SyncMessageRow{};
		row.contentHash = QString::fromUtf8(QByteArray(r[0], int(lengths[0])));
		row.deleted = (r[1] && r[1][0] == '1');
		row.edited = (r[2] && r[2][0] == '1');
		row.payload = QByteArray::fromBase64(QByteArray(r[3], int(lengths[3])));
		out.push_back(std::move(row));
	}
	return true;
}

[[nodiscard]] QString ConnStringFromSettings() {
	const auto core = LoadPersonalityCore();
	return core ? core->pgConnString : QString();
}

} // namespace

void RunMySqlUpload(
		not_null<Main::Session*> session,
		const QByteArray &syncKey,
		Fn<void(bool ok, QString error)> done,
		Fn<void(int done, int total)> onProgress) {
	// Build encrypted, id-blinded rows on the main thread (touches SQLite).
	const auto rows = CollectMessageRows(syncKey);
	const auto embeddings = CollectEmbeddingRows();
	const auto connString = ConnStringFromSettings();
	const auto tenant = MySqlTenantId(session).toUtf8();
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
				&& DoUploadMessages(conn, tenant, rows, error, onRow);
			if (ok) {
				UploadEmbeddings(conn, tenant, embeddings, syncKey);
			}
			mysql_close(conn);
		}
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
	const auto connString = ConnStringFromSettings();
	const auto tenant = MySqlTenantId(session).toUtf8();
	const auto sessionPtr = session.get();
	const auto key = syncKey;
	crl::async([=] {
		auto error = QString();
		auto rows = std::vector<SyncMessageRow>();
		auto conn = Connect(connString, error);
		auto ok = false;
		if (conn) {
			ok = EnsureSchema(conn, error)
				&& DoDownloadMessages(conn, tenant, rows, error);
			mysql_close(conn);
		}
		crl::on_main(sessionPtr, [=] {
			if (!ok) {
				LOG(("TeleForge Sync MySQL: download failed: %1").arg(error));
				if (done) {
					done(false, error);
				}
				return;
			}
			// Merge into the local DB (main thread). Remote is only a replica.
			ApplyMessageRows(rows, key);
			if (done) {
				done(true, QString());
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
		const auto ok = (conn != nullptr) && EnsureSchema(conn, error);
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
		Fn<void(bool ok, QString error)> done,
		Fn<void(int done, int total)> onProgress) {
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
