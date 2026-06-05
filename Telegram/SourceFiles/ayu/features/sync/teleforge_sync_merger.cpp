#include "ayu/features/sync/teleforge_sync_merger.h"

#include "ayu/features/sync/teleforge_sync_snapshot.h"
#include "ayu/features/teleforge/teleforge_storage.h"
#include "settings.h"

#include "logs.h"

#include "ayu/libs/sqlite/sqlite3.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>

namespace TeleForge::Sync {
namespace {

[[nodiscard]] QString LivePath(SyncDatabaseKind kind) {
	switch (kind) {
	case SyncDatabaseKind::Data:
		return TeleForge::Storage::databasePath();
	}
	return {};
}

[[nodiscard]] bool RemoteTableExists(sqlite3 *db, const char *table) {
	sqlite3_stmt *stmt = nullptr;
	const auto sql = QString(
		"SELECT 1 FROM remote.sqlite_master "
		"WHERE type='table' AND name='%1' LIMIT 1;").arg(table);
	if (sqlite3_prepare_v2(
			db,
			sql.toUtf8().constData(),
			-1,
			&stmt,
			nullptr) != SQLITE_OK) {
		return false;
	}
	const auto exists = (sqlite3_step(stmt) == SQLITE_ROW);
	sqlite3_finalize(stmt);
	return exists;
}

[[nodiscard]] bool Exec(sqlite3 *db, const char *sql) {
	char *err = nullptr;
	const auto rc = sqlite3_exec(db, sql, nullptr, nullptr, &err);
	if (rc != SQLITE_OK) {
		const auto message = err ? QString::fromUtf8(err) : QString();
		if (err) {
			sqlite3_free(err);
		}
		LOG(("TeleForge Sync merge SQL failed: %1").arg(message));
		return false;
	}
	return true;
}

[[nodiscard]] bool MergeDataDb(sqlite3 *live, const QString &remotePath) {
	const auto attach = u"ATTACH DATABASE '%1' AS remote;"_q.arg(
		QString(remotePath).replace('\'', "''"));
	if (!Exec(live, attach.toUtf8().constData())) {
		return false;
	}
	const auto coreQueries = std::array<const char*, 11>{
		R"SQL(
INSERT OR REPLACE INTO PersonalityCore
SELECT r.* FROM remote.PersonalityCore r
LEFT JOIN PersonalityCore l ON l.singletonId = r.singletonId
WHERE l.singletonId IS NULL OR r.updatedAt >= l.updatedAt;
)SQL",
		R"SQL(
INSERT OR REPLACE INTO PerChatSettings
SELECT r.* FROM remote.PerChatSettings r
LEFT JOIN PerChatSettings l ON l.peerId = r.peerId
WHERE l.peerId IS NULL OR r.updatedAt >= l.updatedAt;
)SQL",
		R"SQL(
INSERT OR REPLACE INTO MemoryItem
SELECT r.* FROM remote.MemoryItem r
LEFT JOIN MemoryItem l ON l.id = r.id
WHERE l.id IS NULL OR r.updatedAt >= l.updatedAt;
)SQL",
		R"SQL(
INSERT OR REPLACE INTO MemoryEmbedding
SELECT r.* FROM remote.MemoryEmbedding r
LEFT JOIN MemoryEmbedding l ON l.memoryId = r.memoryId
WHERE l.memoryId IS NULL OR r.updatedAt >= l.updatedAt;
)SQL",
		R"SQL(
INSERT OR IGNORE INTO MemorySyncEvent
SELECT r.* FROM remote.MemorySyncEvent r
WHERE NOT EXISTS (
	SELECT 1 FROM MemorySyncEvent l
	WHERE l.eventType = r.eventType
		AND l.memoryId = r.memoryId
		AND l.createdAt = r.createdAt
		AND l.deviceId = r.deviceId);
)SQL",
		R"SQL(
INSERT OR REPLACE INTO SyncArtifact
SELECT r.* FROM remote.SyncArtifact r
LEFT JOIN SyncArtifact l ON l.id = r.id
WHERE l.id IS NULL OR r.updatedAt >= l.updatedAt;
)SQL",
		R"SQL(
INSERT INTO DeletedMessage (
	userId, dialogId, groupedId, peerId, fromId, topicId, messageId, date, flags,
	editDate, views, fwdFlags, fwdFromId, fwdName, fwdDate, fwdPostAuthor,
	replyFlags, replyMessageId, replyPeerId, replyTopId, replyForumTopic,
	replySerialized, entityCreateDate, text, textEntities, mediaPath, hqThumbPath,
	documentType, documentSerialized, thumbsSerialized, documentAttributesSerialized,
	mimeType, contentHash)
