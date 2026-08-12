// Copyright @Radolyn, 2026

#include "ayu/features/teleforge/tf_deleted_restore.h"

#include "api/api_text_entities.h"
#include "ayu/ayu_settings.h"
#include "ayu/data/entities.h"
#include "ayu/data/messages_storage.h"
#include "ayu/utils/ayu_mapper.h"
#include "base/call_delayed.h"
#include "base/flat_set.h"
#include "data/data_channel.h"
#include "data/data_chat.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "dialogs/dialogs_key.h"
#include "history/history.h"
#include "history/history_item.h"
#include "main/main_session.h"
#include "ui/text/text_utilities.h"
#include "window/window_session_controller.h"

#include "logs.h"

#include <map>

namespace TeleForge::DeletedRestore {
namespace {

constexpr auto kRestoreDelayMs = crl::time(400);

// Restored rows are keyed by the storage row id, not by the original MsgId:
// the recreated item gets a local non-history id so it can never collide with
// the server id space, which means the stored row is the only stable identity
// we can use to keep a second pass from duplicating what is already on screen.
[[nodiscard]] auto &RestoredRows() {
	static auto value = std::map<PeerId, base::flat_set<ID>>();
	return value;
}

[[nodiscard]] PeerData *ResolveAuthor(
		not_null<History*> history,
		ID fromId) {
	if (!fromId) {
		return nullptr;
	}
	auto &owner = history->owner();
	if (const auto user = owner.userLoaded(UserId(fromId))) {
		return user;
	}
	if (const auto channel = owner.channelLoaded(ChannelId(fromId))) {
		return channel;
	}
	if (const auto chat = owner.chatLoaded(ChatId(fromId))) {
		return chat;
	}
	return nullptr;
}

void RestoreOne(
		not_null<History*> history,
		const AyuMessageBase &stored) {
	auto text = Ui::Text::WithEntities(QString::fromStdString(stored.text));
	if (text.text.isEmpty()) {
		return;
	}
	const auto entities = AyuMapper::deserializeTextWithEntities(
		stored.textEntities);
	text.entities = Api::EntitiesFromMTP(&history->session(), entities.v);

	const auto author = ResolveAuthor(history, stored.fromId);
	auto flags = MessageFlags(MessageFlag::Local | MessageFlag::HistoryEntry);
	if (author) {
		flags |= MessageFlag::HasFromId;
	}
	if (!stored.postAuthor.empty()) {
		flags |= MessageFlag::HasPostAuthor;
	}

	// addNewLocalMessage() appends to the bottom of the chat; a restored
	// deletion has to land at the point it was originally sent, so the item is
	// created directly and placed by date instead.
	const auto item = history->makeMessage({
		.id = history->nextNonHistoryEntryId(),
		.flags = flags,
		.from = author ? author->id : PeerId(0),
		.date = stored.date ? stored.date : stored.entityCreateDate,
		.postAuthor = QString::fromStdString(stored.postAuthor),
	}, text, MTP_messageMediaEmpty());
	history->insertMessageToBlocks(item);
	item->setDeleted();
}

} // namespace

void restoreForHistory(not_null<History*> history) {
	const auto &settings = AyuSettings::getInstance();
	if (!settings.saveDeletedMessages() || !settings.showDeletedInChat()) {
		return;
	}
	const auto peer = history->peer;
	const auto limit = settings.deletedRestoreLimit();
	if (limit <= 0) {
		return;
	}
	auto stored = AyuMessages::getDeletedMessages(peer, 0, 0, 0, limit);
	if (stored.empty()) {
		return;
	}
	auto &restored = RestoredRows()[peer->id];
	auto added = 0;
	for (const auto &message : stored) {
		if (!restored.emplace(message.fakeId).second) {
			continue;
		}
		if (message.messageId
			&& history->owner().message(peer->id, MsgId(message.messageId))) {
			continue;
		}
		RestoreOne(history, message);
		++added;
	}
	if (added) {
		LOG(("TeleForge: restored %1 deleted message(s) into chat %2")
			.arg(added)
			.arg(peer->id.value));
	}
}

void attachSession(not_null<Main::Session*> session) {
	const auto attach = [=](not_null<Window::SessionController*> controller) {
		controller->activeChatValue(
		) | rpl::on_next([=](Dialogs::Key key) {
			const auto peer = key.peer();
			if (!peer) {
				return;
			}
			base::call_delayed(kRestoreDelayMs, session, [=] {
				if (const auto history = session->data().historyLoaded(peer)) {
					restoreForHistory(history);
				}
			});
		}, controller->lifetime());
	};
	if (const auto controller = session->tryResolveWindow()) {
		attach(controller);
	} else {
		base::call_delayed(1000, session, [=] {
			if (const auto controller = session->tryResolveWindow()) {
				attach(controller);
			}
		});
	}
}

} // namespace TeleForge::DeletedRestore
