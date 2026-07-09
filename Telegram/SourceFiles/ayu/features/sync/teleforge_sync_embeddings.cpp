#include "ayu/features/sync/teleforge_sync_embeddings.h"

#include "ayu/features/teleforge/teleforge_embeddings.h"
#include "ayu/features/teleforge/teleforge_storage.h"

#include <utility>

namespace TeleForge::Sync {

std::vector<EmbeddingRow> CollectEmbeddingRows() {
	auto rows = std::vector<EmbeddingRow>();
	const auto items = TeleForge::Storage::loadMemoryItems();
	rows.reserve(items.size());
	for (const auto &item : items) {
		const auto embedding = TeleForge::Storage::loadMemoryEmbedding(item.id);
		if (!embedding) {
			continue;
		}
		auto vec = TeleForge::DeserializeEmbedding(embedding->vectorBlob);
		if (vec.empty()) {
			continue;
		}
		rows.push_back(EmbeddingRow{
			.memoryId = item.id,
			.modelId = QString::fromStdString(embedding->modelId),
			.dims = embedding->dimensions,
			.vector = std::move(vec),
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