SELECT
	r.userId, r.dialogId, r.groupedId, r.peerId, r.fromId, r.topicId, r.messageId, r.date, r.flags,
	r.editDate, r.views, r.fwdFlags, r.fwdFromId, r.fwdName, r.fwdDate, r.fwdPostAuthor,
	r.replyFlags, r.replyMessageId, r.replyPeerId, r.replyTopId, r.replyForumTopic,
	r.replySerialized, r.entityCreateDate, r.text, r.textEntities, r.mediaPath, r.hqThumbPath,
	r.documentType, r.documentSerialized, r.thumbsSerialized, r.documentAttributesSerialized,
	r.mimeType, r.contentHash
FROM remote.DeletedMessage r
WHERE r.contentHash != ''
	AND NOT EXISTS (
		SELECT 1 FROM DeletedMessage l WHERE l.contentHash = r.contentHash);
)SQL",
		R"SQL(
INSERT INTO EditedMessage (
	userId, dialogId, groupedId, peerId, fromId, topicId, messageId, date, flags,
	editDate, views, fwdFlags, fwdFromId, fwdName, fwdDate, fwdPostAuthor,
	replyFlags, replyMessageId, replyPeerId, replyTopId, replyForumTopic,
	replySerialized, entityCreateDate, text, textEntities, mediaPath, hqThumbPath,
	documentType, documentSerialized, thumbsSerialized, documentAttributesSerialized,
	mimeType, contentHash)
SELECT
	r.userId, r.dialogId, r.groupedId, r.peerId, r.fromId, r.topicId, r.messageId, r.date, r.flags,
	r.editDate, r.views, r.fwdFlags, r.fwdFromId, r.fwdName, r.fwdDate, r.fwdPostAuthor,
	r.replyFlags, r.replyMessageId, r.replyPeerId, r.replyTopId, r.replyForumTopic,
	r.replySerialized, r.entityCreateDate, r.text, r.textEntities, r.mediaPath, r.hqThumbPath,
	r.documentType, r.documentSerialized, r.thumbsSerialized, r.documentAttributesSerialized,
	r.mimeType, r.contentHash
FROM remote.EditedMessage r
WHERE r.contentHash != ''
	AND NOT EXISTS (
		SELECT 1 FROM EditedMessage l WHERE l.contentHash = r.contentHash);
)SQL",
		R"SQL(
INSERT OR REPLACE INTO SpyTarget
SELECT r.* FROM remote.SpyTarget r;
)SQL",
		R"SQL(
INSERT INTO OnlineEvent (userId, timestamp, kind, onlineTill, manualLastSeen)
SELECT r.userId, r.timestamp, r.kind, r.onlineTill, r.manualLastSeen
FROM remote.OnlineEvent r
WHERE NOT EXISTS (
	SELECT 1 FROM OnlineEvent l
	WHERE l.userId = r.userId
		AND l.timestamp = r.timestamp
		AND l.kind = r.kind);
)SQL",
	};
	const auto peerArchiveQueries = std::array<const char*, 6>{
		R"SQL(
INSERT OR IGNORE INTO PeerArchiveProfile
SELECT r.* FROM remote.PeerArchiveProfile r
WHERE NOT EXISTS (
	SELECT 1 FROM PeerArchiveProfile l WHERE l.peerId = r.peerId);
)SQL",
		R"SQL(
UPDATE PeerArchiveProfile SET
	lastSeenAt = (
		SELECT MAX(r.lastSeenAt) FROM remote.PeerArchiveProfile r
		WHERE r.peerId = PeerArchiveProfile.peerId),
	firstSeenAt = (
		SELECT MIN(r.firstSeenAt) FROM remote.PeerArchiveProfile r
		WHERE r.peerId = PeerArchiveProfile.peerId AND r.firstSeenAt > 0)
WHERE EXISTS (
	SELECT 1 FROM remote.PeerArchiveProfile r
	WHERE r.peerId = PeerArchiveProfile.peerId);
)SQL",
		R"SQL(
INSERT OR IGNORE INTO PeerArchiveUsername
SELECT r.* FROM remote.PeerArchiveUsername r
WHERE NOT EXISTS (
	SELECT 1 FROM PeerArchiveUsername l
	WHERE l.peerId = r.peerId AND l.username = r.username);
)SQL",
		R"SQL(
INSERT OR IGNORE INTO PeerArchiveName
SELECT r.* FROM remote.PeerArchiveName r
WHERE NOT EXISTS (
	SELECT 1 FROM PeerArchiveName l
		WHERE l.peerId = r.peerId
		AND l.firstName = r.firstName
		AND l.lastName = r.lastName);
)SQL",
		R"SQL(
INSERT OR IGNORE INTO PeerArchiveChatMembership (
	peerId, chatId, chatTitle, firstSeenAt, lastSeenAt, selfInChat, peerStillInChat)
SELECT
	r.peerId, r.chatId, r.chatTitle, r.firstSeenAt, r.lastSeenAt, r.selfInChat, r.peerStillInChat
