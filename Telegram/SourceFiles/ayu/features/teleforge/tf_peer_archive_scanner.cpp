// Copyright @Radolyn, 2026

#include "ayu/features/teleforge/tf_peer_archive_scanner.h"

#include "ayu/ayu_settings.h"
#include "ayu/features/teleforge/tf_peer_archive.h"
#include "ayu/features/teleforge/tf_peer_archive_crawler.h"
#include "apiwrap.h"
#include "api/api_chat_participants.h"
#include "base/flat_map.h"
#include "base/unixtime.h"
#include "data/data_channel.h"
#include "data/data_chat.h"
#include "data/data_peer_id.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/history_item_components.h"
#include "history/view/history_view_element.h"
#include "main/main_session.h"

namespace TeleForge::PeerArchive {
namespace {

[[nodiscard]] int ScanCooldownSeconds() {
	return AyuSettings::getInstance().archiveChatRefreshSeconds();
}
constexpr auto kParticipantsPerPage = 200;
constexpr auto kParticipantsFirstPage = 50;

[[nodiscard]] int MaxParticipants() {
	return AyuSettings::getInstance().archiveMaxParticipants();
}

auto &LastChatScan() {
	static auto result = base::flat_map<long long, int>();
	return result;
}

void ScanHistoryAuthors(
		not_null<Main::Session*> session,
		not_null<History*> history) {
	const auto chatPeer = history->peer;
	const auto limit = AyuSettings::getInstance().archiveHistoryScanLimit();
	auto seen = 0;
	for (auto i = history->blocks.rbegin(); i != history->blocks.rend(); ++i) {
		const auto &block = *i;
		for (auto j = block->messages.rbegin();
				j != block->messages.rend();
				++j) {
			if (limit > 0 && seen >= limit) {
				return;
			}
			++seen;
			const auto item = (*j)->data();
			if (const auto from = item->from()
				? item->from()->asUser()
				: nullptr) {
				if (!from->isSelf()) {
					queueMessageAuthor(session, from, chatPeer, item->id);
				}
			}
			if (const auto forwarded = item->Get<HistoryMessageForwarded>()) {
				if (const auto sender = forwarded->originalSender) {
					if (const auto from = sender->asUser()) {
						if (!from->isSelf()) {
							queueMessageAuthor(
								session,
								from,
								chatPeer,
								item->id);
						}
					}
				}
			}
		}
	}
}

void NoteChatParticipants(
		not_null<Main::Session*> session,
		not_null<PeerData*> chatPeer,
		const std::vector<not_null<UserData*>> &users) {
	for (const auto user : users) {
		queueMessageAuthor(session, user, chatPeer, MsgId(0));
	}
}

void ScanBasicChatParticipants(
		not_null<Main::Session*> session,
		not_null<ChatData*> chat) {
	auto users = std::vector<not_null<UserData*>>();
	users.reserve(chat->participants.size());
	for (const auto user : chat->participants) {
		users.push_back(user);
	}
	NoteChatParticipants(session, chat, users);
}

void RequestChannelParticipantsPage(
		not_null<Main::Session*> session,
		not_null<ChannelData*> channel,
		not_null<PeerData*> chatPeer,
		int offset,
		Fn<void()> done) {
	if (!channel->canViewMembers()) {
		if (done) {
			done();
		}
		return;
	}
	const auto weak = base::make_weak(session);
	session->api().request(MTPchannels_GetParticipants(
		channel->inputChannel(),
		MTP_channelParticipantsRecent(),
		MTP_int(offset),
		MTP_int(offset > 0 ? kParticipantsPerPage : kParticipantsFirstPage),
		MTP_long(0)
	)).done([=](const MTPchannels_ChannelParticipants &result) {
		if (!weak) {
			return;
		}
		result.match([&](const MTPDchannels_channelParticipants &data) {
			const auto &[availableCount, list] = (offset == 0)
				? Api::ChatParticipants::ParseRecent(channel, data)
				: Api::ChatParticipants::Parse(channel, data);
			auto users = std::vector<not_null<UserData*>>();
			users.reserve(list.size());
			for (const auto &participant : list) {
				if (!participant.isUser()) {
					continue;
				}
				if (const auto user = session->data().userLoaded(
						participant.userId())) {
					users.push_back(user);
				}
			}
			NoteChatParticipants(session, chatPeer, users);
			const auto loaded = int(list.size());
			const auto cap = MaxParticipants();
			if (loaded > 0
				&& offset + loaded < availableCount
				&& (cap <= 0 || offset + loaded < cap)) {
				RequestChannelParticipantsPage(
					session,
					channel,
					chatPeer,
					offset + loaded,
					done);
			} else if (done) {
				done();
			}
		}, [&](const MTPDchannels_channelParticipantsNotModified &) {
			if (done) {
				done();
			}
		});
	}).fail([=] {
		if (done) {
			done();
		}
	}).send();
}

void RequestBasicChatFull(
		not_null<Main::Session*> session,
		not_null<ChatData*> chat) {
	const auto weak = base::make_weak(session);
	session->api().request(MTPmessages_GetFullChat(
		chat->inputChat()
	)).done([=](const MTPmessages_ChatFull &result) {
		if (!weak) {
			return;
		}
		const auto &data = result.c_messages_chatFull();
		session->data().processUsers(data.vusers());
		session->data().processChats(data.vchats());
		data.vfull_chat().match([&](const MTPDchatFull &full) {
			Data::ApplyChatUpdate(chat, full);
			ScanBasicChatParticipants(session, chat);
		}, [](const MTPDchannelFull &) {
		});
	}).send();
}

} // namespace

void scanOpenedChat(
		not_null<Main::Session*> session,
		not_null<PeerData*> chatPeer) {
	if (!archiveEnabled()) {
		return;
	}
	if (chatPeer->isUser() || chatPeer->isSelf()) {
		return;
	}
	const auto chatId = static_cast<long long>(SerializePeerId(chatPeer->id));
	const auto now = base::unixtime::now();
	const auto last = LastChatScan().find(chatId);
	if (last != end(LastChatScan()) && (now - last->second) < ScanCooldownSeconds()) {
		return;
	}
	LastChatScan()[chatId] = now;

	noteChatPeerObserved(chatPeer);
	enqueueLinksFromText(session, chatPeer->about());

	if (const auto history = session->data().historyLoaded(chatPeer)) {
		ScanHistoryAuthors(session, history);
	}

	if (!AyuSettings::getInstance().archiveScanParticipants()) {
		return;
	}
	if (const auto chat = chatPeer->asChat()) {
		if (!chat->participants.empty()) {
			ScanBasicChatParticipants(session, chat);
		} else {
			RequestBasicChatFull(session, chat);
		}
	} else if (const auto channel = chatPeer->asChannel()) {
		const auto broadcastsAllowed
			= AyuSettings::getInstance().archiveScanBroadcasts();
		if (channel->isMegagroup()
			|| (channel->isBroadcast() && broadcastsAllowed)) {
			RequestChannelParticipantsPage(session, channel, chatPeer, 0, nullptr);
		}
	}
}

} // namespace TeleForge::PeerArchive
