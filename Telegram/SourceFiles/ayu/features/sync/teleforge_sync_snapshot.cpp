#include "ayu/features/sync/teleforge_sync_snapshot.h"

#include "ayu/features/sync/teleforge_sync_crypto.h"
#include "ayu/features/teleforge/teleforge_storage.h"
#include "settings.h"

#include "logs.h"

#include "ayu/libs/sqlite/sqlite3.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QTemporaryFile>

namespace TeleForge::Sync {
namespace {

[[nodiscard]] QString SourcePath(SyncDatabaseKind kind) {
	switch (kind) {
	case SyncDatabaseKind::TeleForge:
		return TeleForge::Storage::databasePath();
	case SyncDatabaseKind::AyuData:
		return QDir::cleanPath(QString(cWorkingDir()) + u"/tdata/ayudata.db"_q);
	}
	return {};
}

[[nodiscard]] bool ExecOnPath(const QString &dbPath, const char *sql) {
	sqlite3 *db = nullptr;
	if (sqlite3_open_v2(
			dbPath.toUtf8().constData(),
			&db,
			SQLITE_OPEN_READWRITE,
			nullptr) != SQLITE_OK
		|| !db) {
		return false;
	}
	char *err = nullptr;
	const auto rc = sqlite3_exec(db, sql, nullptr, nullptr, &err);
	if (rc != SQLITE_OK) {
		const auto message = err ? QString::fromUtf8(err) : QString();
		if (err) {
			sqlite3_free(err);
		}
		LOG(("TeleForge Sync SQL failed on %1: %2").arg(dbPath, message));
	}
	sqlite3_close(db);
	return rc == SQLITE_OK;
}

} // namespace

QString SnapshotPath(SyncDatabaseKind kind) {
	const auto suffix = (kind == SyncDatabaseKind::TeleForge)
		? u"teleforge"_q
		: u"ayudata"_q;
	return QDir::cleanPath(
		QString(cWorkingDir())
		+ u"/tdata/teleforge_sync_snapshot_"_q
		+ suffix
		+ u".db"_q);
}

bool CreateDatabaseSnapshot(SyncDatabaseKind kind, const QString &destPath) {
	const auto source = SourcePath(kind);
	if (source.isEmpty() || !QFile::exists(source)) {
		return false;
	}
	QFile::remove(destPath);
	const auto sql = u"VACUUM INTO '%1';"_q.arg(
		QString(destPath).replace('\'', "''"));
	return ExecOnPath(source, sql.toUtf8().constData()) && QFile::exists(destPath);
}

QByteArray CompressSnapshot(const QString &snapshotPath) {
	QFile file(snapshotPath);
	if (!file.open(QIODevice::ReadOnly)) {
		return {};
	}
	return qCompress(file.readAll(), 9);
}

std::optional<QString> DecompressToTemp(const QByteArray &compressed) {
	const auto data = qUncompress(compressed);
	if (data.isEmpty()) {
		return std::nullopt;
	}
	auto temp = QTemporaryFile(
		QDir::tempPath() + u"/teleforge_sync_XXXXXX.db"_q);
	temp.setAutoRemove(false);
	if (!temp.open()) {
		return std::nullopt;
	}
	temp.write(data);
	temp.close();
	return temp.fileName();
}

QByteArray EncryptSnapshot(const QByteArray &compressed, const QByteArray &key) {
	return EncryptBundle(compressed, key);
}

std::optional<QByteArray> DecryptSnapshot(
		const QByteArray &encrypted,
		const QByteArray &key) {
	return DecryptBundle(encrypted, key);
}

} // namespace TeleForge::Sync
