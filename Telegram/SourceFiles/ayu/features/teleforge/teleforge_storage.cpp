#include "ayu/features/teleforge/teleforge_storage.h"

#include "ayu/libs/sqlite/sqlite_orm.h"
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

using namespace sqlite_orm;

constexpr auto kSchemaVersion = 7;

[[nodiscard]] auto MakeStorage(std::string dbPath) {
	// sqlite_orm::storage_t::sync_schema iterates db_objects in *reverse* order;
	// indexes must appear first in this list so tables are created before CREATE INDEX runs.
	return make_storage(
		std::move(dbPath),
		make_index(
			"idx_sync_artifact_type_updated",
			column<SyncArtifactRecord>(&SyncArtifactRecord::artifactType),
			column<SyncArtifactRecord>(&SyncArtifactRecord::updatedAt)),
		make_index(
			"idx_memory_scope_lookup",
			column<MemoryItemRecord>(&MemoryItemRecord::scopeType),
			column<MemoryItemRecord>(&MemoryItemRecord::chatId),
			column<MemoryItemRecord>(&MemoryItemRecord::userId),
			column<MemoryItemRecord>(&MemoryItemRecord::manualHidden)),
		make_index(
			"idx_memory_access",
			column<MemoryItemRecord>(&MemoryItemRecord::lastAccessAt),
			column<MemoryItemRecord>(&MemoryItemRecord::updatedAt)),
		make_index(
			"idx_memory_sync_pending",
			column<MemorySyncEventRecord>(&MemorySyncEventRecord::dispatchedAt),
			column<MemorySyncEventRecord>(&MemorySyncEventRecord::createdAt)),
		make_table<SchemaVersionRecord>(
			"SchemaVersion",
			make_column("singletonId", &SchemaVersionRecord::singletonId, primary_key()),
			make_column("version", &SchemaVersionRecord::version)),
		make_table<PersonalityCoreRecord>(
			"PersonalityCore",
			make_column("singletonId", &PersonalityCoreRecord::singletonId, primary_key()),
			make_column("aggression", &PersonalityCoreRecord::aggression),
			make_column("brevity", &PersonalityCoreRecord::brevity),
			make_column("emojis", &PersonalityCoreRecord::emojis),
			make_column("toxicity", &PersonalityCoreRecord::toxicity),
			make_column("creativity", &PersonalityCoreRecord::creativity),
			make_column("systemPrompt", &PersonalityCoreRecord::systemPrompt),
			make_column("sourceDevice", &PersonalityCoreRecord::sourceDevice),
			make_column("updatedAt", &PersonalityCoreRecord::updatedAt),
			make_column("embeddingEndpointUrl", &PersonalityCoreRecord::embeddingEndpointUrl),
			make_column("embeddingModelId", &PersonalityCoreRecord::embeddingModelId),
			make_column("lmStudioBaseUrl", &PersonalityCoreRecord::lmStudioBaseUrl),
			make_column("chatModelPath", &PersonalityCoreRecord::chatModelPath),
			make_column("chatModelId", &PersonalityCoreRecord::chatModelId),
			make_column("openAiApiKey", &PersonalityCoreRecord::openAiApiKey),
			make_column("chatContextMessages", &PersonalityCoreRecord::chatContextMessages),
			make_column("memorySyncEnabled", &PersonalityCoreRecord::memorySyncEnabled),
			make_column("rerankEndpointUrl", &PersonalityCoreRecord::rerankEndpointUrl),
			make_column("rerankModelId", &PersonalityCoreRecord::rerankModelId),
			make_column("rerankModelPath", &PersonalityCoreRecord::rerankModelPath)),
		make_table<PerChatSettingsRecord>(
			"PerChatSettings",
			make_column("peerId", &PerChatSettingsRecord::peerId, primary_key()),
			make_column("aiAnswer", &PerChatSettingsRecord::aiAnswer),
			make_column("webAccess", &PerChatSettingsRecord::webAccess),
			make_column("calendarAccess", &PerChatSettingsRecord::calendarAccess),
			make_column("pcAgent", &PerChatSettingsRecord::pcAgent),
			make_column("memoryReadEnabled", &PerChatSettingsRecord::memoryReadEnabled),
			make_column("memoryWriteEnabled", &PerChatSettingsRecord::memoryWriteEnabled),
			make_column("globalMemoryTopK", &PerChatSettingsRecord::globalMemoryTopK),
			make_column("chatMemoryTopK", &PerChatSettingsRecord::chatMemoryTopK),
			make_column("userMemoryTopK", &PerChatSettingsRecord::userMemoryTopK),
			make_column("memoryAppendixTopK", &PerChatSettingsRecord::memoryAppendixTopK),
			make_column("recentSummaryLimit", &PerChatSettingsRecord::recentSummaryLimit),
			make_column("stableFactsLimit", &PerChatSettingsRecord::stableFactsLimit),
			make_column("memoryDecayPerDay", &PerChatSettingsRecord::memoryDecayPerDay),
			make_column("directoryWhitelistJson", &PerChatSettingsRecord::directoryWhitelistJson),
			make_column("maxMemoryBlockChars", &PerChatSettingsRecord::maxMemoryBlockChars),
			make_column("updatedAt", &PerChatSettingsRecord::updatedAt)),
		make_table<MemoryItemRecord>(
			"MemoryItem",
			make_column("id", &MemoryItemRecord::id, primary_key().autoincrement()),
			make_column("scopeType", &MemoryItemRecord::scopeType),
			make_column("chatId", &MemoryItemRecord::chatId),
			make_column("userId", &MemoryItemRecord::userId),
			make_column("sourcePeerId", &MemoryItemRecord::sourcePeerId),
			make_column("sourceMessageId", &MemoryItemRecord::sourceMessageId),
			make_column("title", &MemoryItemRecord::title),
			make_column("summary", &MemoryItemRecord::summary),
			make_column("details", &MemoryItemRecord::details),
			make_column("factType", &MemoryItemRecord::factType),
			make_column("tagsJson", &MemoryItemRecord::tagsJson),
			make_column("basePriority", &MemoryItemRecord::basePriority),
			make_column("stabilityScore", &MemoryItemRecord::stabilityScore),
			make_column("createdAt", &MemoryItemRecord::createdAt),
			make_column("updatedAt", &MemoryItemRecord::updatedAt),
			make_column("lastAccessAt", &MemoryItemRecord::lastAccessAt),
			make_column("accessCount", &MemoryItemRecord::accessCount),
			make_column("manualPinned", &MemoryItemRecord::manualPinned),
			make_column("manualHidden", &MemoryItemRecord::manualHidden),
			make_column("syncState", &MemoryItemRecord::syncState),
			make_column("contentHash", &MemoryItemRecord::contentHash)),
		make_table<MemoryEmbeddingRecord>(
			"MemoryEmbedding",
			make_column("memoryId", &MemoryEmbeddingRecord::memoryId, primary_key()),
			make_column("modelId", &MemoryEmbeddingRecord::modelId),
			make_column("dimensions", &MemoryEmbeddingRecord::dimensions),
			make_column("vectorBlob", &MemoryEmbeddingRecord::vectorBlob),
			make_column("createdAt", &MemoryEmbeddingRecord::createdAt),
			make_column("updatedAt", &MemoryEmbeddingRecord::updatedAt)),
		make_table<MemorySyncEventRecord>(
			"MemorySyncEvent",
			make_column("id", &MemorySyncEventRecord::id, primary_key().autoincrement()),
			make_column("eventType", &MemorySyncEventRecord::eventType),
			make_column("memoryId", &MemorySyncEventRecord::memoryId),
			make_column("payload", &MemorySyncEventRecord::payload),
			make_column("deviceId", &MemorySyncEventRecord::deviceId),
			make_column("createdAt", &MemorySyncEventRecord::createdAt),
			make_column("dispatchedAt", &MemorySyncEventRecord::dispatchedAt)),
		make_table<SyncArtifactRecord>(
			"SyncArtifact",
			make_column("id", &SyncArtifactRecord::id, primary_key().autoincrement()),
			make_column("artifactType", &SyncArtifactRecord::artifactType),
			make_column("artifactName", &SyncArtifactRecord::artifactName),
			make_column("payload", &SyncArtifactRecord::payload),
			make_column("deviceId", &SyncArtifactRecord::deviceId),
			make_column("updatedAt", &SyncArtifactRecord::updatedAt)));
}

