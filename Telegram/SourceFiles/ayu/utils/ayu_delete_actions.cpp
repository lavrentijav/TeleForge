// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#include "ayu/utils/ayu_delete_actions.h"

#include "api/api_editing.h"
#include "api/api_common.h"
#include "api/api_sending.h"
#include "ayu/ayu_settings.h"
#include "base/unixtime.h"
#include "data/data_histories.h"
#include "data/data_session.h"
#include "data/data_web_page.h"
#include "history/history_item.h"
#include "main/main_session.h"

namespace AyuDelete {
namespace {

[[nodiscard]] bool CanReplaceWithStub(not_null<HistoryItem*> item) {
	if (!item->out() || item->isService() || !item->isRegular()) {
		return false;
	} else if (item->originalText().text.isEmpty()) {
		return false;
	}
	return item->allowsEdit(base::unixtime::now());
}

[[nodiscard]] TextWithEntities StubText() {
	const auto stub = AyuSettings::getInstance().deleteStubText().trimmed();
	return TextWithEntities{
		.text = stub.isEmpty() ? QStringLiteral(".") : stub,
	};
}

void DeleteMessagesNow(
		not_null<Main::Session*> session,
		MessageIdsList ids,
		bool revoke) {
	session->data().histories().deleteMessages(ids, revoke);
	session->data().sendHistoryChangeNotifications();
}

void ReplaceWithStubThenDelete(
		not_null<Main::Session*> session,
		MessageIdsList ids,
		bool revoke,
		std::vector<not_null<HistoryItem*>> toReplace) {
	struct State {
		int left = 0;
		MessageIdsList ids;
		Main::Session *session = nullptr;
		bool revoke = false;
	};
	const auto state = std::make_shared<State>();
	state->left = int(toReplace.size());
	state->ids = std::move(ids);
	state->session = session;
	state->revoke = revoke;

	const auto finishOne = [=] {
		if (--state->left > 0) {
			return;
		}
		DeleteMessagesNow(state->session, state->ids, state->revoke);
	};

	const auto stub = StubText();
	const auto webpage = Data::WebPageDraft{ .removed = true };
	for (const auto item : toReplace) {
		Api::EditTextMessage(
			item,
			stub,
			webpage,
			Api::SendOptions{},
			[=](mtpRequestId) { finishOne(); },
			[=](const QString &, mtpRequestId) { finishOne(); },
			false);
	}
}

} // namespace

bool AnyMessageSupportsStubReplace(
		not_null<Main::Session*> session,
		const MessageIdsList &ids) {
	for (const auto &fullId : ids) {
		if (const auto item = session->data().message(fullId)) {
			if (CanReplaceWithStub(item)) {
				return true;
			}
		}
	}
	return false;
}

void DeleteMessages(
		not_null<Main::Session*> session,
		MessageIdsList ids,
		bool revoke,
		bool replaceWithStub) {
	if (!replaceWithStub) {
		DeleteMessagesNow(session, std::move(ids), revoke);
		return;
	}

	auto toReplace = std::vector<not_null<HistoryItem*>>();
	toReplace.reserve(ids.size());
	for (const auto &fullId : ids) {
		if (const auto item = session->data().message(fullId)) {
			if (CanReplaceWithStub(item)) {
				toReplace.push_back(item);
			}
		}
	}
	if (toReplace.empty()) {
		DeleteMessagesNow(session, std::move(ids), revoke);
		return;
	}
	ReplaceWithStubThenDelete(
		session,
		std::move(ids),
		revoke,
		std::move(toReplace));
}

} // namespace AyuDelete