FROM remote.PeerArchiveChatMembership r
WHERE NOT EXISTS (
	SELECT 1 FROM PeerArchiveChatMembership l
	WHERE l.peerId = r.peerId AND l.chatId = r.chatId);
)SQL",
		R"SQL(
UPDATE PeerArchiveChatMembership SET
	lastSeenAt = (
		SELECT MAX(r.lastSeenAt) FROM remote.PeerArchiveChatMembership r
		WHERE r.peerId = PeerArchiveChatMembership.peerId
			AND r.chatId = PeerArchiveChatMembership.chatId),
	chatTitle = COALESCE((
		SELECT r.chatTitle FROM remote.PeerArchiveChatMembership r
		WHERE r.peerId = PeerArchiveChatMembership.peerId
			AND r.chatId = PeerArchiveChatMembership.chatId
		ORDER BY r.lastSeenAt DESC LIMIT 1), chatTitle),
	selfInChat = (
		SELECT MAX(r.selfInChat) FROM remote.PeerArchiveChatMembership r
		WHERE r.peerId = PeerArchiveChatMembership.peerId
			AND r.chatId = PeerArchiveChatMembership.chatId)
WHERE EXISTS (
	SELECT 1 FROM remote.PeerArchiveChatMembership r
	WHERE r.peerId = PeerArchiveChatMembership.peerId
		AND r.chatId = PeerArchiveChatMembership.chatId);
)SQL",
	};
	for (const auto *query : coreQueries) {
		if (!Exec(live, query)) {
			Exec(live, "DETACH remote;");
			return false;
		}
	}
	if (RemoteTableExists(live, "PeerArchiveProfile")) {
		for (const auto *query : peerArchiveQueries) {
			if (!Exec(live, query)) {
				Exec(live, "DETACH remote;");
				return false;
			}
		}
		if (RemoteTableExists(live, "PeerArchiveBio")) {
			if (!Exec(live, R"SQL(
INSERT OR IGNORE INTO PeerArchiveBio
SELECT r.* FROM remote.PeerArchiveBio r
WHERE NOT EXISTS (
	SELECT 1 FROM PeerArchiveBio l
	WHERE l.peerId = r.peerId AND l.bio = r.bio);
)SQL")) {
				Exec(live, "DETACH remote;");
				return false;
			}
		}
	}
	return Exec(live, "DETACH remote;");
}

[[nodiscard]] std::optional<EncryptedDbShard> BuildShard(
		SyncDatabaseKind kind,
		const QByteArray &syncKey) {
	const auto snapshotPath = SnapshotPath(kind);
	if (!CreateDatabaseSnapshot(kind, snapshotPath)) {
		return std::nullopt;
	}
	const auto compressed = CompressSnapshot(snapshotPath);
	QFile::remove(snapshotPath);
	if (compressed.isEmpty()) {
		return std::nullopt;
	}
	const auto encrypted = EncryptSnapshot(compressed, syncKey);
	if (encrypted.isEmpty()) {
		return std::nullopt;
	}
	return EncryptedDbShard{
		.kind = kind,
		.encrypted = encrypted,
		.fileName = u"data.tforge"_q,
		.sha256Hex = QString::fromLatin1(
			QCryptographicHash::hash(encrypted, QCryptographicHash::Sha256).toHex()),
		.dateFrom = 0,
		.dateTo = int(QDateTime::currentSecsSinceEpoch()),
	};
}

} // namespace

std::vector<EncryptedDbShard> BuildEncryptedDatabaseShards(const QByteArray &syncKey) {
	auto out = std::vector<EncryptedDbShard>();
	if (const auto shard = BuildShard(SyncDatabaseKind::Data, syncKey)) {
		out.push_back(*shard);
	}
	return out;
}

bool ApplyEncryptedDatabaseShard(
		SyncDatabaseKind kind,
		const QByteArray &encrypted,
		const QByteArray &syncKey) {
	const auto decrypted = DecryptSnapshot(encrypted, syncKey);
	if (!decrypted) {
		return false;
	}
	const auto tempPath = DecompressToTemp(*decrypted);
	if (!tempPath) {
		return false;
	}
	const auto livePath = LivePath(kind);
	if (livePath.isEmpty()) {
		QFile::remove(*tempPath);
		return false;
	}
	sqlite3 *live = nullptr;
	if (sqlite3_open_v2(
			livePath.toUtf8().constData(),
			&live,
			SQLITE_OPEN_READWRITE,
			nullptr) != SQLITE_OK
		|| !live) {
		QFile::remove(*tempPath);
		return false;
	}
	const auto ok = MergeDataDb(live, *tempPath);
	sqlite3_close(live);
	QFile::remove(*tempPath);
	return ok;
}

} // namespace TeleForge::Sync
