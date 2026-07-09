#pragma once

#include <QString>

#include <optional>
#include <vector>

namespace TeleForge::Sync {

// One memory embedding plus the memory metadata needed to make the cloud
// `teleforge_embeddings` table useful for server-side inspection / search.
// Unlike the encrypted snapshot blob, these rows are stored in plaintext.
struct EmbeddingRow {
	long long memoryId = 0;
	QString modelId;
	int dims = 0;
	std::vector<float> vector;
	QString scopeType;
	std::optional<long long> chatId;
	std::optional<long long> userId;
	QString title;
	QString summary;
	long long updatedAt = 0;
};

// Gathers all locally stored memory embeddings. MUST be called on the main
// thread (touches the SQLite unified DB).
[[nodiscard]] std::vector<EmbeddingRow> CollectEmbeddingRows();

} // namespace TeleForge::Sync
