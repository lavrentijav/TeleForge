#include "ayu/features/teleforge/teleforge_storage.h"

#include "ayu/features/teleforge/teleforge_unified_db.h"
#include "ayu/libs/sqlite/sqlite3.h"
#include "settings.h"

#include "base/unixtime.h"
#include "logs.h"

#include <memory>
#include <string>

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>

namespace TeleForge::Storage {
namespace {

constexpr auto kSchemaVersion = 14;

std::unique_ptr<UnifiedStorage> gTeleForgeDb;

void ResetTeleForgeDbConnection() {
	gTeleForgeDb.reset();
}

void RemoveTeleForgeDatabaseFiles(const QString &path) {
	QFile::remove(path);
	QFile::remove(path + QStringLiteral("-wal"));
	QFile::remove(path + QStringLiteral("-shm"));
}

[[nodiscard]] QString LegacyTeleForgeDatabasePathQt() {
	const auto wd = cWorkingDir();
	const QString base = wd.isEmpty() ? QStringLiteral(".") : wd;
	return QDir::cleanPath(base + QStringLiteral("/tdata/teleforge.db"));
}

[[nodiscard]] QString TeleForgeDatabasePathQt() {
	const auto wd = cWorkingDir();
	const QString base = wd.isEmpty() ? QStringLiteral(".") : wd;
	const auto path = QDir::cleanPath(
		base + QStringLiteral("/tdata/data.tforge"));
	QDir().mkpath(QFileInfo(path).absolutePath());
	const auto legacy = LegacyTeleForgeDatabasePathQt();
	if (!QFile::exists(path) && QFile::exists(legacy)) {
		if (QFile::rename(legacy, path)) {
			for (const auto suffix : { u"-wal"_q, u"-shm"_q }) {
				QFile::rename(legacy + suffix, path + suffix);
			}
			LOG(("TeleForge: renamed teleforge.db -> data.tforge"));
		}
	}
	return path;
}

[[nodiscard]] std::string TeleForgeDatabasePathUtf8() {
	return TeleForgeDatabasePathQt().toUtf8().toStdString();
}

[[nodiscard]] UnifiedStorage &DbImpl() {
	if (!gTeleForgeDb) {
		gTeleForgeDb = std::make_unique<UnifiedStorage>(
			MakeUnifiedStorage(TeleForgeDatabasePathUtf8()));
	}
	return *gTeleForgeDb;
}

} // namespace

UnifiedStorage &Db() {
	return DbImpl();
}

namespace {

void EnsureSchemaVersionRow() {
	if (!DbImpl().get_pointer<SchemaVersionRecord>(1)) {
		DbImpl().replace(SchemaVersionRecord{});
	}
}

void MigrateToV1() {
	// V1 introduces explicit schema versioning.
}

void MigrateToV2() {
	// V2 introduces memory storage and extended per-chat settings.
}

void MigrateToV3() {
	// V3 adds embedding / LM Studio endpoint columns on PersonalityCore.
}

void MigrateToV4() {
	// V4 adds memorySyncEnabled, maxMemoryBlockChars, global defaults row support.
}

void MigrateToV5() {
	// V5 adds rerank endpoint / model path columns on PersonalityCore (via sync_schema).
}

void MigrateToV6() {
	// V6 adds chatModelPath / chatModelId on PersonalityCore (via sync_schema).
}

[[nodiscard]] bool ExecSqlite(sqlite3 *db, const char *sql) {
	char *err = nullptr;
	const auto rc = sqlite3_exec(db, sql, nullptr, nullptr, &err);
	if (rc != SQLITE_OK) {
		const auto message = err ? QString::fromUtf8(err) : QString();
		if (err) {
			sqlite3_free(err);
		}
		LOG(("TeleForge SQL failed: %1").arg(message));
		return false;
	}
	return true;
}

void MigrateToV8() {
	const auto legacyPath = QDir::cleanPath(
		QString(cWorkingDir()) + QStringLiteral("/tdata/ayudata.db"));
	if (!QFile::exists(legacyPath)) {
		return;
	}
	LOG(("TeleForge: importing legacy ayudata.db into data.tforge"));
	sqlite3 *live = nullptr;
	const auto livePath = TeleForgeDatabasePathQt().toUtf8();
	if (sqlite3_open_v2(
			livePath.constData(),
			&live,
			SQLITE_OPEN_READWRITE,
			nullptr) != SQLITE_OK
		|| !live) {
		return;
	}
	const auto attach = u"ATTACH DATABASE '%1' AS legacy;"_q.arg(
		QString(legacyPath).replace('\'', "''"));
	if (!ExecSqlite(live, attach.toUtf8().constData())) {
		sqlite3_close(live);
		return;
	}
	const auto queries = {
		R"SQL(
INSERT OR IGNORE INTO AyuDataSchemaVersion (id, version)
SELECT id, version FROM legacy.SchemaVersion WHERE id = 1;
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
FROM legacy.DeletedMessage r
WHERE r.contentHash != ''
	AND NOT EXISTS (SELECT 1 FROM DeletedMessage l WHERE l.contentHash = r.contentHash);
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
FROM legacy.EditedMessage r
WHERE r.contentHash != ''
	AND NOT EXISTS (SELECT 1 FROM EditedMessage l WHERE l.contentHash = r.contentHash);
)SQL",
		R"SQL(
INSERT OR IGNORE INTO DeletedDialog
SELECT * FROM legacy.DeletedDialog;
)SQL",
		R"SQL(
INSERT OR REPLACE INTO RegexFilter
SELECT * FROM legacy.RegexFilter;
)SQL",
		R"SQL(
INSERT OR IGNORE INTO RegexFilterGlobalExclusion
SELECT * FROM legacy.RegexFilterGlobalExclusion;
)SQL",
		R"SQL(
