#include "ayu/features/teleforge/teleforge_memory.h"

#include "ayu/features/sync/teleforge_sync.h"
#include "ayu/features/teleforge/teleforge_core.h"
#include "ayu/features/teleforge/teleforge_embeddings.h"
#include "ayu/features/teleforge/teleforge_rerank.h"
#include "ayu/features/teleforge/teleforge_storage.h"
#include "ayu/features/teleforge/teleforge_vector_db.h"
#include "core/application.h"
#include "main/main_account.h"

#include "base/unixtime.h"

#include <algorithm>
#include <cmath>

#include <condition_variable>
#include <mutex>

#include <crl/crl.h>

#include <QtCore/QCryptographicHash>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QStringList>
#include <QtCore/QSysInfo>

namespace TeleForge {
namespace {

[[nodiscard]] QString ScopeToString(MemoryScope scope) {
	switch (scope) {
	case MemoryScope::Global: return "global";
	case MemoryScope::Chat: return "chat";
	case MemoryScope::User: return "user";
	}
	return "global";
}

[[nodiscard]] MemoryScope ScopeFromString(const std::string &scope) {
	if (scope == "chat") {
		return MemoryScope::Chat;
	} else if (scope == "user") {
		return MemoryScope::User;
	}
	return MemoryScope::Global;
}

[[nodiscard]] QString CanonicalText(
		const QString &title,
		const QString &summary,
		const QString &details) {
	auto parts = QStringList();
	if (!title.trimmed().isEmpty()) {
		parts.push_back(title.trimmed());
	}
	if (!summary.trimmed().isEmpty()) {
		parts.push_back(summary.trimmed());
	}
	if (!details.trimmed().isEmpty()) {
		parts.push_back(details.trimmed());
	}
	return parts.join("\n");
}

[[nodiscard]] QString MergeText(const QString &current, const QString &incoming) {
	if (current.trimmed().isEmpty()) {
		return incoming.trimmed();
	} else if (incoming.trimmed().isEmpty() || current.contains(incoming, Qt::CaseInsensitive)) {
		return current.trimmed();
	} else if (incoming.contains(current, Qt::CaseInsensitive)) {
		return incoming.trimmed();
	}
	return current.trimmed() + "\n" + incoming.trimmed();
}

[[nodiscard]] QString ComputeHash(const QString &text) {
	return QString::fromLatin1(
		QCryptographicHash::hash(text.toUtf8(), QCryptographicHash::Sha256).toHex());
}

[[nodiscard]] int Now() {
	return base::unixtime::now();
}

[[nodiscard]] MemoryEntry FromRecord(const Storage::MemoryItemRecord &record) {
	return {
		.id = record.id,
		.scope = ScopeFromString(record.scopeType),
		.chatId = record.chatId,
		.userId = record.userId,
		.title = QString::fromStdString(record.title),
		.summary = QString::fromStdString(record.summary),
		.details = QString::fromStdString(record.details),
		.factType = QString::fromStdString(record.factType),
		.tagsJson = QString::fromStdString(record.tagsJson),
		.basePriority = record.basePriority,
		.stabilityScore = record.stabilityScore,
		.createdAt = record.createdAt,
		.updatedAt = record.updatedAt,
		.lastAccessAt = record.lastAccessAt,
		.accessCount = record.accessCount,
		.manualPinned = record.manualPinned,
		.manualHidden = record.manualHidden,
		.contentHash = QString::fromStdString(record.contentHash),
	};
}

[[nodiscard]] Storage::MemoryItemRecord ToRecord(
		const MemoryEntry &entry,
		std::optional<long long> sourcePeerId = std::nullopt,
		std::optional<int> sourceMessageId = std::nullopt) {
	return {
		.id = entry.id,
		.scopeType = ScopeToString(entry.scope).toStdString(),
		.chatId = entry.chatId,
		.userId = entry.userId,
		.sourcePeerId = sourcePeerId,
		.sourceMessageId = sourceMessageId,
		.title = entry.title.toStdString(),
		.summary = entry.summary.toStdString(),
		.details = entry.details.toStdString(),
		.factType = entry.factType.toStdString(),
		.tagsJson = entry.tagsJson.toStdString(),
		.basePriority = entry.basePriority,
		.stabilityScore = entry.stabilityScore,
		.createdAt = entry.createdAt,
		.updatedAt = entry.updatedAt,
		.lastAccessAt = entry.lastAccessAt,
		.accessCount = entry.accessCount,
		.manualPinned = entry.manualPinned,
		.manualHidden = entry.manualHidden,
		.syncState = 0,
		.contentHash = entry.contentHash.toStdString(),
	};
}

[[nodiscard]] bool SameScope(
		const Storage::MemoryItemRecord &record,
		MemoryScope scope,
		std::optional<long long> chatId,
		std::optional<long long> userId) {
	return ScopeFromString(record.scopeType) == scope
		&& record.chatId == chatId
		&& record.userId == userId
		&& !record.manualHidden;
}

[[nodiscard]] double AgeDecayPenalty(int updatedAt, double decayPerDay) {
	const auto ageSeconds = std::max(0, Now() - updatedAt);
	const auto ageDays = static_cast<double>(ageSeconds) / 86400.;
	return ageDays * decayPerDay;
}

[[nodiscard]] double ScopeBoost(MemoryScope scope) {
	switch (scope) {
	case MemoryScope::Global: return 0.04;
	case MemoryScope::Chat: return 0.025;
	case MemoryScope::User: return 0.015;
	}
	return 0.;
}

[[nodiscard]] double BuildEffectiveScore(
		double similarity,
		const MemoryEntry &entry,
		double decayPerDay) {
	const auto priority = std::clamp(entry.basePriority / 10., 0., 1.);
	const auto stability = std::clamp(entry.stabilityScore / 10., 0., 1.);
	const auto pinnedBoost = entry.manualPinned ? 0.30 : 0.0;
	const auto accessBoost = std::min(entry.accessCount, 10) * 0.015;
	return (similarity * 0.65)
		+ (priority * 0.20)
		+ (stability * 0.10)
		+ ScopeBoost(entry.scope)
		+ pinnedBoost
		+ accessBoost
		- AgeDecayPenalty(entry.updatedAt, decayPerDay);
}

void SortSnippetsByScore(std::vector<RetrievedMemorySnippet> &list) {
	std::sort(list.begin(), list.end(), [](const auto &left, const auto &right) {
		return left.effectiveScore > right.effectiveScore;
	});
}

void ApplyRerankPoolIfConfigured(
		const QString &query,
		double decayPerDay,
		std::vector<RetrievedMemorySnippet> &list) {
	if (list.empty()) {
		return;
	}
	auto cfg = CurrentRerankConfig();
	if (cfg.endpointUrl.trimmed().isEmpty()
		&& cfg.apiKind != RerankApiKind::NativeEmbeddings) {
		return;
	}
	if (cfg.modelPath.trimmed().isEmpty()) {
		cfg.modelPath = DefaultTeleForgeRerankGgufPath();
	}
	constexpr auto kPool = size_t(128);
	SortSnippetsByScore(list);
	const auto n = std::min(kPool, list.size());
	auto texts = QStringList();
	for (size_t i = 0; i < n; ++i) {
		texts.push_back(CanonicalText(
			list[i].entry.title,
			list[i].entry.summary,
			list[i].entry.details));
	}
	const auto queryCopy = query;
	const auto cfgCopy = cfg;
	std::optional<std::vector<float>> scores;
	{
		auto mutex = std::mutex();
		auto cv = std::condition_variable();
		auto done = false;
		crl::async([&] {
			const auto result = HttpRerankScores(queryCopy, texts, cfgCopy);
			{
				const auto lock = std::unique_lock(mutex);
				scores = result;
				done = true;
			}
			cv.notify_one();
		});
		auto lock = std::unique_lock(mutex);
		cv.wait(lock, [&] { return done; });
	}
	if (!scores || scores->size() != n) {
		return;
	}
	for (size_t i = 0; i < n; ++i) {
		const auto sim = std::clamp((*scores)[i], 0.f, 1.f);
		list[i].similarity = sim;
		list[i].effectiveScore = BuildEffectiveScore(sim, list[i].entry, decayPerDay);
	}
	SortSnippetsByScore(list);
}

[[nodiscard]] QString BuildBlock(
		const QString &label,
		const std::vector<RetrievedMemorySnippet> &snippets,
		int stableFactsLimit,
		int recentSummaryLimit) {
	if (snippets.empty()) {
		return QString();
	}

	auto stableLines = QStringList();
	auto recentLines = QStringList();
	for (const auto &snippet : snippets) {
		const auto line = QString("- %1").arg(
			!snippet.entry.summary.trimmed().isEmpty()
				? snippet.entry.summary.trimmed()
				: snippet.entry.details.trimmed());
		if (snippet.entry.stabilityScore >= 7.0 && stableLines.size() < stableFactsLimit) {
			stableLines.push_back(line);
		} else if (recentLines.size() < recentSummaryLimit) {
			recentLines.push_back(line);
		}
	}

	auto block = QStringList();
	block.push_back(label + ":");
	if (!stableLines.isEmpty()) {
		block.push_back("Stable facts:");
		block.append(stableLines);
	}
	if (!recentLines.isEmpty()) {
		block.push_back("Recent summaries:");
		block.append(recentLines);
	}
	return block.join('\n');
}

void PersistEmbedding(int memoryId, const EmbeddingVector &embedding) {
	Storage::upsertMemoryEmbedding({
		.memoryId = memoryId,
		.modelId = embedding.modelId.toStdString(),
		.dimensions = static_cast<int>(embedding.values.size()),
		.vectorBlob = SerializeEmbedding(embedding.values),
		.createdAt = Now(),
		.updatedAt = Now(),
	});
}

void EnqueueSyncEvent(const QString &eventType, const MemoryEntry &entry) {
	auto payload = QJsonObject{
		{ "memoryId", entry.id },
		{ "scope", ScopeToString(entry.scope) },
		{ "chatId", entry.chatId.has_value() ? QJsonValue(*entry.chatId) : QJsonValue() },
		{ "userId", entry.userId.has_value() ? QJsonValue(*entry.userId) : QJsonValue() },
		{ "title", entry.title },
		{ "summary", entry.summary },
		{ "details", entry.details },
		{ "updatedAt", entry.updatedAt },
	};
	Storage::enqueueMemorySyncEvent({
		.eventType = eventType.toStdString(),
		.memoryId = entry.id,
		.payload = QJsonDocument(payload).toJson(QJsonDocument::Compact).toStdString(),
		.deviceId = QSysInfo::machineHostName().toStdString(),
		.createdAt = Now(),
	});
	const auto core = LoadPersonalityCore();
	if (core && core->memorySyncEnabled) {
		if (const auto session = Core::App().maybePrimarySession()) {
			TeleForge::Sync::scheduleUploadDebounced(session);
		}
	}
}

} // namespace

MemorySettings LoadMemorySettings(long long chatId) {
	const auto settingsRecord = Storage::effectivePerChatSettings(chatId);
	return {
		.enabled = settingsRecord.memoryReadEnabled,
		.writeEnabled = settingsRecord.memoryWriteEnabled,
		.aiAnswer = settingsRecord.aiAnswer,
		.globalTopK = settingsRecord.globalMemoryTopK,
		.chatTopK = settingsRecord.chatMemoryTopK,
		.userTopK = settingsRecord.userMemoryTopK,
		.appendixTopK = settingsRecord.memoryAppendixTopK,
		.recentSummaryLimit = settingsRecord.recentSummaryLimit,
		.stableFactsLimit = settingsRecord.stableFactsLimit,
		.decayPerDay = settingsRecord.memoryDecayPerDay,
		.maxMemoryBlockChars = settingsRecord.maxMemoryBlockChars,
	};
}

std::vector<MemoryEntry> ListMemories(
		std::optional<MemoryScope> scope,
		std::optional<long long> chatId,
		std::optional<long long> userId,
		bool includeHidden) {
	const auto records = Storage::loadMemoryItems();
	auto result = std::vector<MemoryEntry>();
	for (const auto &record : records) {
		const auto entry = FromRecord(record);
		if (!includeHidden && entry.manualHidden) {
			continue;
		}
		if (scope.has_value() && entry.scope != *scope) {
			continue;
		}
		if (chatId.has_value() && entry.chatId != chatId) {
			continue;
		}
		if (userId.has_value() && entry.userId != userId) {
			continue;
		}
		result.push_back(entry);
	}
	return result;
}

std::optional<MemoryEntry> GetMemory(int id) {
	if (const auto record = Storage::loadMemoryItem(id)) {
		return FromRecord(*record);
	}
	return std::nullopt;
}

MemoryEntry UpsertMemory(const MemoryUpsertRequest &request) {
	auto incoming = MemoryEntry();
	incoming.scope = request.scope;
	incoming.chatId = request.chatId;
	incoming.userId = request.userId;
	incoming.title = request.title.trimmed();
	incoming.summary = request.summary.trimmed();
	incoming.details = request.details.trimmed();
	incoming.factType = request.factType.trimmed();
	incoming.tagsJson = request.tagsJson.trimmed().isEmpty() ? "[]" : request.tagsJson.trimmed();
	incoming.basePriority = request.basePriority;
	incoming.stabilityScore = request.stabilityScore;
	incoming.createdAt = Now();
	incoming.updatedAt = Now();
	incoming.lastAccessAt = Now();
	incoming.accessCount = 1;
	incoming.manualPinned = request.manualPinned;

	const auto canonical = CanonicalText(incoming.title, incoming.summary, incoming.details);
	incoming.contentHash = ComputeHash(canonical);
	const auto queryEmbedding = BuildLocalEmbedding(canonical);

	auto bestSimilarity = -1.0;
	std::optional<Storage::MemoryItemRecord> bestRecord;
	for (const auto &record : Storage::loadMemoryItems()) {
		if (!SameScope(record, request.scope, request.chatId, request.userId)) {
			continue;
		}
		const auto storedEmbedding = Storage::loadMemoryEmbedding(record.id);
		if (!storedEmbedding.has_value()) {
			continue;
		}
		const auto similarity = CosineSimilarity(
			queryEmbedding.values,
			DeserializeEmbedding(storedEmbedding->vectorBlob));
		if (similarity > bestSimilarity) {
			bestSimilarity = similarity;
			bestRecord = record;
		}
	}

	if (bestRecord.has_value()
		&& bestSimilarity >= kMemoryNearDuplicateMergeSimilarity) {
		auto merged = FromRecord(*bestRecord);
		merged.title = MergeText(merged.title, incoming.title);
		merged.summary = MergeText(merged.summary, incoming.summary);
		merged.details = MergeText(merged.details, incoming.details);
		merged.basePriority = std::max(merged.basePriority, incoming.basePriority);
		merged.stabilityScore = std::max(merged.stabilityScore, incoming.stabilityScore);
		merged.updatedAt = Now();
		merged.lastAccessAt = Now();
		merged.accessCount += 1;
		merged.manualPinned = merged.manualPinned || incoming.manualPinned;
		merged.contentHash = ComputeHash(
			CanonicalText(merged.title, merged.summary, merged.details));

		Storage::upsertMemoryItem(ToRecord(
			merged,
			request.sourcePeerId,
			request.sourceMessageId));
		PersistEmbedding(merged.id, BuildLocalEmbedding(
			CanonicalText(merged.title, merged.summary, merged.details)));
		EnqueueSyncEvent("merge", merged);
		VectorDb::facts().invalidate();
		return merged;
	}

	incoming.id = Storage::upsertMemoryItem(ToRecord(
		incoming,
		request.sourcePeerId,
		request.sourceMessageId));
	PersistEmbedding(incoming.id, queryEmbedding);
	EnqueueSyncEvent("create", incoming);
	VectorDb::facts().invalidate();
	return incoming;
}

bool UpdateMemory(const MemoryEntry &entry) {
	auto existing = Storage::loadMemoryItem(entry.id);
	if (!existing.has_value()) {
		return false;
	}
	auto updated = entry;
	updated.updatedAt = Now();
	updated.contentHash = ComputeHash(
		CanonicalText(updated.title, updated.summary, updated.details));
	Storage::upsertMemoryItem(ToRecord(
		updated,
		existing->sourcePeerId,
		existing->sourceMessageId));
	PersistEmbedding(updated.id, BuildLocalEmbedding(
		CanonicalText(updated.title, updated.summary, updated.details)));
	EnqueueSyncEvent("update", updated);
	VectorDb::facts().invalidate();
	return true;
}

bool HideMemory(int id, bool hidden) {
	auto existing = GetMemory(id);
	if (!existing.has_value()) {
		return false;
	}
	auto updated = *existing;
	updated.manualHidden = hidden;
	return UpdateMemory(updated);
}

void TouchMemory(int id) {
	auto existing = GetMemory(id);
	if (!existing.has_value()) {
		return;
	}
	auto updated = *existing;
	updated.lastAccessAt = Now();
	updated.updatedAt = Now();
	updated.accessCount += 1;
	UpdateMemory(updated);
}

RetrievedMemoryContext BuildMemoryContext(const RetrievalRequest &request) {
	auto context = RetrievedMemoryContext();
	if (!request.settings.enabled) {
		return context;
	}

	const auto queryEmbedding = BuildLocalEmbedding(request.queryText);
	auto global = std::vector<RetrievedMemorySnippet>();
	auto chat = std::vector<RetrievedMemorySnippet>();
	auto appendix = std::vector<RetrievedMemorySnippet>();
	auto userBuckets = QHash<long long, std::vector<RetrievedMemorySnippet>>();

	for (const auto &entry : ListMemories(std::nullopt, std::nullopt, std::nullopt, false)) {
		const auto embeddingRecord = Storage::loadMemoryEmbedding(entry.id);
		if (!embeddingRecord.has_value()) {
			continue;
		}
		const auto similarity = CosineSimilarity(
			queryEmbedding.values,
			DeserializeEmbedding(embeddingRecord->vectorBlob));
		auto snippet = RetrievedMemorySnippet{
			.entry = entry,
			.similarity = similarity,
			.effectiveScore = BuildEffectiveScore(
				similarity,
				entry,
				request.settings.decayPerDay),
		};
		if (entry.scope == MemoryScope::Global) {
			global.push_back(snippet);
		} else if (entry.scope == MemoryScope::Chat && entry.chatId == request.chatId) {
			chat.push_back(snippet);
		} else if (entry.scope == MemoryScope::User
			&& entry.userId.has_value()
			&& std::find(
				request.uniqueUserIds.begin(),
				request.uniqueUserIds.end(),
				*entry.userId) != request.uniqueUserIds.end()) {
			userBuckets[*entry.userId].push_back(snippet);
		}
		appendix.push_back(snippet);
	}

	SortSnippetsByScore(global);
	SortSnippetsByScore(chat);
	SortSnippetsByScore(appendix);
	for (auto i = userBuckets.begin(); i != userBuckets.end(); ++i) {
		SortSnippetsByScore(i.value());
	}

	ApplyRerankPoolIfConfigured(
		request.queryText,
		request.settings.decayPerDay,
		global);
	ApplyRerankPoolIfConfigured(
		request.queryText,
		request.settings.decayPerDay,
		chat);
	ApplyRerankPoolIfConfigured(
		request.queryText,
		request.settings.decayPerDay,
		appendix);
	for (auto i = userBuckets.begin(); i != userBuckets.end(); ++i) {
		ApplyRerankPoolIfConfigured(
			request.queryText,
			request.settings.decayPerDay,
			i.value());
	}

	if (global.size() > static_cast<size_t>(request.settings.globalTopK)) {
		global.resize(request.settings.globalTopK);
	}
	if (chat.size() > static_cast<size_t>(request.settings.chatTopK)) {
		chat.resize(request.settings.chatTopK);
	}

	context.globalBlock = BuildBlock(
		"Global memory",
		global,
		request.settings.stableFactsLimit,
		request.settings.recentSummaryLimit);
	context.chatBlock = BuildBlock(
		"Chat memory",
		chat,
		request.settings.stableFactsLimit,
		request.settings.recentSummaryLimit);

	auto selectedIds = std::vector<int>();
	for (const auto &snippet : global) {
		selectedIds.push_back(snippet.entry.id);
	}
	for (const auto &snippet : chat) {
		selectedIds.push_back(snippet.entry.id);
	}

	for (const auto userId : request.uniqueUserIds) {
		auto list = userBuckets.value(userId);
		if (list.empty()) {
			continue;
		}
		if (list.size() > static_cast<size_t>(request.settings.userTopK)) {
			list.resize(request.settings.userTopK);
		}
		for (const auto &snippet : list) {
			selectedIds.push_back(snippet.entry.id);
		}
		context.firstMessageUserBlocks.insert(
			userId,
			BuildBlock(
				QString("User memory %1").arg(userId),
				list,
				request.settings.stableFactsLimit,
				request.settings.recentSummaryLimit));
	}

	for (const auto &snippet : global) {
		context.selected.push_back(snippet);
	}
	for (const auto &snippet : chat) {
		context.selected.push_back(snippet);
	}
	for (const auto userId : request.uniqueUserIds) {
		auto list = userBuckets.value(userId);
		if (list.empty()) {
			continue;
		}
		if (list.size() > static_cast<size_t>(request.settings.userTopK)) {
			list.resize(request.settings.userTopK);
		}
		context.selected.insert(context.selected.end(), list.begin(), list.end());
	}

	auto appendixFiltered = std::vector<RetrievedMemorySnippet>();
	for (const auto &snippet : appendix) {
		if (std::find(selectedIds.begin(), selectedIds.end(), snippet.entry.id) != selectedIds.end()) {
			continue;
		}
		appendixFiltered.push_back(snippet);
		if (appendixFiltered.size() >= static_cast<size_t>(request.settings.appendixTopK)) {
			break;
		}
	}
	context.appendixBlock = BuildBlock(
		"Additional relevant memory",
		appendixFiltered,
		request.settings.stableFactsLimit,
		request.settings.recentSummaryLimit);
	context.selected.insert(
		context.selected.end(),
		appendixFiltered.begin(),
		appendixFiltered.end());

	return context;
}

} // namespace TeleForge
