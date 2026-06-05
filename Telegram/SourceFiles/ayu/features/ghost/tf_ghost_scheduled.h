// Copyright @Radolyn, 2026
#pragma once

#include "data/data_msg_id.h"

class HistoryItem;
class PeerData;

namespace Api {
struct SendOptions;
} // namespace Api

namespace Ayu::GhostScheduled {

void registerGhostSend(
	const Api::SendOptions &options,
	uint64 randomId,
	PeerId peerId);

void onRandomIdRegistered(uint64 randomId, FullMsgId itemId);

void onRandomIdUnregistered(uint64 randomId);

void onScheduledItemQueued(not_null<HistoryItem*> item);

void onSentFromScheduled(not_null<HistoryItem*> item, MsgId sentId);

void acknowledgeDelivery(not_null<const HistoryItem*> item);

[[nodiscard]] bool shouldSuppress(not_null<const HistoryItem*> item);

} // namespace Ayu::GhostScheduled