using TeleForgeStorage = decltype(MakeStorage(std::string()));

std::unique_ptr<TeleForgeStorage> gTeleForgeDb;

void ResetTeleForgeDbConnection() {
	gTeleForgeDb.reset();
}

void RemoveTeleForgeDatabaseFiles(const QString &path) {
	QFile::remove(path);
	QFile::remove(path + QStringLiteral("-wal"));
	QFile::remove(path + QStringLiteral("-shm"));
}

[[nodiscard]] QString TeleForgeDatabasePathQt() {
	const auto wd = cWorkingDir();
	const QString base = wd.isEmpty() ? QStringLiteral(".") : wd;
	const QString path = QDir::cleanPath(
		base + QStringLiteral("/tdata/teleforge.db"));
	QDir().mkpath(QFileInfo(path).absolutePath());
	return path;
}

[[nodiscard]] std::string TeleForgeDatabasePathUtf8() {
	return TeleForgeDatabasePathQt().toUtf8().toStdString();
}

[[nodiscard]] TeleForgeStorage &Db() {
	if (!gTeleForgeDb) {
		gTeleForgeDb = std::make_unique<TeleForgeStorage>(
			MakeStorage(TeleForgeDatabasePathUtf8()));
	}
	return *gTeleForgeDb;
}

void EnsureSchemaVersionRow() {
	if (!Db().get_pointer<SchemaVersionRecord>(1)) {
		Db().replace(SchemaVersionRecord{});
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

void MigrateToV7() {
	// V7 adds openAiApiKey / chatContextMessages on PersonalityCore (via sync_schema).
}

void RunMigrations() {
	EnsureSchemaVersionRow();
	const auto version = Db().get<SchemaVersionRecord>(1).version;
	LOG(("TeleForge: RunMigrations — SchemaVersion row is %1, target %2 (empty steps if already current)")
		.arg(version)
		.arg(kSchemaVersion));
	for (auto next = version + 1; next <= kSchemaVersion; ++next) {
		try {
			LOG(("TeleForge: migration transaction -> version %1").arg(next));
			Db().begin_transaction();
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
			}
			Db().replace(SchemaVersionRecord{
				.singletonId = 1,
				.version = next,
			});
			Db().commit();
		} catch (const std::exception &ex) {
			Db().rollback();
			LOG(("TeleForge migration %1 failed: %2").arg(next).arg(ex.what()));
			throw;
		}
	}
	LOG(("TeleForge: RunMigrations finished — SchemaVersion now %1")
		.arg(Db().get<SchemaVersionRecord>(1).version));
}

} // namespace

