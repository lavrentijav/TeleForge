#pragma once

#include <optional>
#include <vector>

#include <QtCore/QHash>
#include <QtCore/QString>

namespace TeleForge {

/// Cosine similarity at or above this merges into the nearest existing memory (same scope).
inline constexpr double kMemoryNearDuplicateMergeSimilarity = 0.92;

enum class MemoryScope {
	Global,
	Chat,
	User,
};

struct MemorySettings {
	bool enabled = true;
	bool writeEnabled = true;
	bool aiAnswer = true;
	int globalTopK = 6;
	int chatTopK = 8;
	int userTopK = 3;
	int appendixTopK = 6;
	int recentSummaryLimit = 6;
	int stableFactsLimit = 8;
	double decayPerDay = 0.05;
	int maxMemoryBlockChars = 12000;
};

struct MemoryEntry {
	int id = 0;
	MemoryScope scope = MemoryScope::Global;
	std::optional<long long> chatId;
	std::optional<long long> userId;
	QString title;
	QString summary;
	QString details;
	QString factType;
	QString tagsJson;
	double basePriority = 5.;
	double stabilityScore = 5.;
	int createdAt = 0;
	int updatedAt = 0;
	int lastAccessAt = 0;
	int accessCount = 0;
	bool manualPinned = false;
	bool manualHidden = false;
	QString contentHash;
};

struct MemoryUpsertRequest {
	MemoryScope scope = MemoryScope::Global;
	std::optional<long long> chatId;
	std::optional<long long> userId;
	std::optional<long long> sourcePeerId;
	std::optional<int> sourceMessageId;
	QString title;
	QString summary;
	QString details;
	QString factType;
	QString tagsJson = "[]";
	double basePriority = 5.;
	double stabilityScore = 5.;
	bool manualPinned = false;
};

struct RetrievedMemorySnippet {
	MemoryEntry entry;
	double similarity = 0.;
	double effectiveScore = 0.;
};

struct RetrievedMemoryContext {
	QString globalBlock;
	QString chatBlock;
	QHash<long long, QString> firstMessageUserBlocks;
	QString appendixBlock;
	std::vector<RetrievedMemorySnippet> selected;
};

struct RetrievalRequest {
	long long chatId = 0;
	QString queryText;
	std::vector<long long> uniqueUserIds;
	MemorySettings settings;
};

[[nodiscard]] MemorySettings LoadMemorySettings(long long chatId);
[[nodiscard]] std::vector<MemoryEntry> ListMemories(
	std::optional<MemoryScope> scope = std::nullopt,
	std::optional<long long> chatId = std::nullopt,
	std::optional<long long> userId = std::nullopt,
	bool includeHidden = false);
[[nodiscard]] std::optional<MemoryEntry> GetMemory(int id);
[[nodiscard]] MemoryEntry UpsertMemory(const MemoryUpsertRequest &request);
bool UpdateMemory(const MemoryEntry &entry);
bool HideMemory(int id, bool hidden = true);
void TouchMemory(int id);
[[nodiscard]] RetrievedMemoryContext BuildMemoryContext(
	const RetrievalRequest &request);

} // namespace TeleForge
