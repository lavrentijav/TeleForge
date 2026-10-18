#include "tfc/features/sync/teleforge_sync_embeddings.h"

#include "tfc/features/teleforge/teleforge_storage.h"

#include <utility>

namespace TeleForge::Sync {

std::vector<EmbeddingRow> CollectEmbeddingRows() {
	auto rows = std::vector<EmbeddingRow>();
	const auto items = TeleForge::Storage::loadMemoryItems();
	rows.reserve(items.size());
	for (const auto &item : items) {
		auto embedding = TeleForge::Storage::loadMemoryEmbedding(item.id);
		if (!embedding || embedding->vectorBlob.empty()) {
			continue;
		}
		rows.push_back(EmbeddingRow{
			.memoryId = item.id,
			.modelId = QString::fromStdString(embedding->modelId),
			.dims = embedding->dimensions,
			.vectorBlob = std::move(embedding->vectorBlob),
			.scopeType = QString::fromStdString(item.scopeType),
			.chatId = item.chatId,
			.userId = item.userId,
			.title = QString::fromStdString(item.title),
			.summary = QString::fromStdString(item.summary),
			.updatedAt = item.updatedAt,
		});
	}
	return rows;
}

} // namespace TeleForge::Sync