void initialize() {
	const auto pathQt = TeleForgeDatabasePathQt();
	const auto tryInit = [&] {
		LOG(("TeleForge: sync_schema (preserve=true) pass 1 — sqlite_orm creates/updates schema; "
			"indexes must be listed before tables in make_storage because sync iterates objects in reverse order"));
		Db().sync_schema(true);
		LOG(("TeleForge: sync_schema pass 1 done"));
		RunMigrations();
		LOG(("TeleForge: sync_schema pass 2 — after TeleForge SchemaVersion migrations"));
		Db().sync_schema(true);
		LOG(("TeleForge: sync_schema pass 2 done"));
		LOG(("TeleForge: ensureGlobalTeleForgeDefaultsRow"));
		ensureGlobalTeleForgeDefaultsRow();
	};
	try {
		LOG(("TeleForge: Storage::initialize — cWorkingDir='%1', file='%2'")
			.arg(cWorkingDir())
			.arg(pathQt));
		tryInit();
		LOG(("TeleForge: storage initialized OK | file=%1 | SchemaVersion=%2")
			.arg(pathQt)
			.arg(Db().get<SchemaVersionRecord>(1).version));
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
					.arg(Db().get<SchemaVersionRecord>(1).version));
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
		if (const auto record = Db().get_pointer<PersonalityCoreRecord>(1)) {
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
		Db().replace(record);
	} catch (const std::exception &ex) {
		LOG(("TeleForge upsertPersonalityCore failed: %1").arg(ex.what()));
	}
}

