#pragma once

#include "ayu/features/teleforge/teleforge_inference.h"

#include "history/history.h"
#include "history/history_item.h"

#include <vector>

namespace TeleForge {

[[nodiscard]] std::vector<InferenceTurn> CollectInferenceTurns(
	not_null<History*> history,
	int maxTurns = 48);

void MaybeIngestHistoryItem(not_null<HistoryItem*> item);

void IngestOutgoingChatText(
	long long chatPeerStorageId,
	long long selfUserStorageId,
	const QString &plainText);

[[nodiscard]] QString BuildRetrievalQueryFromTurns(
	const std::vector<InferenceTurn> &chronologicalTurns,
	int lastN = 3);

} // namespace TeleForge