INSERT OR IGNORE INTO SpyMessageRead
SELECT * FROM legacy.SpyMessageRead;
)SQL",
		R"SQL(
INSERT OR IGNORE INTO SpyMessageContentsRead
SELECT * FROM legacy.SpyMessageContentsRead;
)SQL",
		R"SQL(
INSERT OR REPLACE INTO SpyTarget
SELECT * FROM legacy.SpyTarget;
)SQL",
		R"SQL(
INSERT INTO OnlineEvent (userId, timestamp, kind, onlineTill, manualLastSeen)
SELECT r.userId, r.timestamp, r.kind, r.onlineTill, r.manualLastSeen
FROM legacy.OnlineEvent r
WHERE NOT EXISTS (
	SELECT 1 FROM OnlineEvent l
	WHERE l.userId = r.userId AND l.timestamp = r.timestamp AND l.kind = r.kind);
)SQL",
	};
	for (const auto *query : queries) {
		if (!ExecSqlite(live, query)) {
			(void)ExecSqlite(live, "DETACH legacy;");
			sqlite3_close(live);
			return;
		}
	}
	(void)ExecSqlite(live, "DETACH legacy;");
	sqlite3_close(live);
	const auto backup = legacyPath + u".migrated_"_q + QString::number(base::unixtime::now());
	QFile::rename(legacyPath, backup);
	QFile::remove(legacyPath + QStringLiteral("-wal"));
	QFile::remove(legacyPath + QStringLiteral("-shm"));
	LOG(("TeleForge: legacy ayudata.db imported and renamed to %1").arg(backup));
}

void MigrateToV7() {
	// V7 adds openAiApiKey / chatContextMessages on PersonalityCore (via sync_schema).
}

void MigrateToV9() {
	// V9 adds PeerArchive* tables (via sync_schema).
}

void MigrateToV10() {
	// V10 extends PeerArchive profile/userpic columns (via sync_schema).
}

void MigrateToV11() {
	// V11 adds PeerArchiveBio table (via sync_schema).
}

void MigrateToV12() {
	// V12 adds autoSendEnabled / visionEnabled on PersonalityCore (via sync_schema).
}

void MigrateToV13() {
	// V13 adds cloudBackend / pgConnString on PersonalityCore (via sync_schema).
}

void MigrateToV14() {
	// V14 adds pgVersion on PersonalityCore (via sync_schema).
}

void RunMigrations() {
	EnsureSchemaVersionRow();
	const auto version = DbImpl().get<SchemaVersionRecord>(1).version;
	LOG(("TeleForge: RunMigrations — SchemaVersion row is %1, target %2 (empty steps if already current)")
		.arg(version)
		.arg(kSchemaVersion));
	for (auto next = version + 1; next <= kSchemaVersion; ++next) {
		try {
			LOG(("TeleForge: migration transaction -> version %1").arg(next));
			DbImpl().begin_transaction();
			if (next == 1) {
				MigrateToV1();
			} else if (next == 2) {
				MigrateToV2();
			} else if (next == 3) {
				MigrateToV3();
			} else if (next == 4) {
				MigrateToV4();
			} else if (next == 5) {
				MigrateToV5();
			} else if (next == 6) {
				MigrateToV6();
			} else if (next == 7) {
				MigrateToV7();
			} else if (next == 8) {
				MigrateToV8();
			} else if (next == 9) {
				MigrateToV9();
			} else if (next == 10) {
				MigrateToV10();
			} else if (next == 11) {
				MigrateToV11();
			} else if (next == 12) {
				MigrateToV12();
			} else if (next == 13) {
				MigrateToV13();
			} else if (next == 14) {
				MigrateToV14();
			}
			DbImpl().replace(SchemaVersionRecord{
				.singletonId = 1,
				.version = next,
			});
			DbImpl().commit();
		} catch (const std::exception &ex) {
			DbImpl().rollback();
			LOG(("TeleForge migration %1 failed: %2").arg(next).arg(ex.what()));
			throw;
		}
	}
	LOG(("TeleForge: RunMigrations finished — SchemaVersion now %1")
		.arg(DbImpl().get<SchemaVersionRecord>(1).version));
}

} // namespace