std::optional<PerChatSettingsRecord> loadPerChatSettings(long long peerId) {
	try {
		if (const auto record = Db().get_pointer<PerChatSettingsRecord>(peerId)) {
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
		Db().replace(record);
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
		Db().remove<PerChatSettingsRecord>(peerId);
	} catch (const std::exception &ex) {
		LOG(("TeleForge removePerChatSettings failed: %1").arg(ex.what()));
	}
}

void ensureGlobalTeleForgeDefaultsRow() {
	try {
		if (Db().get_pointer<PerChatSettingsRecord>(kTeleForgeGlobalDefaultsPeerId)) {
			return;
		}
		auto row = PerChatSettingsRecord{};
		row.peerId = kTeleForgeGlobalDefaultsPeerId;
		row.updatedAt = base::unixtime::now();
		Db().replace(row);
	} catch (const std::exception &ex) {
		LOG(("TeleForge ensureGlobalTeleForgeDefaultsRow failed: %1").arg(ex.what()));
	}
}

std::vector<MemoryItemRecord> loadMemoryItems() {
	try {
		return Db().get_all<MemoryItemRecord>(
			order_by(&MemoryItemRecord::updatedAt).desc());
	} catch (const std::exception &ex) {
		LOG(("TeleForge loadMemoryItems failed: %1").arg(ex.what()));
		return {};
	}
}

std::optional<MemoryItemRecord> loadMemoryItem(int id) {
	try {
		if (const auto record = Db().get_pointer<MemoryItemRecord>(id)) {
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
			Db().replace(record);
			return record.id;
		}
		return static_cast<int>(Db().insert(record));
	} catch (const std::exception &ex) {
		LOG(("TeleForge upsertMemoryItem failed: %1").arg(ex.what()));
		return 0;
	}
}

void upsertMemoryEmbedding(const MemoryEmbeddingRecord &record) {
	try {
		Db().replace(record);
	} catch (const std::exception &ex) {
		LOG(("TeleForge upsertMemoryEmbedding failed: %1").arg(ex.what()));
	}
}

std::optional<MemoryEmbeddingRecord> loadMemoryEmbedding(int memoryId) {
	try {
		if (const auto record = Db().get_pointer<MemoryEmbeddingRecord>(memoryId)) {
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
		Db().insert(record);
	} catch (const std::exception &ex) {
		LOG(("TeleForge enqueueMemorySyncEvent failed: %1").arg(ex.what()));
	}
}

std::vector<MemorySyncEventRecord> loadPendingMemorySyncEvents(int maxRows) {
	try {
		return Db().get_all<MemorySyncEventRecord>(
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
		Db().update_all(
			set(c(&MemorySyncEventRecord::dispatchedAt) = dispatchedAt),
			where(c(&MemorySyncEventRecord::id) == id));
	} catch (const std::exception &ex) {
		LOG(("TeleForge markMemorySyncEventDispatched failed: %1").arg(ex.what()));
	}
}

std::vector<SyncArtifactRecord> loadSyncArtifacts(const std::string &artifactType) {
	try {
		return Db().get_all<SyncArtifactRecord>(
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
			const auto latest = Db().select(max(&SyncArtifactRecord::id));
			stored.id = latest.empty() || !latest.front()
				? 1
				: (*latest.front() + 1);
		}
		Db().replace(stored);
	} catch (const std::exception &ex) {
		LOG(("TeleForge storeSyncArtifact failed: %1").arg(ex.what()));
	}
}

QString databasePath() {
	return TeleForgeDatabasePathQt();
}

} // namespace TeleForge::Storage
