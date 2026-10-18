// Copyright @Radolyn, 2026
#include "tfc/features/ghost/tf_ghost_read.h"

#include "tfc/tf_settings.h"
#include "tfc/data/tf_database.h"
#include "tfc/data/entities.h"

#include "base/flat_set.h"
#include "base/timer.h"
#include "base/unixtime.h"
#include "core/application.h"
#include "data/data_histories.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/view/history_view_element.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "apiwrap.h"
#include "main/main_session.h"

#include "logs.h"

#include <algorithm>
#include <map>
#include <set>
#include <utility>

namespace TeleForge::GhostRead {
namespace {

constexpr auto kGhostReadRetentionDays = 90;


constexpr auto kFlagIncoming = 1; // one per real unread message, counted
constexpr auto kFlagRange = 2; // flush target only, never counted
constexpr auto kFlushBatch = 4;
constexpr auto kFlushDelay = crl::time(1000);
constexpr auto kFlushMaxPerRun = 200;
constexpr auto kFloodWaitMargin = crl::time(1000);

struct Target {
	TfId accountId = 0;
	PeerId peerId;
	TfId topicId = 0;
	MsgId till;
};

struct FlushState {
	std::vector<Target> queue;
	std::vector<Target> issued;
	int sent = 0;
	crl::time floodWaitUntil = 0;
};

[[nodiscard]] FlushState &State() {
	static auto value = FlushState();
	return value;
}

[[nodiscard]] base::flat_set<TfId> &AccountsWithRows() {
	static auto value = base::flat_set<TfId>();
	return value;
}

[[nodiscard]] base::flat_set<TfId> &AccountsChecked() {
	static auto value = base::flat_set<TfId>();
	return value;
}

void runBatch();

[[nodiscard]] base::Timer &FlushTimer() {
	static base::Timer timer([] {
		runBatch();
	});
	return timer;
}

[[nodiscard]] TfId AccountOf(not_null<Main::Session*> session) {
	return TfId(session->userId().bare);
}

[[nodiscard]] bool Recording(not_null<Main::Session*> session) {
	const auto &ghost = TfSettings::ghost(session);
	return ghost.isGhostModeActive() && !ghost.sendReadMessages();
}

[[nodiscard]] bool HasPendingRows(TfId accountId) {
	auto &withRows = AccountsWithRows();
	if (withRows.contains(accountId)) {
		return true;
	}
	auto &checked = AccountsChecked();
	if (!checked.emplace(accountId).second) {
		return false;
	}
	if (TfDatabase::hasGhostRead(accountId)) {
		withRows.emplace(accountId);
		return true;
	}
	return false;
}

void RefreshPending(TfId accountId) {
	if (!AccountsWithRows().contains(accountId)) {
		return;
	}
	if (!TfDatabase::hasGhostRead(accountId)) {
		AccountsWithRows().remove(accountId);
	}
}

[[nodiscard]] Main::Session *SessionForAccount(TfId accountId) {
	for (const auto &[index, account] : Core::App().domain().accounts()) {
		if (const auto session = account->maybeSession()) {
			if (AccountOf(session) == accountId) {
				return session;
			}
		}
	}
	return nullptr;
}

struct TillKey {
	TfId peerId = 0;
	TfId topicId = 0;
	auto operator<=>(const TillKey &) const = default;
};

[[nodiscard]] std::map<TillKey, int> MaxTillsForAccount(TfId accountId) {
	auto result = std::map<TillKey, int>();
	for (const auto &row : TfDatabase::loadGhostReadForAccount(accountId)) {
		if (row.msgId <= 0) {
			continue;
		}
		auto &till = result[{ row.peerId, row.topicId }];
		till = std::max(till, row.msgId);
	}
	return result;
}

[[nodiscard]] MsgId ClampTill(not_null<History*> history, MsgId till) {
	if (!history->lastServerMessageKnown()) {
		return 0;
	}
	const auto last = history->lastServerMessage();
	if (!last) {
		return 0;
	}
	return std::min(till, last->id);
}

[[nodiscard]] History *ResolveHistory(
		not_null<Main::Session*> session,
		PeerId peerId) {
	if (const auto loaded = session->data().historyLoaded(peerId)) {
		return loaded;
	}
	const auto peer = session->data().peerLoaded(peerId);
	return peer ? session->data().historyLoaded(peer) : nullptr;
}

void runBatch() {
	auto &state = State();
	const auto now = crl::now();

	if (!state.issued.empty()) {
		if (state.floodWaitUntil > now) {
			// The batch was refused, so its rows stay for a later flush.
			state.issued.clear();
			if (!state.queue.empty()) {
				FlushTimer().callOnce(state.floodWaitUntil - now);
			} else {
				state.sent = 0;
			}
			return;
		}
		for (const auto &target : state.issued) {
			TfDatabase::eraseGhostReadUpTo(
				target.accountId,
				TfId(target.peerId.value),
				int(target.till.bare),
				target.topicId);
			RefreshPending(target.accountId);
		}
		state.issued.clear();
	}
	if (state.queue.empty()) {
		state.sent = 0;
		return;
	}
	if (state.floodWaitUntil > now) {
		FlushTimer().callOnce(state.floodWaitUntil - now);
		return;
	}
	if (state.sent >= kFlushMaxPerRun) {
		state.queue.clear();
		state.sent = 0;
		return;
	}

	auto sent = 0;
	auto stopped = false;
	while (!state.queue.empty() && sent < kFlushBatch) {
		const auto target = state.queue.back();
		state.queue.pop_back();

		const auto session = SessionForAccount(target.accountId);
		if (!session) {
			continue;
		}
		const auto &ghost = TfSettings::ghost(session);
		if (ghost.isGhostModeActive() || !ghost.sendReadMessages()) {
			state.queue.push_back(target);
			stopped = true;
			break;
		}
		const auto history = ResolveHistory(session, target.peerId);
		if (!history) {
			continue;
		}
		const auto till = ClampTill(history, target.till);
		if (!till) {
			continue;
		}
		LOG(("GhostRead: flushing peer %1 till %2"
			).arg(target.peerId.value
			).arg(till.bare));
		if (target.topicId) {
			session->api().request(MTPmessages_ReadDiscussion(
				history->peer->input(),
				MTP_int(target.topicId),
				MTP_int(till.bare)
			)).done([=](const MTPBool &) {
			}).send();
		} else {
			session->data().histories().readInboxTillForced(history, till);
		}
		state.issued.push_back({
			.accountId = target.accountId,
			.peerId = target.peerId,
			.till = till,
		});
		++sent;
	}
	state.sent += sent;
	if (!state.issued.empty()
		|| (!stopped && sent > 0 && !state.queue.empty())) {
		FlushTimer().callOnce(kFlushDelay);
	}
}

} // namespace

void rememberShownTill(
		not_null<History*> history,
		MsgId wasReadTill,
		MsgId tillId) {
	if (tillId <= wasReadTill) {
		return;
	}
	const auto session = &history->session();
	if (!Recording(session)) {
		return;
	}
	const auto accountId = AccountOf(session);
	const auto peerId = TfId(history->peer->id.value);
	const auto now = base::unixtime::now();

	auto ids = std::set<int>();
	auto rows = std::vector<GhostReadMessage>();
	for (const auto &block : history->blocks) {
		for (const auto &message : block->messages) {
			const auto item = message->data();
			if (!item->isRegular()
				|| (item->out() && !item->isFromScheduled())) {
				continue;
			} else if (item->id <= wasReadTill || item->id > tillId) {
				continue;
			}
			const auto id = int(item->id.bare);
			if (!ids.emplace(id).second) {
				continue;
			}
			rows.push_back({
				.accountId = accountId,
				.peerId = peerId,
				.topicId = TfId(item->topicRootId().bare),
				.msgId = id,
				.readAt = now,
				.flags = kFlagIncoming,
			});
		}
	}
	if (!ids.contains(int(tillId.bare))) {
		rows.push_back({
			.accountId = accountId,
			.peerId = peerId,
			.msgId = int(tillId.bare),
			.readAt = now,
			.flags = kFlagRange,
		});
	}
	static auto purgedAccounts = std::set<TfId>();
	if (purgedAccounts.emplace(accountId).second) {
		TfDatabase::purgeGhostReadBefore(now - kGhostReadRetentionDays * 86400);
	}
	TfDatabase::insertGhostReadBatch(rows);
	AccountsWithRows().emplace(accountId);
}

void applyServerEntry(
		not_null<History*> history,
		int serverUnread,
		MsgId serverMaxInboxRead) {
	const auto session = &history->session();
	const auto &ghost = TfSettings::ghost(session);
	if (!ghost.isGhostModeActive()) {
		return;
	}
	const auto accountId = AccountOf(session);
	if (!HasPendingRows(accountId)) {
		return;
	}
	const auto peerId = TfId(history->peer->id.value);
	auto till = MsgId(0);
	auto consumed = 0;
	for (const auto &row : TfDatabase::loadGhostReadForPeer(accountId, peerId)) {
		if (row.msgId <= serverMaxInboxRead.bare) {
			continue;
		}
		till = std::max(till, MsgId(row.msgId));
		if (row.flags & kFlagIncoming) {
			++consumed;
		}
	}
	if (!till) {
		return;
	}
	history->setInboxReadTill(till);
	history->setUnreadCount(std::max(0, serverUnread - consumed));
	history->updateChatListEntry();
}

void scheduleFlush(not_null<Main::Session*> session) {
	const auto &ghost = TfSettings::ghost(session);
	if (ghost.isGhostModeActive() || !ghost.sendReadMessages()) {
		return;
	}
	const auto accountId = AccountOf(session);
	auto tills = MaxTillsForAccount(accountId);
	if (tills.empty()) {
		return;
	}
	auto &state = State();
	state.queue.clear();
	for (const auto &[key, till] : tills) {
		state.queue.push_back({
			.accountId = accountId,
			.peerId = PeerId(key.peerId),
			.topicId = key.topicId,
			.till = MsgId(till),
		});
	}
	state.sent = 0;
	LOG(("GhostRead: scheduling flush of %1 chats"
		).arg(int(state.queue.size())));
	runBatch();
}

void flushIfNotGhost(not_null<Main::Session*> session) {
	scheduleFlush(session);
}

bool hasPending(not_null<Main::Session*> session) {
	return HasPendingRows(AccountOf(session));
}

void forgetPeer(not_null<Main::Session*> session, PeerId peerId) {
	const auto accountId = AccountOf(session);
	TfDatabase::eraseGhostReadForPeer(accountId, TfId(peerId.value));
	RefreshPending(accountId);
}

void forgetAccount(not_null<Main::Session*> session) {
	const auto accountId = AccountOf(session);
	TfDatabase::eraseGhostReadForAccount(accountId);
	AccountsWithRows().remove(accountId);
	AccountsChecked().emplace(accountId);
	auto &state = State();
	state.queue.erase(
		ranges::remove_if(state.queue, [&](const Target &target) {
			return target.accountId == accountId;
		}),
		end(state.queue));
	state.issued.erase(
		ranges::remove_if(state.issued, [&](const Target &target) {
			return target.accountId == accountId;
		}),
		end(state.issued));
}

void noteFloodWait(const QString &errorType) {
	if (!errorType.startsWith(u"FLOOD_"_q)) {
		return;
	}
	auto ok = false;
	const auto seconds = errorType.mid(errorType.lastIndexOf(u'_') + 1).toInt(&ok);
	if (!ok || seconds <= 0) {
		return;
	}
	const auto wait = crl::time(seconds) * 1000 + kFloodWaitMargin;
	State().floodWaitUntil = std::max(State().floodWaitUntil, crl::now() + wait);
	LOG(("GhostRead: FLOOD_WAIT, flush delayed by %1s").arg(seconds));
}

} // namespace TeleForge::GhostRead