void initialize() {
	const auto pathQt = TeleForgeDatabasePathQt();
	const auto tryInit = [&] {
		LOG(("TeleForge: sync_schema (preserve=true) pass 1 — sqlite_orm creates/updates schema; "
			"indexes must be listed before tables in make_storage because sync iterates objects in reverse order"));
		DbImpl().sync_schema(true);
		LOG(("TeleForge: sync_schema pass 1 done"));
		RunMigrations();
		LOG(("TeleForge: sync_schema pass 2 — after TeleForge SchemaVersion migrations"));
		DbImpl().sync_schema(true);
		LOG(("TeleForge: sync_schema pass 2 done"));
		LOG(("TeleForge: ensureGlobalTeleForgeDefaultsRow"));
		ensureGlobalTeleForgeDefaultsRow();
	};
	try {
		LOG(("TeleForge: Storage::initialize — cWorkingDir='%1', unified DB='%2'")
			.arg(cWorkingDir())
			.arg(pathQt));
		tryInit();
		LOG(("TeleForge: storage initialized OK | file=%1 | SchemaVersion=%2")
			.arg(pathQt)
			.arg(DbImpl().get<SchemaVersionRecord>(1).version));
	} catch (const std::exception &ex) {
		const auto what = QString::fromUtf8(ex.what());
		const auto recoverable = what.contains(
			QStringLiteral("no such table"),
			Qt::CaseInsensitive);
		if (recoverable) {
			LOG(("TeleForge: recoverable error (%1): deleting DB+journal and retrying — "
				"often partial file, mismatched WAL, or legacy schema from before index/table ordering fix")
				.arg(what));
			LOG(("TeleForge: removing files for path: %1").arg(pathQt));
			ResetTeleForgeDbConnection();
			RemoveTeleForgeDatabaseFiles(pathQt);
			try {
				tryInit();
				LOG(("TeleForge: storage initialized OK after recreate | SchemaVersion=%1")
					.arg(DbImpl().get<SchemaVersionRecord>(1).version));
			} catch (const std::exception &ex2) {
				LOG(("TeleForge storage initialization failed after recreate: %1").arg(ex2.what()));
			}
		} else {
			LOG(("TeleForge storage initialization failed (non-recoverable path): %1").arg(what));
		}
	}
}

std::optional<PersonalityCoreRecord> loadPersonalityCore() {
	try {
		if (const auto record = DbImpl().get_pointer<PersonalityCoreRecord>(1)) {
			return *record;
		}
		return std::nullopt;
	} catch (const std::exception &ex) {
		LOG(("TeleForge loadPersonalityCore failed: %1").arg(ex.what()));
		return std::nullopt;
	}
}

void upsertPersonalityCore(const PersonalityCoreRecord &record) {
	try {
		DbImpl().replace(record);
	} catch (const std::exception &ex) {
		LOG(("TeleForge upsertPersonalityCore failed: %1").arg(ex.what()));
	}
}

std::optional<PerChatSettingsRecord> loadPerChatSettings(long long peerId) {
	try {
		if (const auto record = DbImpl().get_pointer<PerChatSettingsRecord>(peerId)) {
			return *record;
		}
		return std::nullopt;
	} catch (const std::exception &ex) {
		LOG(("TeleForge loadPerChatSettings failed: %1").arg(ex.what()));
		return std::nullopt;
	}
}

void upsertPerChatSettings(const PerChatSettingsRecord &record) {
	try {
		DbImpl().replace(record);
	} catch (const std::exception &ex) {
		LOG(("TeleForge upsertPerChatSettings failed: %1").arg(ex.what()));
	}
}

PerChatSettingsRecord effectivePerChatSettings(long long peerId) {
	if (const auto specific = loadPerChatSettings(peerId)) {
		return *specific;
	}
	if (const auto global = loadPerChatSettings(kTeleForgeGlobalDefaultsPeerId)) {
		auto merged = *global;
		merged.peerId = peerId;
		return merged;
	}
	return PerChatSettingsRecord{ .peerId = peerId };
}

void removePerChatSettings(long long peerId) {
	if (peerId == kTeleForgeGlobalDefaultsPeerId) {
		return;
	}
	try {
		DbImpl().remove<PerChatSettingsRecord>(peerId);
	} catch (const std::exception &ex) {
		LOG(("TeleForge removePerChatSettings failed: %1").arg(ex.what()));
	}
}

