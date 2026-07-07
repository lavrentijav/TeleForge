// Copyright @Radolyn, 2026

#include "ayu/features/teleforge/tf_peer_archive_crawler.h"

#include "ayu/features/teleforge/tf_peer_archive.h"
#include "ayu/features/teleforge/tf_peer_archive_scanner.h"
#include "ayu/ayu_settings.h"
#include "base/call_delayed.h"
#include "base/flat_set.h"
#include "base/random.h"
#include "base/unixtime.h"
#include "data/data_folder.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "dialogs/dialogs_main_list.h"
#include "history/history.h"

#include <QRegularExpression>

namespace TeleForge::PeerArchive {
namespace {

constexpr auto kReseedMs = 1800000;
constexpr auto kMaxQueue = 1500;
constexpr auto kBatchPerTick = 4;

[[nodiscard]] crl::time UserDeltaMs() {
	return crl::time(AyuSettings::getInstance().archiveUserPollDelta()) * 1000;
}

[[nodiscard]] crl::time GroupDeltaMs() {
	return crl::time(AyuSettings::getInstance().archiveGroupPollDelta()) * 1000;
}

[[nodiscard]] crl::time JitterFor(crl::time delta) {
	const auto span = std::max<crl::time>(1, delta / 4);
	return crl::time(base::RandomIndex(int(span)));
}

auto &UserQueue() {
	static auto result = base::flat_set<long long>();
	return result;
}

auto &GroupQueue() {
	static auto result = base::flat_set<long long>();
	return result;
}

auto &UserScheduled() {
	static auto scheduled = false;
	return scheduled;
}

auto &GroupScheduled() {
	static auto scheduled = false;
	return scheduled;
}

void EnqueuePeer(not_null<PeerData*> peer) {
	if (!peer->id) {
		return;
	}
	const auto storageId = static_cast<long long>(SerializePeerId(peer->id));
	auto &queue = peer->isUser() ? UserQueue() : GroupQueue();
	if (queue.size() >= kMaxQueue) {
		return;
	}
	queue.emplace(storageId);
}

void EnqueueUsernameHint(
		not_null<Main::Session*> session,
		const QString &username) {
	const auto clean = username.trimmed();
	if (clean.size() < 4) {
		return;
	}
	if (const auto peer = session->data().peerByUsername(clean)) {
		EnqueuePeer(peer);
	}
}

void EnqueueLinksFromText(
		not_null<Main::Session*> session,
		const QString &text) {
	if (text.isEmpty()) {
		return;
	}
	static const auto atRe = QRegularExpression(
		u"@([A-Za-z][\\w_]{3,31})"_q);
	static const auto tmeRe = QRegularExpression(
		u"(?:https?://)?(?:t\\.me|telegram\\.me)/([A-Za-z][\\w_]{3,31})"_q,
		QRegularExpression::CaseInsensitiveOption);
	auto it = atRe.globalMatch(text);
	while (it.hasNext()) {
		const auto match = it.next();
		EnqueueUsernameHint(session, match.captured(1));
	}
	it = tmeRe.globalMatch(text);
	while (it.hasNext()) {
		const auto match = it.next();
		EnqueueUsernameHint(session, match.captured(1));
	}
}

void EnqueueFromList(not_null<const Dialogs::MainList*> list) {
	if (!list->loaded()) {
		return;
	}
	for (const auto &row : list->indexed()->all()) {
		if (const auto history = row->history()) {
			EnqueuePeer(history->peer);
		}
	}
}

void SeedQueue(not_null<Main::Session*> session) {
	if (!archiveEnabled()) {
		return;
	}
	const auto &data = session->data();
	EnqueueFromList(data.chatsList());
	if (const auto folder = data.folderLoaded(Data::Folder::kId)) {
		EnqueueFromList(data.chatsList(folder));
	}
}

void ProcessPeer(not_null<Main::Session*> session, long long storageId) {
	const auto peer = session->data().peer(PeerId(storageId));
	if (!peer || peer->isSelf()) {
		return;
	}
	if (const auto user = peer->asUser()) {
		if (!user->isServiceUser()) {
			noteUserObserved(user, {
				.kind = ObservationSource::Kind::Profile,
			});
		}
		return;
	}
	scanOpenedChat(session, peer);
}

void ProcessQueue(
		not_null<Main::Session*> session,
		base::flat_set<long long> &queue) {
	if (queue.empty()) {
		SeedQueue(session);
	}
	for (auto processed = 0
		; processed != kBatchPerTick && !queue.empty()
		; ++processed) {
		const auto storageId = *queue.begin();
		queue.erase(queue.begin());
		ProcessPeer(session, storageId);
	}
}

void ScheduleUserTick(not_null<Main::Session*> session, crl::time delay) {
	if (UserScheduled()) {
		return;
	}
	UserScheduled() = true;
	base::call_delayed(delay, session, [=] {
		UserScheduled() = false;
		if (!archiveEnabled()) {
			UserQueue().clear();
			return;
		}
		ProcessQueue(session, UserQueue());
		const auto base = UserDeltaMs();
		ScheduleUserTick(session, base + JitterFor(base));
	});
}

void ScheduleGroupTick(not_null<Main::Session*> session, crl::time delay) {
	if (GroupScheduled()) {
		return;
	}
	GroupScheduled() = true;
	base::call_delayed(delay, session, [=] {
		GroupScheduled() = false;
		if (!archiveEnabled()) {
			GroupQueue().clear();
			return;
		}
		ProcessQueue(session, GroupQueue());
		const auto base = GroupDeltaMs();
		ScheduleGroupTick(session, base + JitterFor(base));
	});
}

void ScheduleReseed(not_null<Main::Session*> session) {
	base::call_delayed(kReseedMs, session, [=] {
		if (!archiveEnabled()) {
			return;
		}
		SeedQueue(session);
		ScheduleReseed(session);
	});
}

} // namespace

void enqueueCrawlPeer(
		not_null<Main::Session*> session,
		not_null<PeerData*> peer) {
	if (!archiveEnabled()) {
		return;
	}
	EnqueuePeer(peer);
}

void enqueueLinksFromText(
		not_null<Main::Session*> session,
		const QString &text) {
	if (!archiveEnabled()) {
		return;
	}
	EnqueueLinksFromText(session, text);
}

void attachCrawler(not_null<Main::Session*> session) {
	SeedQueue(session);
	ScheduleUserTick(session, UserDeltaMs());
	ScheduleGroupTick(session, GroupDeltaMs());
	ScheduleReseed(session);
}

} // namespace TeleForge::PeerArchive
