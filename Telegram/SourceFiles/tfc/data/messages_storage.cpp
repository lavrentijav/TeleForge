// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#include "tfc/data/messages_storage.h"

#include "tfc/tf_settings.h"
#include "tfc/data/tf_database.h"
#include "tfc/utils/tf_mapper.h"
#include "tfc/utils/telegram_helpers.h"
#include "base/unixtime.h"
#include "data/data_forum_topic.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/history_item_components.h"
#include "main/main_session.h"

#include <algorithm>

namespace TfMessages {

namespace {

[[nodiscard]] std::string PrependStageLabel(
		const std::string &label,
		const std::string &text) {
	return text.empty() ? label : (label + "\n" + text);
}

[[nodiscard]] TfMessageBase WithStageLabel(
		TfMessageBase message,
		const std::string &label) {
	message.text = PrependStageLabel(label, message.text);
	message.textEntities.clear();
	return message;
}

} // namespace

template<typename DerivedMessage>
std::vector<TfMessageBase> convertToBase(const std::vector<DerivedMessage> &messages) {
	std::vector<TfMessageBase> based;
	based.reserve(messages.size());
	for (const auto &msg : messages) {
		based.push_back(static_cast<TfMessageBase>(msg));
	}
	return based;
}

void map(not_null<HistoryItem*> item, TfMessageBase &message) {
	const ID userId = item->history()->owner().session().userId().bare & PeerId::kChatTypeMask;

	message.userId = userId;
	message.dialogId = getDialogIdFromPeer(item->history()->peer);
	message.groupedId = item->groupId().raw();
	message.peerId = item->history()->peer->id.value & PeerId::kChatTypeMask;
	message.fromId = item->from()->id.value & PeerId::kChatTypeMask;
	if (item->topic()) {
		message.topicId = item->topicRootId().bare;
	} else {
		message.topicId = 0;
	}
	message.messageId = item->id.bare;
	message.date = item->date();
	message.flags = TfMapper::mapItemFlagsToMTPFlags(item);

	if (const auto edited = item->Get<HistoryMessageEdited>()) {
		message.editDate = edited->date;
	} else {
		message.editDate = base::unixtime::now();
	}

	message.views = item->viewsCount();
	message.fwdFlags = 0;
	message.fwdFromId = 0;
	// message.fwdName
	message.fwdDate = 0;
	// message.fwdPostAuthor
	if (const auto msgsigned = item->Get<HistoryMessageSigned>()) {
		message.postAuthor = msgsigned->author.toStdString();
	}
	message.replyFlags = 0;
	message.replyMessageId = 0;
	message.replyPeerId = 0;
	message.replyTopId = 0;
	message.replyForumTopic = false;
	// message.replySerialized
	// message.replyMarkupSerialized
	message.entityCreateDate = base::unixtime::now();

	auto serializedText = TfMapper::serializeTextWithEntities(item);
	message.text = serializedText.first;
	message.textEntities = serializedText.second;

	// todo: implement mapping
	message.mediaPath = "/";
	// message.hqThumbPath
	message.documentType = 0; // document type none
	// message.documentSerialized
	// message.thumbsSerialized
	// message.documentAttributesSerialized
	// message.mimeType
}

void addEditedMessage(not_null<HistoryItem *> item) {
	EditedMessage message;
	map(item, message);

	if (message.text.empty()) {
		return;
	}

	TfDatabase::addEditedMessage(message);
}

std::vector<TfMessageBase> getEditedMessages(not_null<HistoryItem*> item, ID minId, ID maxId, int totalLimit) {
	const ID userId = item->history()->owner().session().userId().bare & PeerId::kChatTypeMask;
	const auto dialogId = getDialogIdFromPeer(item->history()->peer);
	const auto msgId = item->id.bare;

	return convertToBase(TfDatabase::getEditedMessages(userId, dialogId, msgId, minId, maxId, totalLimit));
}

bool hasRevisions(not_null<HistoryItem*> item) {
	const ID userId = item->history()->owner().session().userId().bare & PeerId::kChatTypeMask;
	const auto dialogId = getDialogIdFromPeer(item->history()->peer);
	const auto msgId = item->id.bare;

	return TfDatabase::hasRevisions(userId, dialogId, msgId);
}

void snapshotEditsBeforeDelete(not_null<HistoryItem*> item) {
	const auto &settings = TfSettings::getInstance();
	if (!settings.saveMessagesHistory() || item->isLocal()) {
		return;
	}
	const auto &current = item->originalText().text;
	if (current.isEmpty()) {
		return;
	}
	if (!item->Get<HistoryMessageEdited>() && !hasRevisions(item)) {
		return;
	}
	const auto latest = getEditedMessages(item, 0, 0, 1);
	if (!latest.empty() && latest.front().text == current.toStdString()) {
		return;
	}
	addEditedMessage(item);
}

void addDeletedMessage(not_null<HistoryItem*> item) {
	DeletedMessage message;
	map(item, message);

	if (message.text.empty()) {
		const auto revisions = getEditedMessages(item, 0, 0, 128);
		if (revisions.empty()) {
			return;
		}
		const auto &fallback = revisions.front();
		message.text = fallback.text;
		message.textEntities = fallback.textEntities;
		message.editDate = fallback.editDate;
	}

	TfDatabase::addDeletedMessage(message);
}

std::vector<TfMessageBase>
getDeletedMessages(not_null<PeerData*> peer, ID topicId, ID minId, ID maxId, int totalLimit, const QString &searchQuery) {
	const ID userId = peer->session().userId().bare & PeerId::kChatTypeMask;
	return convertToBase(
		TfDatabase::getDeletedMessages(userId, getDialogIdFromPeer(peer), topicId, minId, maxId, totalLimit, searchQuery.toStdString()));
}

bool hasDeletedMessages(not_null<PeerData*> peer, ID topicId) {
	const ID userId = peer->session().userId().bare & PeerId::kChatTypeMask;
	return TfDatabase::hasDeletedMessages(userId, getDialogIdFromPeer(peer), topicId);
}

std::vector<TfMessageBase> buildDeletedMessageTimeline(
		not_null<PeerData*> peer,
		const TfMessageBase &deletedMessage,
		int totalLimit) {
	const ID userId = peer->session().userId().bare & PeerId::kChatTypeMask;
	const auto dialogId = getDialogIdFromPeer(peer);
	auto revisions = TfDatabase::getEditedMessages(
		userId,
		dialogId,
		deletedMessage.messageId,
		0,
		0,
		totalLimit);
	std::reverse(revisions.begin(), revisions.end());

	auto result = std::vector<TfMessageBase>();
	result.reserve(revisions.size() + 2);

	for (auto i = 0; i != revisions.size(); ++i) {
		const auto label = (i == 0)
			? std::string("[Original]")
			: std::string("[Edit ") + std::to_string(i) + "]";
		result.push_back(WithStageLabel(
			static_cast<TfMessageBase>(revisions[i]),
			label));
	}

	const auto lastRevisionLabel = revisions.empty()
		? std::string("[Original]")
		: std::string("[Edit ") + std::to_string(revisions.size()) + "]";
	result.push_back(WithStageLabel(deletedMessage, lastRevisionLabel));

	auto deletedMarker = deletedMessage;
	deletedMarker.text = "[DELETED]";
	deletedMarker.textEntities.clear();
	result.push_back(std::move(deletedMarker));

	return result;
}

}
