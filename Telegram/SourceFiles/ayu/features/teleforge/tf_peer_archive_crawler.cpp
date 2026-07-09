// Copyright @Radolyn, 2026

#include "ayu/features/teleforge/tf_peer_archive_crawler.h"

#include "ayu/ayu_settings.h"
#include "ayu/features/teleforge/tf_peer_archive.h"
#include "ayu/features/teleforge/tf_peer_archive_scanner.h"
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

#include <algorithm>

namespace TeleForge::PeerArchive {
namespace {

constexpr auto kTickJitterMs = 5000;
constexpr auto kMinTickMs = 10000;

[[nodiscard]] crl::time TickMs() {
	const auto seconds = AyuSettings::getInstance().archiveKnownUserOnlineSeconds();
	return std::max(crl::time(kMinTickMs), crl::time(seconds) * crl::time(1000));
}

[[nodiscard]] crl::time ReseedMs() {
	const auto seconds = AyuSettings::getInstance().archiveChatRefreshSeconds();
	return crl::time(seconds) * crl::time(1000);
}
constexpr auto kMaxQueue = 1500;

auto &CrawlQueue() {
	static auto result = base::flat_set<long long>();
	return result;
}

auto &CrawlScheduled() {
	static auto scheduled = false;
	return scheduled;
}

void ScheduleTick(not_null<Main::Session*> session, crl::time delay);

void EnqueueId(PeerId id) {
	if (!id) {
		return;
	}
	const auto storageId = static_cast<long long>(SerializePeerId(id));
	auto &queue = CrawlQueue();
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
		EnqueueId(peer->id);
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
			EnqueueId(history->peer->id);
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

void ProcessNext(not_null<Main::Session*> session) {
	if (!archiveEnabled()) {
		CrawlQueue().clear();
		return;
	}
	auto &queue = CrawlQueue();
	if (queue.empty()) {
		SeedQueue(session);
	}
	if (queue.empty()) {
		return;
	}
	const auto storageId = *queue.begin();
	queue.erase(queue.begin());

	const auto peer = session->data().peer(
		DeserializePeerId(static_cast<quint64>(storageId)));
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

void ScheduleTick(not_null<Main::Session*> session, crl::time delay) {
	if (CrawlScheduled()) {
		return;
	}
	CrawlScheduled() = true;
	base::call_delayed(delay, session, [=] {
		CrawlScheduled() = false;
		if (!archiveEnabled()) {
			return;
		}
		ProcessNext(session);
		const auto jitter = crl::time(base::RandomIndex(kTickJitterMs));
		ScheduleTick(session, TickMs() + jitter);
	});
}

void ScheduleReseed(not_null<Main::Session*> session) {
	base::call_delayed(ReseedMs(), session, [=] {
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
	EnqueueId(peer->id);
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
	ScheduleTick(session, TickMs());
	ScheduleReseed(session);
}

} // namespace TeleForge::PeerArchive
