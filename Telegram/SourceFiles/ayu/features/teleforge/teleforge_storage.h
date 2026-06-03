#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace TeleForge::Storage {

struct SchemaVersionRecord {
	int singletonId = 1;
	int version = 0;
};

struct PersonalityCoreRecord {
	int singletonId = 1;
	double aggression = 5.;
	double brevity = 5.;
	double emojis = 5.;
	double toxicity = 5.;
	double creativity = 5.;
	std::string systemPrompt;
	std::string sourceDevice;
	int updatedAt = 0;
	std::string embeddingEndpointUrl;
	std::string embeddingModelId;
	std::string lmStudioBaseUrl;
	std::string chatModelPath;
	std::string chatModelId;
	std::string openAiApiKey;
	int chatContextMessages = 40;
	bool memorySyncEnabled = false;
	std::string rerankEndpointUrl;
	std::string rerankModelId;
	std::string rerankModelPath;
};

/// Template row for chats without their own row in PerChatSettings.
inline constexpr long long kTeleForgeGlobalDefaultsPeerId = 0;

struct PerChatSettingsRecord {
	long long peerId = 0;
	bool aiAnswer = true;
	bool webAccess = false;
	bool calendarAccess = false;
	bool pcAgent = false;
	bool memoryReadEnabled = true;
	bool memoryWriteEnabled = true;
	int globalMemoryTopK = 6;
	int chatMemoryTopK = 8;
	int userMemoryTopK = 3;
	int memoryAppendixTopK = 6;
	int recentSummaryLimit = 6;
	int stableFactsLimit = 8;
	double memoryDecayPerDay = 0.05;
	std::string directoryWhitelistJson = "[]";
	int maxMemoryBlockChars = 12000;
	int updatedAt = 0;
};

struct MemoryItemRecord {
	int id = 0;
	std::string scopeType;
	std::optional<long long> chatId;
	std::optional<long long> userId;
	std::optional<long long> sourcePeerId;
	std::optional<int> sourceMessageId;
	std::string title;
	std::string summary;
	std::string details;
	std::string factType;
	std::string tagsJson = "[]";
	double basePriority = 5.;
	double stabilityScore = 5.;
	int createdAt = 0;
	int updatedAt = 0;
	int lastAccessAt = 0;
	int accessCount = 0;
	bool manualPinned = false;
	bool manualHidden = false;
	int syncState = 0;
	std::string contentHash;
};

struct MemoryEmbeddingRecord {
	int memoryId = 0;
	std::string modelId;
	int dimensions = 0;
	std::vector<char> vectorBlob;
	int createdAt = 0;
	int updatedAt = 0;
};

struct MemorySyncEventRecord {
	int id = 0;
	std::string eventType;
	int memoryId = 0;
	std::string payload;
	std::string deviceId;
	int createdAt = 0;
	std::optional<int> dispatchedAt;
};

struct SyncArtifactRecord {
	int id = 0;
	std::string artifactType;
	std::string artifactName;
	std::string payload;
	std::string deviceId;
	int updatedAt = 0;
};

void initialize();

std::optional<PersonalityCoreRecord> loadPersonalityCore();
void upsertPersonalityCore(const PersonalityCoreRecord &record);

std::optional<PerChatSettingsRecord> loadPerChatSettings(long long peerId);
void upsertPerChatSettings(const PerChatSettingsRecord &record);
[[nodiscard]] PerChatSettingsRecord effectivePerChatSettings(long long peerId);
void removePerChatSettings(long long peerId);
void ensureGlobalTeleForgeDefaultsRow();

std::vector<MemoryItemRecord> loadMemoryItems();
std::optional<MemoryItemRecord> loadMemoryItem(int id);
int upsertMemoryItem(const MemoryItemRecord &record);
void upsertMemoryEmbedding(const MemoryEmbeddingRecord &record);
std::optional<MemoryEmbeddingRecord> loadMemoryEmbedding(int memoryId);
void enqueueMemorySyncEvent(const MemorySyncEventRecord &record);
std::vector<MemorySyncEventRecord> loadPendingMemorySyncEvents(int limit = 128);
void markMemorySyncEventDispatched(int id, int dispatchedAt);

std::vector<SyncArtifactRecord> loadSyncArtifacts(const std::string &artifactType);
void storeSyncArtifact(const SyncArtifactRecord &record);

[[nodiscard]] QString databasePath();

} // namespace TeleForge::Storage
