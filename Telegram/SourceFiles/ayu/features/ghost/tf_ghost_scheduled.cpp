// Copyright @Radolyn, 2026
#include "ayu/features/ghost/tf_ghost_scheduled.h"

#include "api/api_common.h"
#include "history/history_item.h"
#include "history/history.h"

#include <mutex>

namespace Ayu::GhostScheduled {
namespace {

auto g_mutex = std::mutex();
auto g_pendingRandomIds = base::flat_set<uint64>();
auto g_pendingRandomPeer = base::flat_map<uint64, PeerId>();
auto g_ghostScheduledLocalIds = base::flat_set<FullMsgId>();
auto g_ghostDeliveredIds = base::flat_set<FullMsgId>();
auto g_ghostPendingCountByPeer = base::flat_map<PeerId, int>();

void markDeliveredId(FullMsgId id) {
	if (!id) {
		return;
	}
	g_ghostDeliveredIds.emplace(id);
}

void decrementPendingCount(PeerId peerId) {
	const auto i = g_ghostPendingCountByPeer.find(peerId);
	if (i == end(g_ghostPendingCountByPeer) || i->second <= 0) {
		return;
	}
	if (--i->second <= 0) {
		g_ghostPendingCountByPeer.erase(i);
	}
}

void incrementPendingCount(PeerId peerId) {
	++g_ghostPendingCountByPeer[peerId];
}

} // namespace

void registerGhostSend(
		const Api::SendOptions &options,
		uint64 randomId,
		PeerId peerId) {
	if (!options.ghostDeferredSend || !randomId || !peerId) {
		return;
	}
	const auto lock = std::unique_lock(g_mutex);
	g_pendingRandomIds.emplace(randomId);
	g_pendingRandomPeer.emplace(randomId, peerId);
	incrementPendingCount(peerId);
}

void onRandomIdRegistered(uint64 randomId, FullMsgId itemId) {
	if (!randomId || !itemId) {
		return;
	}
	const auto lock = std::unique_lock(g_mutex);
	if (!g_pendingRandomIds.remove(randomId)) {
		return;
	}
	g_ghostScheduledLocalIds.emplace(itemId);
}

void onRandomIdUnregistered(uint64 randomId) {
	if (!randomId) {
		return;
	}
	const auto lock = std::unique_lock(g_mutex);
	if (!g_pendingRandomIds.remove(randomId)) {
		return;
	}
	const auto i = g_pendingRandomPeer.find(randomId);
	if (i != end(g_pendingRandomPeer)) {
		decrementPendingCount(i->second);
		g_pendingRandomPeer.erase(i);
	}
}

void onScheduledItemQueued(not_null<HistoryItem*> item) {
	const auto peerId = item->history()->peer->id;
	const auto lock = std::unique_lock(g_mutex);
	if (!g_ghostPendingCountByPeer.contains(peerId)) {
		return;
	}
	g_ghostScheduledLocalIds.emplace(item->fullId());
}

void onSentFromScheduled(not_null<HistoryItem*> item, MsgId sentId) {
	const auto peerId = item->history()->peer->id;
	const auto lock = std::unique_lock(g_mutex);
	g_ghostScheduledLocalIds.remove(item->fullId());
	decrementPendingCount(peerId);
	if (sentId) {
		markDeliveredId({ peerId, sentId });
	}
}

void acknowledgeDelivery(not_null<const HistoryItem*> item) {
	if (!item->out() || !item->isFromScheduled()) {
		return;
	}
	const auto peerId = item->history()->peer->id;
	const auto lock = std::unique_lock(g_mutex);
	if (g_ghostDeliveredIds.contains(item->fullId())) {
		return;
	}
	markDeliveredId(item->fullId());
	decrementPendingCount(peerId);
}

bool shouldSuppress(not_null<const HistoryItem*> item) {
	if (!item->out()) {
		return false;
	}
	const auto peerId = item->history()->peer->id;
	const auto lock = std::unique_lock(g_mutex);
	if (g_ghostDeliveredIds.contains(item->fullId())) {
		return true;
	}
	if (g_ghostScheduledLocalIds.contains(item->fullId())) {
		return true;
	}
	if (!item->isFromScheduled()) {
		return false;
	}
	const auto pending = g_ghostPendingCountByPeer.find(peerId);
	return pending != end(g_ghostPendingCountByPeer) && pending->second > 0;
}

} // namespace Ayu::GhostScheduled