void ensureGlobalTeleForgeDefaultsRow() {
	try {
		if (DbImpl().get_pointer<PerChatSettingsRecord>(kTeleForgeGlobalDefaultsPeerId)) {
			return;
		}
		auto row = PerChatSettingsRecord{};
		row.peerId = kTeleForgeGlobalDefaultsPeerId;
		row.updatedAt = base::unixtime::now();
		DbImpl().replace(row);
	} catch (const std::exception &ex) {
		LOG(("TeleForge ensureGlobalTeleForgeDefaultsRow failed: %1").arg(ex.what()));
	}
}

std::vector<MemoryItemRecord> loadMemoryItems() {
	try {
		return DbImpl().get_all<MemoryItemRecord>(
			order_by(&MemoryItemRecord::updatedAt).desc());
	} catch (const std::exception &ex) {
		LOG(("TeleForge loadMemoryItems failed: %1").arg(ex.what()));
		return {};
	}
}

std::optional<MemoryItemRecord> loadMemoryItem(int id) {
	try {
		if (const auto record = DbImpl().get_pointer<MemoryItemRecord>(id)) {
			return *record;
		}
		return std::nullopt;
	} catch (const std::exception &ex) {
		LOG(("TeleForge loadMemoryItem failed: %1").arg(ex.what()));
		return std::nullopt;
	}
}

int upsertMemoryItem(const MemoryItemRecord &record) {
	try {
		if (record.id > 0) {
			DbImpl().replace(record);
			return record.id;
		}
		return static_cast<int>(DbImpl().insert(record));
	} catch (const std::exception &ex) {
		LOG(("TeleForge upsertMemoryItem failed: %1").arg(ex.what()));
		return 0;
	}
}

void upsertMemoryEmbedding(const MemoryEmbeddingRecord &record) {
	try {
		DbImpl().replace(record);
	} catch (const std::exception &ex) {
		LOG(("TeleForge upsertMemoryEmbedding failed: %1").arg(ex.what()));
	}
}

std::optional<MemoryEmbeddingRecord> loadMemoryEmbedding(int memoryId) {
	try {
		if (const auto record = DbImpl().get_pointer<MemoryEmbeddingRecord>(memoryId)) {
			return *record;
		}
		return std::nullopt;
	} catch (const std::exception &ex) {
		LOG(("TeleForge loadMemoryEmbedding failed: %1").arg(ex.what()));
		return std::nullopt;
	}
}

void enqueueMemorySyncEvent(const MemorySyncEventRecord &record) {
	try {
		DbImpl().insert(record);
	} catch (const std::exception &ex) {
		LOG(("TeleForge enqueueMemorySyncEvent failed: %1").arg(ex.what()));
	}
}

std::vector<MemorySyncEventRecord> loadPendingMemorySyncEvents(int maxRows) {
	try {
		return DbImpl().get_all<MemorySyncEventRecord>(
			where(is_null(&MemorySyncEventRecord::dispatchedAt)),
			order_by(&MemorySyncEventRecord::createdAt).asc(),
			limit(maxRows));
	} catch (const std::exception &ex) {
		LOG(("TeleForge loadPendingMemorySyncEvents failed: %1").arg(ex.what()));
		return {};
	}
}

void markMemorySyncEventDispatched(int id, int dispatchedAt) {
	try {
		DbImpl().update_all(
			set(c(&MemorySyncEventRecord::dispatchedAt) = dispatchedAt),
			where(c(&MemorySyncEventRecord::id) == id));
	} catch (const std::exception &ex) {
		LOG(("TeleForge markMemorySyncEventDispatched failed: %1").arg(ex.what()));
	}
}

std::vector<SyncArtifactRecord> loadSyncArtifacts(const std::string &artifactType) {
	try {
		return DbImpl().get_all<SyncArtifactRecord>(
			where(column<SyncArtifactRecord>(&SyncArtifactRecord::artifactType) == artifactType),
			order_by(column<SyncArtifactRecord>(&SyncArtifactRecord::updatedAt)).desc());
	} catch (const std::exception &ex) {
		LOG(("TeleForge loadSyncArtifacts failed: %1").arg(ex.what()));
		return {};
	}
}

void storeSyncArtifact(const SyncArtifactRecord &record) {
	try {
		auto stored = record;
		if (stored.id <= 0) {
			const auto latest = DbImpl().select(max(&SyncArtifactRecord::id));
			stored.id = latest.empty() || !latest.front()
				? 1
				: (*latest.front() + 1);
		}
		DbImpl().replace(stored);
	} catch (const std::exception &ex) {
		LOG(("TeleForge storeSyncArtifact failed: %1").arg(ex.what()));
	}
}

QString databasePath() {
	return TeleForgeDatabasePathQt();
}

} // namespace TeleForge::Storage
