// Copyright @Radolyn, 2026

#include "tfc/features/teleforge/tf_peer_archive.h"

#include "ayu/ayu_settings.h"
#include "tfc/features/teleforge/teleforge_storage.h"
#include "tfc/features/teleforge/teleforge_unified_db.h"
#include "tfc/features/teleforge/tf_peer_archive_entities.h"
#include "tfc/features/teleforge/tf_peer_archive_crawler.h"
#include "tfc/features/teleforge/tf_peer_archive_scanner.h"
#include "tfc/features/teleforge/tf_peer_archive_scheduler.h"
#include "ayu/libs/sqlite/sqlite_orm.h"
#include "base/call_delayed.h"
#include "base/unixtime.h"
#include "data/data_changes.h"
#include "data/data_channel.h"
#include "data/data_chat.h"
#include "data/data_peer.h"
#include "data/data_peer_id.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "logs.h"
#include "rpl/rpl.h"
#include "dialogs/dialogs_key.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/history_item_components.h"
#include "main/main_session.h"
#include "window/window_session_controller.h"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>

#include <algorithm>
#include <map>
#include <memory>
#include <utility>
#include <vector>

namespace TeleForge::PeerArchive {
namespace {

using sqlite_orm::c;
using sqlite_orm::limit;
using sqlite_orm::order_by;
using sqlite_orm::set;
using sqlite_orm::where;

[[nodiscard]] auto &ArchiveDb() {
	return TeleForge::Storage::Db();
}

auto &ArchiveEnabledCache() {
	static auto enabled = false;
	return enabled;
}

[[nodiscard]] long long AccountId(not_null<Main::Session*> session) {
	return static_cast<long long>(session->user()->id.value);
}

// Read paths without a session (the profile panel) resolve the account here.
[[nodiscard]] long long &CurrentAccountId() {
	static auto value = 0LL;
	return value;
}

constexpr auto kApiMinIntervalMs = crl::time(200);
constexpr auto kApiBurst = 4.;

[[nodiscard]] double &ApiTokens() {
	static auto value = kApiBurst;
	return value;
}

[[nodiscard]] crl::time &ApiTokensUpdatedAt() {
	static auto value = crl::time(0);
	return value;
}

[[nodiscard]] crl::time &ApiBlockedUntil() {
	static auto value = crl::time(0);
	return value;
}

void RefillApiTokens(crl::time now) {
	const auto elapsed = now - ApiTokensUpdatedAt();
	if (elapsed <= 0) {
		return;
	}
	ApiTokens() = std::min(kApiBurst, ApiTokens() + double(elapsed) / kApiMinIntervalMs);
	ApiTokensUpdatedAt() = now;
}

[[nodiscard]] long long StorageId(PeerId id) {
	return static_cast<long long>(SerializePeerId(id));
}

[[nodiscard]] QString UserpicArchiveDir() {
	const auto dir = QFileInfo(TeleForge::Storage::databasePath()).absolutePath()
		+ u"/peer_archive/userpics/"_q;
	QDir().mkpath(dir);
	return dir;
}

[[nodiscard]] QString ChatUserpicArchiveDir() {
	const auto dir = QFileInfo(TeleForge::Storage::databasePath()).absolutePath()
		+ u"/peer_archive/chat_userpics/"_q;
	QDir().mkpath(dir);
	return dir;
}

// JPEG compression of a 256-512px userpic costs several milliseconds each and
// the archive encodes them in bursts, so only the parts that must touch peer
// data and Qt paint state stay on the main thread. The encoded bytes are
// written from a worker, and the resulting DB row is inserted back on main so
// the single sqlite connection is never used from two threads.
void SaveImageAsync(QImage image, QString path, Fn<void(bool)> done) {
	crl::async([image = std::move(image), path = std::move(path), done] {
		const auto ok = !image.isNull() && image.save(path, "JPG", 92);
		if (done) {
			crl::on_main([=] { done(ok); });
		}
	});
}

void saveChatUserpicToDisk(not_null<PeerData*> peer) {
	if (peer->isUser() || !AyuSettings::getInstance().archiveSaveUserpics()) {
		return;
	}
	peer->loadUserpic();
	const auto photoId = static_cast<long long>(peer->userpicPhotoId());
	const auto chatId = StorageId(peer->id);
	const auto path = ChatUserpicArchiveDir()
		+ QString::number(chatId)
		+ u'_'
		+ QString::number(photoId)
		+ u".jpg"_q;
	if (QFile::exists(path)) {
		return;
	}
	auto view = peer->createUserpicView();
	auto image = PeerData::GenerateUserpicImage(peer, view, 256);
	if (image.isNull()) {
		return;
	}
	SaveImageAsync(std::move(image), path, nullptr);
}

[[nodiscard]] bool SelfInChat(not_null<PeerData*> chatPeer) {
	if (const auto chat = chatPeer->asChat()) {
		return chat->amIn();
	} else if (const auto channel = chatPeer->asChannel()) {
		return channel->amIn();
	}
	return false;
}

[[nodiscard]] QString SourceToString(ObservationSource::Kind kind) {
	switch (kind) {
	case ObservationSource::Kind::Message: return u"message"_q;
	case ObservationSource::Kind::Participant: return u"participant"_q;
	case ObservationSource::Kind::Profile: return u"profile"_q;
	case ObservationSource::Kind::Update: return u"update"_q;
	}
	return u"update"_q;
}

void touchProfile(
		long long accountId,
		long long peerId,
		int now,
		const ObservationSource &source) {
	auto row = ArchiveDb().get_pointer<ProfileRecord>(accountId, peerId);
	if (!row) {
		ArchiveDb().replace(ProfileRecord{
			.accountId = accountId,
			.peerId = peerId,
			.firstSeenAt = now,
			.lastSeenAt = now,
			.firstChatId = source.chatId,
			.firstMessageId = source.messageId,
			.firstSource = SourceToString(source.kind).toStdString(),
		});
		return;
	}
	if (!row->firstSeenAt) {
		row->firstSeenAt = now;
	}
	row->lastSeenAt = now;
	if (!row->firstChatId && source.chatId) {
		row->firstChatId = source.chatId;
		row->firstMessageId = source.messageId;
		row->firstSource = SourceToString(source.kind).toStdString();
	}
	ArchiveDb().replace(*row);
}

void appendUsername(
		long long accountId,
		long long peerId,
		const QString &username,
		int now) {
	const auto trimmed = username.trimmed();
	if (trimmed.isEmpty()) {
		return;
	}
	const auto existing = ArchiveDb().get_all<UsernameRecord>(
		where(c(&UsernameRecord::accountId) == accountId
			&& c(&UsernameRecord::peerId) == peerId
			&& c(&UsernameRecord::username) == trimmed.toStdString()));
	if (!existing.empty()) {
		return;
	}
	ArchiveDb().insert(UsernameRecord{
		.accountId = accountId,
		.peerId = peerId,
		.username = trimmed.toStdString(),
		.observedAt = now,
	});
}

void appendBio(
		long long accountId,
		long long peerId,
		const QString &bio,
		int now) {
	const auto trimmed = bio.trimmed();
	if (trimmed.isEmpty()) {
		return;
	}
	const auto existing = ArchiveDb().get_all<BioRecord>(
		where(c(&BioRecord::accountId) == accountId
			&& c(&BioRecord::peerId) == peerId
			&& c(&BioRecord::bio) == trimmed.toStdString()));
	if (!existing.empty()) {
		return;
	}
	ArchiveDb().insert(BioRecord{
		.accountId = accountId,
		.peerId = peerId,
		.bio = trimmed.toStdString(),
		.observedAt = now,
	});
}

void appendName(
		long long accountId,
		long long peerId,
		const QString &first,
		const QString &last,
		int now) {
	const auto f = first.trimmed();
	const auto l = last.trimmed();
	const auto existing = ArchiveDb().get_all<NameRecord>(
		where(c(&NameRecord::accountId) == accountId
			&& c(&NameRecord::peerId) == peerId
			&& c(&NameRecord::firstName) == f.toStdString()
			&& c(&NameRecord::lastName) == l.toStdString()));
	if (!existing.empty()) {
		return;
	}
	ArchiveDb().insert(NameRecord{
		.accountId = accountId,
		.peerId = peerId,
		.firstName = f.toStdString(),
		.lastName = l.toStdString(),
		.observedAt = now,
	});
}

void noteChat(
		long long accountId,
		long long peerId,
		long long chatId,
		const QString &title,
		bool selfInChat,
		int now) {
	const auto rows = ArchiveDb().get_all<ChatMembershipRecord>(
		where(c(&ChatMembershipRecord::accountId) == accountId
			&& c(&ChatMembershipRecord::peerId) == peerId
			&& c(&ChatMembershipRecord::chatId) == chatId));
	if (rows.empty()) {
		ArchiveDb().insert(ChatMembershipRecord{
			.accountId = accountId,
			.peerId = peerId,
			.chatId = chatId,
			.chatTitle = title.toStdString(),
			.firstSeenAt = now,
			.lastSeenAt = now,
			.selfInChat = selfInChat,
			.peerStillInChat = true,
		});
		return;
	}
	auto row = rows.front();
	if (!row.firstSeenAt) {
		row.firstSeenAt = now;
	}
	row.lastSeenAt = now;
	row.chatTitle = title.toStdString();
	row.selfInChat = selfInChat || row.selfInChat;
	row.peerStillInChat = true;
	ArchiveDb().update(row);
}

[[nodiscard]] bool HasStoredUserpic(
		long long accountId,
		long long peerId,
		long long photoId) {
	const auto rows = ArchiveDb().get_all<UserpicRecord>(
		where(c(&UserpicRecord::accountId) == accountId
			&& c(&UserpicRecord::peerId) == peerId
			&& c(&UserpicRecord::photoId) == photoId),
		order_by(&UserpicRecord::observedAt).desc(),
		limit(1));
	return !rows.empty();
}

void insertUserpicRecord(
		long long accountId,
		long long peerId,
		const QString &path,
		int now,
		long long photoId) {
	ArchiveDb().insert(UserpicRecord{
		.accountId = accountId,
		.peerId = peerId,
		.localPath = path.toStdString(),
		.observedAt = now,
		.photoId = photoId,
	});
}

void saveUserpicToDisk(
		not_null<UserData*> user,
		int now,
		long long accountId,
		long long peerId) {
	if (!AyuSettings::getInstance().archiveSaveUserpics()) {
		return;
	}
	user->loadUserpic();
	const auto photoId = static_cast<long long>(user->userpicPhotoId());
	if (HasStoredUserpic(accountId, peerId, photoId)) {
		return;
	}
	const auto path = UserpicArchiveDir()
		+ QString::number(peerId)
		+ u'_'
		+ QString::number(now)
		+ u'_'
		+ QString::number(photoId)
		+ u".jpg"_q;

	const auto persist = [=] {
		auto view = user->createUserpicView();
		auto image = PeerData::GenerateUserpicImage(user, view, 512);
		if (image.isNull()) {
			return false;
		}
		SaveImageAsync(std::move(image), path, [=](bool ok) {
			if (ok && !HasStoredUserpic(accountId, peerId, photoId)) {
				insertUserpicRecord(accountId, peerId, path, now, photoId);
			}
		});
		return true;
	};

	if (persist()) {
		return;
	}

	const auto session = &user->session();
	session->downloaderTaskFinished(
	) | rpl::take(3) | rpl::on_next([=] {
		persist();
	}, session->lifetime());
}

void bindSessionWindow(not_null<Main::Session*> session) {
	const auto attach = [=](not_null<Window::SessionController*> controller) {
		controller->activeChatValue(
		) | rpl::on_next([=](Dialogs::Key key) {
			if (!archiveEnabled()) {
				return;
			}
			if (const auto peer = key.peer()) {
				scanOpenedChat(session, peer);
			}
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

// --- Batched observation processing --------------------------------------
// The dialogs/updates burst right after launch fires peerUpdates and
// newItemAdded hundreds of times. Running the archive writes synchronously
// from each event (every one its own sqlite auto-commit, plus a 512px
// userpic JPEG encode) blocked the UI thread for seconds. We coalesce
// observations into a queue keyed by (user, chat) and drain it in bounded
// slices inside a single transaction, off the synchronous event path.
// Duplicate userpic encodes are skipped by the existing HasStoredUserpic
// guard, so keying by (user, chat) preserves memberships without extra cost.

constexpr auto kObservationFlushDelayMs = crl::time(250);

[[nodiscard]] int MaxObservationsPerFlush() {
	return std::clamp(
		AyuSettings::getInstance().archiveCrawlBatchSize(),
		1,
		200);
}

struct PendingObservation {
	MsgId messageId = 0;
	ObservationSource::Kind kind = ObservationSource::Kind::Update;
};

// (userId.value, chatId.value); chatId.value == 0 for plain profile updates.
using ObservationKey = std::pair<uint64, uint64>;

[[nodiscard]] std::map<ObservationKey, PendingObservation> &PendingObservations() {
	static auto value = std::map<ObservationKey, PendingObservation>();
	return value;
}

[[nodiscard]] bool &ObservationFlushScheduled() {
	static auto value = false;
	return value;
}

void FlushObservations(not_null<Main::Session*> session);

void ScheduleObservationFlush(not_null<Main::Session*> session) {
	if (ObservationFlushScheduled()) {
		return;
	}
	ObservationFlushScheduled() = true;
	base::call_delayed(kObservationFlushDelayMs, session, [=] {
		ObservationFlushScheduled() = false;
		FlushObservations(session);
	});
}

void EnqueueObservation(
		not_null<Main::Session*> session,
		PeerId userId,
		PeerId chatId,
		MsgId messageId,
		ObservationSource::Kind kind) {
	if (!ArchiveEnabledCache() || !userId) {
		return;
	}
	auto &slot = PendingObservations()[ObservationKey(
		userId.value,
		chatId.value)];
	slot.messageId = messageId;
	slot.kind = kind;
	ScheduleObservationFlush(session);
}

void FlushObservations(not_null<Main::Session*> session) {
	auto &pending = PendingObservations();
	if (!ArchiveEnabledCache()) {
		pending.clear();
		return;
	}
	if (pending.empty()) {
		return;
	}
	// Drain a bounded slice so one event-loop turn stays short; the rest is
	// picked up by the next scheduled flush.
	const auto perFlush = MaxObservationsPerFlush();
	auto batch = std::vector<std::pair<ObservationKey, PendingObservation>>();
	batch.reserve(std::min<std::size_t>(pending.size(), perFlush));
	for (auto it = pending.begin();
			it != pending.end() && int(batch.size()) < perFlush;) {
		batch.push_back(*it);
		it = pending.erase(it);
	}
	auto &data = session->data();
	ArchiveDb().transaction([&] {
		for (const auto &[key, obs] : batch) {
			const auto userPeer = data.peerLoaded(PeerId(key.first));
			const auto user = userPeer ? userPeer->asUser() : nullptr;
			if (!user) {
				continue;
			}
			if (key.second) {
				if (const auto chatPeer = data.peerLoaded(PeerId(key.second))) {
					noteMessageAuthor(user, chatPeer, obs.messageId);
					continue;
				}
			}
			noteUserObserved(user, { .kind = obs.kind });
		}
		return true;
	});
	if (!pending.empty()) {
		ScheduleObservationFlush(session);
	}
}

// Rows written before account scoping carry accountId 0. The first account to
// attach after the upgrade claims them, which cannot merge two live accounts.
void AdoptLegacyRows(long long accountId) {
	if (!accountId) {
		return;
	}
	if (!ArchiveDb().count<ProfileRecord>(
			where(c(&ProfileRecord::accountId) == 0))) {
		return;
	}
	ArchiveDb().update_all(
		set(c(&ProfileRecord::accountId) = accountId),
		where(c(&ProfileRecord::accountId) == 0));
	ArchiveDb().update_all(
		set(c(&UsernameRecord::accountId) = accountId),
		where(c(&UsernameRecord::accountId) == 0));
	ArchiveDb().update_all(
		set(c(&NameRecord::accountId) = accountId),
		where(c(&NameRecord::accountId) == 0));
	ArchiveDb().update_all(
		set(c(&BioRecord::accountId) = accountId),
		where(c(&BioRecord::accountId) == 0));
	ArchiveDb().update_all(
		set(c(&UserpicRecord::accountId) = accountId),
		where(c(&UserpicRecord::accountId) == 0));
	ArchiveDb().update_all(
		set(c(&ChatMembershipRecord::accountId) = accountId),
		where(c(&ChatMembershipRecord::accountId) == 0));
}

} // namespace

bool archiveEnabled() {
	return ArchiveEnabledCache();
}

long long archiveAccountId(not_null<Main::Session*> session) {
	return AccountId(session);
}

int archiveApiWaitMs() {
	const auto now = crl::now();
	if (const auto blocked = ApiBlockedUntil() - now; blocked > 0) {
		return int(blocked);
	}
	RefillApiTokens(now);
	if (ApiTokens() >= 1.) {
		return 0;
	}
	return int((1. - ApiTokens()) * kApiMinIntervalMs) + 1;
}

bool archiveTakeApiSlot() {
	const auto now = crl::now();
	if (now < ApiBlockedUntil()) {
		return false;
	}
	RefillApiTokens(now);
	if (ApiTokens() < 1.) {
		return false;
	}
	ApiTokens() -= 1.;
	return true;
}

int archiveFloodWaitSeconds(const QString &errorType) {
	static const auto expression = QRegularExpression(
		u"^FLOOD(_PREMIUM)?_WAIT_(\\d+)$"_q);
	const auto match = expression.match(errorType);
	return match.hasMatch() ? match.captured(2).toInt() : 0;
}

void archiveNoteFloodWait(int seconds) {
	if (seconds <= 0) {
		return;
	}
	const auto until = crl::now()
		+ crl::time(std::min(seconds, 3600)) * 1000
		+ 1000;
	if (until > ApiBlockedUntil()) {
		ApiBlockedUntil() = until;
	}
}

void archiveNoteApiFailure(const QString &errorType) {
	if (const auto flood = archiveFloodWaitSeconds(errorType)) {
		archiveNoteFloodWait(flood);
		LOG(("PeerArchive: flood wait %1 sec, archive requests paused").arg(flood));
		return;
	}
	LOG(("PeerArchive: API request failed: %1").arg(errorType));
}

void setArchiveEnabled(bool enabled) {
	ArchiveEnabledCache() = enabled;
	AyuSettings::getInstance().setPeerArchiveEnabled(enabled);
}

void attachSession(not_null<Main::Session*> session) {
	ArchiveEnabledCache() = AyuSettings::getInstance().peerArchiveEnabled();
	CurrentAccountId() = AccountId(session);
	try {
		AdoptLegacyRows(CurrentAccountId());
	} catch (const std::exception &ex) {
		LOG(("PeerArchive: legacy row adoption failed: %1").arg(ex.what()));
	}

	AyuSettings::getInstance().peerArchiveEnabledChanges(
	) | rpl::on_next([=](bool enabled) {
		ArchiveEnabledCache() = enabled;
	}, session->lifetime());

	session->changes().peerUpdates(
		Data::PeerUpdate::Flag::Username
		| Data::PeerUpdate::Flag::Usernames
		| Data::PeerUpdate::Flag::Name
		| Data::PeerUpdate::Flag::About
		| Data::PeerUpdate::Flag::Photo
	) | rpl::on_next([=](const Data::PeerUpdate &update) {
		if (!archiveEnabled()) {
			return;
		}
		if (const auto user = update.peer->asUser()) {
			EnqueueObservation(
				session,
				user->id,
				PeerId(0),
				MsgId(0),
				ObservationSource::Kind::Update);
		}
	}, session->lifetime());

	session->data().newItemAdded(
	) | rpl::on_next([=](not_null<HistoryItem*> item) {
		if (!archiveEnabled()) {
			return;
		}
		if (const auto from = item->from() ? item->from()->asUser() : nullptr) {
			if (!from->isSelf()) {
				EnqueueObservation(
					session,
					from->id,
					item->history()->peer->id,
					item->id,
					ObservationSource::Kind::Message);
			}
		}
		if (const auto forwarded = item->Get<HistoryMessageForwarded>()) {
			if (const auto sender = forwarded->originalSender) {
				if (const auto from = sender->asUser()) {
					if (!from->isSelf()) {
						EnqueueObservation(
							session,
							from->id,
							item->history()->peer->id,
							item->id,
							ObservationSource::Kind::Message);
					}
				}
			}
		}
	}, session->lifetime());

	bindSessionWindow(session);
	attachCrawler(session);
	attachScheduler(session);
}

void noteChatPeerObserved(not_null<PeerData*> chatPeer) {
	if (!archiveEnabled()) {
		return;
	}
	if (chatPeer->isUser() || chatPeer->isSelf()) {
		return;
	}
	saveChatUserpicToDisk(chatPeer);
	enqueueCrawlPeer(&chatPeer->session(), chatPeer);
}

QString chatUserpicArchivePath(long long chatStorageId) {
	const auto dir = ChatUserpicArchiveDir();
	const auto prefix = QString::number(chatStorageId) + u'_';
	const auto files = QDir(dir).entryList(
		QStringList{ prefix + u'*' },
		QDir::Files,
		QDir::Time);
	if (files.isEmpty()) {
		const auto legacy = dir + QString::number(chatStorageId) + u".jpg"_q;
		return QFile::exists(legacy) ? legacy : QString();
	}
	return dir + files.front();
}

void noteUserObserved(
		not_null<UserData*> user,
		ObservationSource source) {
	if (!archiveEnabled()) {
		return;
	}
	if (user->isSelf() || user->isServiceUser()) {
		return;
	}
	const auto now = base::unixtime::now();
	const auto accountId = AccountId(&user->session());
	const auto peerId = StorageId(user->id);
	touchProfile(accountId, peerId, now, source);
	appendName(accountId, peerId, user->firstName, user->lastName, now);
	const auto &bio = user->about();
	appendBio(accountId, peerId, bio, now);
	enqueueLinksFromText(&user->session(), bio);
	for (const auto &name : user->usernames()) {
		appendUsername(accountId, peerId, name, now);
	}
	if (!user->username().isEmpty()) {
		appendUsername(accountId, peerId, user->username(), now);
	}
	saveUserpicToDisk(user, now, accountId, peerId);
	if (const auto channelId = user->personalChannelId()) {
		if (const auto channel = user->session().data().channelLoaded(channelId)) {
			enqueueCrawlPeer(&user->session(), channel);
			noteChatPeerObserved(channel);
		}
	}
	if (source.chatId) {
		if (const auto chatPeer = user->session().data().peerLoaded(
				DeserializePeerId(static_cast<quint64>(source.chatId)))) {
			noteChat(
				accountId,
				peerId,
				source.chatId,
				chatPeer->name(),
				SelfInChat(chatPeer),
				now);
		}
	}
}

void noteMessageAuthor(
		not_null<UserData*> user,
		not_null<PeerData*> chatPeer,
		MsgId messageId) {
	if (!archiveEnabled()) {
		return;
	}
	if (user->isSelf()) {
		return;
	}
	const auto now = base::unixtime::now();
	const auto accountId = AccountId(&user->session());
	const auto peerId = StorageId(user->id);
	const auto chatId = StorageId(chatPeer->id);
	const auto source = ObservationSource{
		.kind = (messageId
			? ObservationSource::Kind::Message
			: ObservationSource::Kind::Participant),
		.chatId = chatId,
		.messageId = messageId ? int(messageId.bare) : 0,
	};
	touchProfile(accountId, peerId, now, source);
	appendName(accountId, peerId, user->firstName, user->lastName, now);
	const auto &bio = user->about();
	appendBio(accountId, peerId, bio, now);
	enqueueLinksFromText(&user->session(), bio);
	for (const auto &name : user->usernames()) {
		appendUsername(accountId, peerId, name, now);
	}
	if (!user->username().isEmpty()) {
		appendUsername(accountId, peerId, user->username(), now);
	}
	saveUserpicToDisk(user, now, accountId, peerId);
	noteChat(
		accountId,
		peerId,
		chatId,
		chatPeer->name(),
		SelfInChat(chatPeer),
		now);
}

void queueMessageAuthor(
		not_null<Main::Session*> session,
		not_null<UserData*> user,
		not_null<PeerData*> chatPeer,
		MsgId messageId) {
	if (!archiveEnabled() || user->isSelf()) {
		return;
	}
	EnqueueObservation(
		session,
		user->id,
		chatPeer->id,
		messageId,
		messageId
			? ObservationSource::Kind::Message
			: ObservationSource::Kind::Participant);
}

ProfileSnapshot LoadProfileFor(long long accountId, long long peerStorageId) {
	auto result = ProfileSnapshot{ .peerId = peerStorageId };
	if (const auto profile = ArchiveDb().get_pointer<ProfileRecord>(
			accountId,
			peerStorageId)) {
		result.firstSeenAt = profile->firstSeenAt;
		result.lastSeenAt = profile->lastSeenAt;
		result.firstChatId = profile->firstChatId;
		result.firstMessageId = profile->firstMessageId;
		result.firstSource = QString::fromStdString(profile->firstSource);
	}
	const auto usernames = ArchiveDb().get_all<UsernameRecord>(
		where(c(&UsernameRecord::accountId) == accountId
			&& c(&UsernameRecord::peerId) == peerStorageId),
		order_by(&UsernameRecord::observedAt).desc());
	for (const auto &row : usernames) {
		result.usernames.push_back({
			.username = QString::fromStdString(row.username),
			.observedAt = row.observedAt,
		});
	}
	const auto names = ArchiveDb().get_all<NameRecord>(
		where(c(&NameRecord::accountId) == accountId
			&& c(&NameRecord::peerId) == peerStorageId),
		order_by(&NameRecord::observedAt).desc());
	for (const auto &row : names) {
		result.names.push_back({
			.firstName = QString::fromStdString(row.firstName),
			.lastName = QString::fromStdString(row.lastName),
			.observedAt = row.observedAt,
		});
	}
	const auto bios = ArchiveDb().get_all<BioRecord>(
		where(c(&BioRecord::accountId) == accountId
			&& c(&BioRecord::peerId) == peerStorageId),
		order_by(&BioRecord::observedAt).desc());
	for (const auto &row : bios) {
		result.bios.push_back({
			.bio = QString::fromStdString(row.bio),
			.observedAt = row.observedAt,
		});
	}
	const auto userpics = ArchiveDb().get_all<UserpicRecord>(
		where(c(&UserpicRecord::accountId) == accountId
			&& c(&UserpicRecord::peerId) == peerStorageId),
		order_by(&UserpicRecord::observedAt).desc());
	for (const auto &row : userpics) {
		result.userpics.push_back({
			.localPath = QString::fromStdString(row.localPath),
			.observedAt = row.observedAt,
			.photoId = row.photoId,
		});
	}
	const auto chats = ArchiveDb().get_all<ChatMembershipRecord>(
		where(c(&ChatMembershipRecord::accountId) == accountId
			&& c(&ChatMembershipRecord::peerId) == peerStorageId),
		order_by(&ChatMembershipRecord::lastSeenAt).desc());
	for (const auto &row : chats) {
		result.chats.push_back({
			.chatId = row.chatId,
			.chatTitle = QString::fromStdString(row.chatTitle),
			.firstSeenAt = row.firstSeenAt,
			.lastSeenAt = row.lastSeenAt,
			.selfInChat = row.selfInChat,
			.peerStillInChat = row.peerStillInChat,
		});
		if (row.selfInChat) {
			++result.sharedGroupsCount;
		}
	}
	return result;
}

ProfileSnapshot loadProfile(long long peerStorageId) {
	return LoadProfileFor(CurrentAccountId(), peerStorageId);
}

[[nodiscard]] QString LatestDisplayName(const ProfileSnapshot &snapshot) {
	if (!snapshot.names.empty()) {
		const auto &name = snapshot.names.front();
		const auto full = (name.firstName + ' ' + name.lastName).trimmed();
		if (!full.isEmpty()) {
			return full;
		}
	}
	return QString::number(snapshot.peerId);
}

[[nodiscard]] QString LatestSubtitle(const ProfileSnapshot &snapshot) {
	if (!snapshot.usernames.empty()) {
		return u"@"_q + snapshot.usernames.front().username;
	}
	if (!snapshot.bios.empty()) {
		const auto bio = snapshot.bios.front().bio;
		return bio.length() > 64 ? (bio.left(64) + u"…"_q) : bio;
	}
	return QString();
}

[[nodiscard]] QString SearchBlob(const ProfileSnapshot &snapshot) {
	auto parts = QStringList();
	for (const auto &name : snapshot.names) {
		parts.push_back(name.firstName);
		parts.push_back(name.lastName);
	}
	for (const auto &username : snapshot.usernames) {
		parts.push_back(username.username);
		parts.push_back(u"@"_q + username.username);
	}
	for (const auto &bio : snapshot.bios) {
		parts.push_back(bio.bio);
	}
	return parts.join(u' ').toLower();
}

[[nodiscard]] ArchivedUserRow BuildListRow(
		const ProfileSnapshot &snapshot,
		not_null<Main::Session*> session) {
	auto row = ArchivedUserRow{ .peerId = snapshot.peerId };
	row.displayName = LatestDisplayName(snapshot);
	row.subtitle = LatestSubtitle(snapshot);

	auto currentPhotoId = static_cast<long long>(0);
	if (const auto peer = session->data().peerLoaded(
			DeserializePeerId(static_cast<quint64>(snapshot.peerId)))) {
		if (const auto user = peer->asUser()) {
			currentPhotoId = static_cast<long long>(user->userpicPhotoId());
		}
	}

	for (const auto &userpic : snapshot.userpics) {
		const auto exists = QFile::exists(userpic.localPath);
		if (row.userpicPath.isEmpty() && exists) {
			row.userpicPath = userpic.localPath;
		}
		if (!exists || (currentPhotoId && userpic.photoId != currentPhotoId)) {
			++row.deletedUserpics;
		}
	}
	return row;
}

std::vector<ArchivedUserRow> searchArchivedUsers(
		not_null<Main::Session*> session,
		const QString &query) {
	const auto needle = query.trimmed().toLower();
	const auto accountId = AccountId(session);
	const auto profiles = ArchiveDb().get_all<ProfileRecord>(
		where(c(&ProfileRecord::accountId) == accountId),
		order_by(&ProfileRecord::lastSeenAt).desc());
	auto result = std::vector<ArchivedUserRow>();
	result.reserve(profiles.size());

	for (const auto &profile : profiles) {
		const auto snapshot = LoadProfileFor(accountId, profile.peerId);
		if (!needle.isEmpty() && !SearchBlob(snapshot).contains(needle)) {
			continue;
		}
		result.push_back(BuildListRow(snapshot, session));
	}
	return result;
}

namespace {

constexpr auto kMembershipRefreshStepMs = 1;

struct MembershipRefreshState {
	std::vector<ChatMembershipRecord> rows;
	std::size_t index = 0;
	Fn<void()> done;
	Fn<void(int, int)> progress;
};

// One shared chat per event-loop turn: the DB connection is main-thread only,
// so a worker pool is not an option here.
void RefreshMembershipStep(
		not_null<Main::Session*> session,
		std::shared_ptr<MembershipRefreshState> state) {
	while (state->index < state->rows.size()) {
		if (!archiveEnabled()) {
			if (state->done) {
				state->done();
			}
			return;
		}
		auto row = state->rows[state->index++];
		const auto chatPeer = session->data().peerLoaded(
			DeserializePeerId(static_cast<quint64>(row.chatId)));
		if (!chatPeer) {
			continue;
		}
		row.lastSeenAt = base::unixtime::now();
		row.selfInChat = SelfInChat(chatPeer);
		row.chatTitle = chatPeer->name().toStdString();
		ArchiveDb().update(row);
		scanOpenedChat(session, chatPeer);
		if (state->progress) {
			state->progress(int(state->index), int(state->rows.size()));
		}
		base::call_delayed(kMembershipRefreshStepMs, session, [=] {
			RefreshMembershipStep(session, state);
		});
		return;
	}
	if (state->done) {
		state->done();
	}
}

} // namespace

void refreshMembershipFromApi(
		not_null<Main::Session*> session,
		not_null<UserData*> user,
		Fn<void()> done,
		Fn<void(int, int)> progress) {
	auto state = std::make_shared<MembershipRefreshState>();
	state->rows = ArchiveDb().get_all<ChatMembershipRecord>(
		where(c(&ChatMembershipRecord::accountId) == AccountId(session)
			&& c(&ChatMembershipRecord::peerId) == StorageId(user->id)));
	state->done = std::move(done);
	state->progress = std::move(progress);
	if (state->rows.empty()) {
		if (state->done) {
			state->done();
		}
		return;
	}
	RefreshMembershipStep(session, std::move(state));
}

} // namespace TeleForge::PeerArchive
