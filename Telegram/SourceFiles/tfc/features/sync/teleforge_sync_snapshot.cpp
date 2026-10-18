#include "tfc/features/sync/teleforge_sync_snapshot.h"

#include "tfc/features/sync/teleforge_sync_crypto.h"
#include "tfc/features/teleforge/teleforge_storage.h"
#include "settings.h"

#include "logs.h"

#include "ayu/libs/sqlite/sqlite3.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QTemporaryFile>

#include <zlib.h>

#include <vector>

namespace TeleForge::Sync {
namespace {

constexpr auto kChunkSize = std::size_t(256 * 1024);
constexpr auto kCompressionLevel = 9;

[[nodiscard]] QString SourcePath(SyncDatabaseKind kind) {
	switch (kind) {
	case SyncDatabaseKind::Data:
		return TeleForge::Storage::databasePath();
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
	sqlite3_exec(
		db,
		TeleForge::Storage::kSqliteTuning,
		nullptr,
		nullptr,
		nullptr);
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
	Q_UNUSED(kind);
	return QDir::cleanPath(
		QString(cWorkingDir())
		+ u"/tdata/teleforge_sync_snapshot_data.tforge"_q);
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
	const auto unfolded = quint32(file.size() & 0xFFFFFFFF);
	auto compressed = QByteArray();
	compressed.reserve(4 + int(file.size() / 4));
	compressed.append(char((unfolded >> 24) & 0xFF));
	compressed.append(char((unfolded >> 16) & 0xFF));
	compressed.append(char((unfolded >> 8) & 0xFF));
	compressed.append(char(unfolded & 0xFF));

	auto stream = z_stream{};
	if (deflateInit(&stream, kCompressionLevel) != Z_OK) {
		return {};
	}
	const auto guard = gsl::finally([&] { deflateEnd(&stream); });
	auto input = std::vector<char>(kChunkSize);
	auto output = std::vector<char>(kChunkSize);
	while (true) {
		const auto read = file.read(input.data(), qint64(input.size()));
		if (read < 0) {
			return {};
		}
		const auto last = (read == 0);
		stream.avail_in = uInt(read);
		stream.next_in = reinterpret_cast<Bytef*>(input.data());
		while (true) {
			stream.avail_out = uInt(output.size());
			stream.next_out = reinterpret_cast<Bytef*>(output.data());
			const auto result = deflate(&stream, last ? Z_FINISH : Z_NO_FLUSH);
			if (result != Z_OK && result != Z_STREAM_END) {
				return {};
			}
			compressed.append(
				output.data(),
				int(output.size() - stream.avail_out));
			if (result == Z_STREAM_END) {
				return compressed;
			} else if (stream.avail_out != 0) {
				if (!last) {
					break;
				}
				return {};
			}
		}
		if (last) {
			return {};
		}
	}
}

std::optional<QString> DecompressToTemp(const QByteArray &compressed) {
	if (compressed.size() <= 4) {
		return std::nullopt;
	}
	const auto expected = (qint64(quint32(uchar(compressed[0]))) << 24)
		| (qint64(quint32(uchar(compressed[1]))) << 16)
		| (qint64(quint32(uchar(compressed[2]))) << 8)
		| qint64(quint32(uchar(compressed[3])));
	if (expected <= 0) {
		return std::nullopt;
	}
	auto temp = QTemporaryFile(
		QDir::tempPath() + u"/teleforge_sync_XXXXXX.db"_q);
	if (!temp.open()) {
		return std::nullopt;
	}
	auto stream = z_stream{};
	if (inflateInit(&stream) != Z_OK) {
		return std::nullopt;
	}
	const auto guard = gsl::finally([&] { inflateEnd(&stream); });
	stream.next_in = reinterpret_cast<Bytef*>(
		const_cast<char*>(compressed.constData()) + 4);
	stream.avail_in = uInt(compressed.size() - 4);

	auto output = std::vector<char>(kChunkSize);
	auto written = qint64(0);
	while (true) {
		stream.avail_out = uInt(output.size());
		stream.next_out = reinterpret_cast<Bytef*>(output.data());
		const auto result = inflate(&stream, Z_NO_FLUSH);
		const auto produced = qint64(output.size() - stream.avail_out);
		if (produced && temp.write(output.data(), produced) != produced) {
			return std::nullopt;
		}
		written += produced;
		if (result == Z_STREAM_END) {
			break;
		} else if (result == Z_BUF_ERROR) {
			if (!stream.avail_in && !produced) {
				break;
			}
			return std::nullopt;
		} else if (result != Z_OK) {
			return std::nullopt;
		} else if (!stream.avail_in && stream.avail_out) {
			return std::nullopt;
		}
	}
	if (written != expected) {
		return std::nullopt;
	}
	temp.close();
	temp.setAutoRemove(false);
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
