// Copyright @Radolyn, 2026

#include "ayu/features/teleforge/tf_peer_archive_scheduler.h"

#include "apiwrap.h"
#include "ayu/ayu_settings.h"
#include "ayu/features/spy/online_history_storage.h"
#include "ayu/features/teleforge/teleforge_unified_db.h"
#include "ayu/features/teleforge/tf_peer_archive.h"
#include "ayu/features/teleforge/tf_peer_archive_entities.h"
#include "ayu/features/teleforge/tf_peer_archive_scanner.h"
#include "base/call_delayed.h"
#include "base/flat_map.h"
#include "base/flat_set.h"
#include "base/unixtime.h"
#include "data/data_folder.h"
#include "data/data_peer_id.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "dialogs/dialogs_main_list.h"
#include "history/history.h"

#include <algorithm>

namespace TeleForge::PeerArchive {
namespace {

constexpr auto kSchedulerTickMs = 5000;

enum class PollKind {
	Chat,
	Online,
	Profile,
};

struct DueItem {
	PollKind kind = PollKind::Chat;
	long long id = 0;
	int overdue = 0;
};

auto &LastOnlinePoll() {
	static auto result = base::flat_map<long long, int>();
	return result;
}

auto &LastProfilePoll() {
	static auto result = base::flat_map<long long, int>();
	return result;
}

auto &LastChatPoll() {
	static auto result = base::flat_map<long long, int>();
	return result;
}

auto &OnlinePrecision() {
	static auto result = base::flat_map<long long, int>();
	return result;
}

auto &Scheduled() {
	static auto scheduled = false;
	return scheduled;
}

auto &SeenPrivateUsers() {
	static auto result = base::flat_set<long long>();
	return result;
}

[[nodiscard]] bool ShouldRun() {
	return archiveEnabled() || TeleForge::Spy::spyModeGloballyEnabled();
}

[[nodiscard]] bool HasPrivateChat(
		not_null<Main::Session*> session,
		not_null<UserData*> user) {
	return session->data().historyLoaded(user) != nullptr;
}

void CollectChats(
		not_null<Main::Session*> session,
		not_null<const Dialogs::MainList*> list,
		int chatInterval,
		int now,
		std::vector<DueItem> &candidates) {
	if (!list->loaded()) {
		return;
	}
	for (const auto &row : list->indexed()->all()) {
		const auto history = row->history();
		if (!history) {
			continue;
		}
		const auto peer = history->peer;
		if (peer->isUser() || peer->isSelf()) {
			continue;
		}
		const auto id = static_cast<long long>(SerializePeerId(peer->id));
		const auto lastIt = LastChatPoll().find(id);
		const auto last = (lastIt != end(LastChatPoll())) ? lastIt->second : 0;
		candidates.push_back({
			.kind = PollKind::Chat,
			.id = id,
			.overdue = now - last - chatInterval,
		});
	}
}

void CollectPrivateUsers(
		not_null<Main::Session*> session,
		not_null<const Dialogs::MainList*> list) {
	if (!list->loaded()) {
		return;
	}
	for (const auto &row : list->indexed()->all()) {
		const auto history = row->history();
		if (!history) {
			continue;
		}
		const auto user = history->peer->asUser();
		if (!user || user->isSelf() || user->isServiceUser()) {
			continue;
		}
		SeenPrivateUsers().emplace(
			static_cast<long long>(user->id.value));
	}
}

void CollectUsers(
		not_null<Main::Session*> session,
		int knownOnlineInterval,
		int privateOnlineInterval,
		int privateProfileInterval,
		int otherProfileInterval,
		int now,
		std::vector<DueItem> &candidates) {
	const auto profiles = TeleForge::Storage::Db().get_all<ProfileRecord>();
	for (const auto &profile : profiles) {
		const auto peer = session->data().peer(
			DeserializePeerId(static_cast<quint64>(profile.peerId)));
		const auto user = peer ? peer->asUser() : nullptr;
		if (!user || user->isSelf() || user->isServiceUser()) {
			continue;
		}
		const auto uid = static_cast<long long>(user->id.value);
		const auto isPrivate = HasPrivateChat(session, user)
			|| SeenPrivateUsers().contains(uid);
		const auto profileInterval = isPrivate
			? privateProfileInterval
			: otherProfileInterval;

		const auto profileIt = LastProfilePoll().find(uid);
		const auto lastProfile = (profileIt != end(LastProfilePoll()))
			? profileIt->second
			: 0;
		candidates.push_back({
			.kind = PollKind::Profile,
			.id = uid,
			.overdue = now - lastProfile - profileInterval,
		});
	}

	session->data().enumerateUsers([&](not_null<UserData*> user) {
		if (user->isSelf() || user->isServiceUser()) {
			return;
		}
		const auto uid = static_cast<long long>(user->id.value);
		if (!TeleForge::Spy::isSpyEnabledForUser(uid)) {
			return;
		}
		const auto isPrivate = HasPrivateChat(session, user)
			|| SeenPrivateUsers().contains(uid);
		const auto onlineInterval = isPrivate
			? privateOnlineInterval
			: knownOnlineInterval;
		const auto onlineIt = LastOnlinePoll().find(uid);
		const auto lastOnline = (onlineIt != end(LastOnlinePoll()))
			? onlineIt->second
			: 0;
		candidates.push_back({
			.kind = PollKind::Online,
			.id = uid,
			.overdue = now - lastOnline - onlineInterval,
		});
	});
}

void PollUserOnline(
		not_null<Main::Session*> session,
		not_null<UserData*> user,
		int precisionSeconds) {
	const auto weak = base::make_weak(session);
	session->api().request(MTPusers_GetUsers(
		MTP_vector<MTPInputUser>(1, user->inputUser())
	)).done([=](const MTPVector<MTPUser> &result) {
		if (!weak) {
			return;
		}
		weak->data().processUsers(result);
	}).send();
	LastOnlinePoll()[user->id.value] = base::unixtime::now();
	setOnlinePollPrecisionSeconds(user->id.value, precisionSeconds);
}

void PollUserProfile(not_null<Main::Session*> session, not_null<UserData*> user) {
	session->api().requestFullPeer(user);
	LastProfilePoll()[user->id.value] = base::unixtime::now();
}

void PollChat(not_null<Main::Session*> session, not_null<PeerData*> peer) {
	scanOpenedChat(session, peer);
	LastChatPoll()[static_cast<long long>(SerializePeerId(peer->id))]
		= base::unixtime::now();
}

void ProcessTick(not_null<Main::Session*> session) {
	if (!ShouldRun()) {
		return;
	}
	const auto &settings = AyuSettings::getInstance();
	const auto now = base::unixtime::now();
	const auto chatInterval = settings.archiveChatRefreshSeconds();
	const auto knownOnline = settings.archiveKnownUserOnlineSeconds();
	const auto privateOnline = settings.archivePrivateOnlineSeconds();
	const auto privateProfile = settings.archivePrivateProfileSeconds();
	const auto otherProfile = settings.archiveOtherProfileSeconds();

	SeenPrivateUsers().clear();
	CollectPrivateUsers(session, session->data().chatsList());
	if (const auto folder = session->data().folderLoaded(Data::Folder::kId)) {
		CollectPrivateUsers(session, session->data().chatsList(folder));
	}

	auto candidates = std::vector<DueItem>();
	CollectChats(
		session,
		session->data().chatsList(),
		chatInterval,
		now,
		candidates);
	if (const auto folder = session->data().folderLoaded(Data::Folder::kId)) {
		CollectChats(
			session,
			session->data().chatsList(folder),
			chatInterval,
			now,
			candidates);
	}
	CollectUsers(
		session,
		knownOnline,
		privateOnline,
		privateProfile,
		otherProfile,
		now,
		candidates);

	if (candidates.empty()) {
		return;
	}
	const auto best = ranges::max_element(
		candidates,
		std::less<>(),
		&DueItem::overdue);
	if (best == end(candidates) || best->overdue < 0) {
		return;
	}

	switch (best->kind) {
	case PollKind::Chat: {
		const auto peer = session->data().peer(
			DeserializePeerId(static_cast<quint64>(best->id)));
		if (peer) {
			PollChat(session, peer);
		}
		break;
	}
	case PollKind::Online: {
		const auto user = session->data().userLoaded(UserId(best->id));
		if (!user) {
			break;
		}
		const auto isPrivate = HasPrivateChat(session, user)
			|| SeenPrivateUsers().contains(best->id);
		PollUserOnline(
			session,
			user,
			isPrivate ? privateOnline : knownOnline);
		break;
	}
	case PollKind::Profile: {
		const auto user = session->data().userLoaded(UserId(best->id));
		if (!user) {
			break;
		}
		PollUserProfile(session, user);
		break;
	}
	}
}

void ScheduleTick(not_null<Main::Session*> session) {
	if (Scheduled()) {
		return;
	}
	Scheduled() = true;
	base::call_delayed(kSchedulerTickMs, session, [=] {
		Scheduled() = false;
		ProcessTick(session);
		ScheduleTick(session);
	});
}

} // namespace

void attachScheduler(not_null<Main::Session*> session) {
	ScheduleTick(session);
}

int onlinePollPrecisionSeconds(long long userId) {
	const auto i = OnlinePrecision().find(userId);
	return (i != end(OnlinePrecision())) ? i->second : 60;
}

void setOnlinePollPrecisionSeconds(long long userId, int seconds) {
	OnlinePrecision()[userId] = seconds;
}

int roundTimestampToPrecision(int timestamp, int precisionSeconds) {
	if (!timestamp || precisionSeconds <= 0) {
		return timestamp;
	}
	return (timestamp / precisionSeconds) * precisionSeconds;
}

QString formatTimestampWithPrecision(int timestamp, int precisionSeconds) {
	if (!timestamp) {
		return u"—"_q;
	}
	const auto rounded = roundTimestampToPrecision(timestamp, precisionSeconds);
	const auto dt = QDateTime::fromSecsSinceEpoch(rounded);
	if (precisionSeconds >= 3600) {
		return dt.toString(u"dd.MM.yyyy HH:00"_q);
	}
	if (precisionSeconds >= 60) {
		return dt.toString(u"dd.MM.yyyy HH:mm"_q);
	}
	return dt.toString(u"dd.MM.yyyy HH:mm:ss"_q);
}

} // namespace TeleForge::PeerArchive
