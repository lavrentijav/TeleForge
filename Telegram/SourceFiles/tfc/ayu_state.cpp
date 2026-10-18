// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#include "tfc/ayu_state.h"

#include <deque>

namespace AyuState {
namespace {

constexpr auto kMaxHiddenPeers = 64;
constexpr auto kMaxHiddenPerPeer = 2000;

struct HiddenMessages {
	std::unordered_set<MsgId> ids;
	std::deque<MsgId> order;
};

std::unordered_map<PeerId, HiddenMessages> hiddenMessages;
std::deque<PeerId> hiddenPeerOrder;

void hideOne(PeerId peerId, MsgId messageId) {
	auto it = hiddenMessages.find(peerId);
	if (it == hiddenMessages.end()) {
		if (int(hiddenPeerOrder.size()) >= kMaxHiddenPeers) {
			hiddenMessages.erase(hiddenPeerOrder.front());
			hiddenPeerOrder.pop_front();
		}
		hiddenPeerOrder.push_back(peerId);
		it = hiddenMessages.emplace(peerId, HiddenMessages()).first;
	}
	auto &peer = it->second;
	if (!peer.ids.insert(messageId).second) {
		return;
	}
	peer.order.push_back(messageId);
	if (int(peer.order.size()) > kMaxHiddenPerPeer) {
		peer.ids.erase(peer.order.front());
		peer.order.pop_front();
	}
}

} // namespace

void hide(PeerId peerId, MsgId messageId) {
	hideOne(peerId, messageId);
}

void hide(not_null<HistoryItem*> item) {
	hide(item->history()->peer->id, item->id);
}

bool isHidden(PeerId peerId, MsgId messageId) {
	const auto it = hiddenMessages.find(peerId);
	if (it != hiddenMessages.end()) {
		return it->second.ids.contains(messageId);
	}
	return false;
}

bool isHidden(not_null<HistoryItem*> item) {
	return isHidden(item->history()->peer->id, item->id);
}

}
