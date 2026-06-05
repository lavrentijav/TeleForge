// Copyright @Radolyn, 2026

#include "ayu/features/teleforge/tf_peer_archive.h"

#include "ayu/ayu_settings.h"
#include "ayu/features/teleforge/teleforge_storage.h"
#include "ayu/features/teleforge/teleforge_unified_db.h"
#include "ayu/features/teleforge/tf_peer_archive_entities.h"
#include "ayu/features/teleforge/tf_peer_archive_crawler.h"
#include "ayu/features/teleforge/tf_peer_archive_scanner.h"
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
#include "rpl/rpl.h"
#include "dialogs/dialogs_key.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/history_item_components.h"
#include "main/main_session.h"
#include "window/window_session_controller.h"

#include <QDir>
#include <QFileInfo>

namespace TeleForge::PeerArchive {
namespace {

using sqlite_orm::c;
using sqlite_orm::limit;
using sqlite_orm::order_by;
using sqlite_orm::where;

[[nodiscard]] auto &ArchiveDb() {
	return TeleForge::Storage::Db();
}

auto &ArchiveEnabledCache() {
	static auto enabled = false;
	return enabled;
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

void saveChatUserpicToDisk(not_null<PeerData*> peer) {
	if (peer->isUser()) {
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
	const auto image = PeerData::GenerateUserpicImage(peer, view, 256);
	if (image.isNull()) {
		return;
	}
	image.save(path, "JPG", 90);
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
		long long peerId,
		int now,
		const ObservationSource &source) {
	auto row = ArchiveDb().get_pointer<ProfileRecord>(peerId);
	if (!row) {
		ArchiveDb().replace(ProfileRecord{
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

void appendUsername(long long peerId, const QString &username, int now) {
	const auto trimmed = username.trimmed();
	if (trimmed.isEmpty()) {
		return;
	}
	const auto existing = ArchiveDb().get_all<UsernameRecord>(
		where(c(&UsernameRecord::peerId) == peerId
			&& c(&UsernameRecord::username) == trimmed.toStdString()));
	if (!existing.empty()) {
		return;
	}
	ArchiveDb().insert(UsernameRecord{
		.peerId = peerId,
		.username = trimmed.toStdString(),
		.observedAt = now,
	});
}

void appendBio(long long peerId, const QString &bio, int now) {
	const auto trimmed = bio.trimmed();
	if (trimmed.isEmpty()) {
		return;
	}
	const auto existing = ArchiveDb().get_all<BioRecord>(
		where(c(&BioRecord::peerId) == peerId
			&& c(&BioRecord::bio) == trimmed.toStdString()));
	if (!existing.empty()) {
		return;
	}
	ArchiveDb().insert(BioRecord{
		.peerId = peerId,
		.bio = trimmed.toStdString(),
		.observedAt = now,
	});
}

void appendName(
		long long peerId,
		const QString &first,
		const QString &last,
		int now) {
	const auto f = first.trimmed();
	const auto l = last.trimmed();
	const auto existing = ArchiveDb().get_all<NameRecord>(
		where(c(&NameRecord::peerId) == peerId
			&& c(&NameRecord::firstName) == f.toStdString()
			&& c(&NameRecord::lastName) == l.toStdString()));
	if (!existing.empty()) {
		return;
	}
	ArchiveDb().insert(NameRecord{
		.peerId = peerId,
		.firstName = f.toStdString(),
		.lastName = l.toStdString(),
		.observedAt = now,
	});
}

void noteChat(
		long long peerId,
		long long chatId,
		const QString &title,
		bool selfInChat,
		int now) {
	const auto rows = ArchiveDb().get_all<ChatMembershipRecord>(
		where(c(&ChatMembershipRecord::peerId) == peerId
			&& c(&ChatMembershipRecord::chatId) == chatId));
	if (rows.empty()) {
		ArchiveDb().insert(ChatMembershipRecord{
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

[[nodiscard]] bool HasStoredUserpic(long long peerId, long long photoId) {
	const auto rows = ArchiveDb().get_all<UserpicRecord>(
		where(c(&UserpicRecord::peerId) == peerId
			&& c(&UserpicRecord::photoId) == photoId),
		order_by(&UserpicRecord::observedAt).desc(),
		limit(1));
	return !rows.empty();
}

void insertUserpicRecord(
		long long peerId,
		const QString &path,
		int now,
		long long photoId) {
	ArchiveDb().insert(UserpicRecord{
		.peerId = peerId,
		.localPath = path.toStdString(),
		.observedAt = now,
		.photoId = photoId,
	});
}

void saveUserpicToDisk(
		not_null<UserData*> user,
		int now,
		long long peerId) {
	user->loadUserpic();
	const auto photoId = static_cast<long long>(user->userpicPhotoId());
	if (HasStoredUserpic(peerId, photoId)) {
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
		const auto image = PeerData::GenerateUserpicImage(user, view, 512);
		if (image.isNull()) {
			return false;
		}
		if (!image.save(path, "JPG", 92)) {
			return false;
		}
		insertUserpicRecord(peerId, path, now, photoId);
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

} // namespace

bool archiveEnabled() {
	return ArchiveEnabledCache();
}

void setArchiveEnabled(bool enabled) {
	ArchiveEnabledCache() = enabled;
	AyuSettings::getInstance().setPeerArchiveEnabled(enabled);
}

void attachSession(not_null<Main::Session*> session) {
	ArchiveEnabledCache() = AyuSettings::getInstance().peerArchiveEnabled();

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
			noteUserObserved(user, { .kind = ObservationSource::Kind::Update });
		}
	}, session->lifetime());

	session->data().newItemAdded(
	) | rpl::on_next([=](not_null<HistoryItem*> item) {
		if (!archiveEnabled()) {
			return;
		}
		if (const auto from = item->from() ? item->from()->asUser() : nullptr) {
			if (!from->isSelf()) {
				noteMessageAuthor(from, item->history()->peer, item->id);
			}
		}
		if (const auto forwarded = item->Get<HistoryMessageForwarded>()) {
			if (const auto sender = forwarded->originalSender) {
				if (const auto from = sender->asUser()) {
					if (!from->isSelf()) {
						noteMessageAuthor(
							from,
							item->history()->peer,
							item->id);
					}
				}
			}
		}
	}, session->lifetime());

	bindSessionWindow(session);
	attachCrawler(session);
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
	const auto peerId = StorageId(user->id);
	touchProfile(peerId, now, source);
	appendName(peerId, user->firstName, user->lastName, now);
	const auto &bio = user->about();
	appendBio(peerId, bio, now);
	enqueueLinksFromText(&user->session(), bio);
	for (const auto &name : user->usernames()) {
		appendUsername(peerId, name, now);
	}
	if (!user->username().isEmpty()) {
		appendUsername(peerId, user->username(), now);
	}
	saveUserpicToDisk(user, now, peerId);
	if (const auto channelId = user->personalChannelId()) {
		if (const auto channel = user->session().data().channelLoaded(channelId)) {
			enqueueCrawlPeer(&user->session(), channel);
			noteChatPeerObserved(channel);
		}
	}
	if (source.chatId) {
		if (const auto chatPeer = user->session().data().peer(
				PeerId(source.chatId))) {
			noteChat(
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
	const auto peerId = StorageId(user->id);
	const auto chatId = StorageId(chatPeer->id);
	const auto source = ObservationSource{
		.kind = (messageId
			? ObservationSource::Kind::Message
			: ObservationSource::Kind::Participant),
		.chatId = chatId,
		.messageId = messageId ? int(messageId.bare) : 0,
	};
	touchProfile(peerId, now, source);
	appendName(peerId, user->firstName, user->lastName, now);
	const auto &bio = user->about();
	appendBio(peerId, bio, now);
	enqueueLinksFromText(&user->session(), bio);
	for (const auto &name : user->usernames()) {
		appendUsername(peerId, name, now);
	}
	if (!user->username().isEmpty()) {
		appendUsername(peerId, user->username(), now);
	}
	saveUserpicToDisk(user, now, peerId);
	noteChat(
		peerId,
		chatId,
		chatPeer->name(),
		SelfInChat(chatPeer),
		now);
}

ProfileSnapshot loadProfile(long long peerStorageId) {
	auto result = ProfileSnapshot{ .peerId = peerStorageId };
	if (const auto profile = ArchiveDb().get_pointer<ProfileRecord>(peerStorageId)) {
		result.firstSeenAt = profile->firstSeenAt;
		result.lastSeenAt = profile->lastSeenAt;
		result.firstChatId = profile->firstChatId;
		result.firstMessageId = profile->firstMessageId;
		result.firstSource = QString::fromStdString(profile->firstSource);
	}
	const auto usernames = ArchiveDb().get_all<UsernameRecord>(
		where(c(&UsernameRecord::peerId) == peerStorageId),
		order_by(&UsernameRecord::observedAt).desc());
	for (const auto &row : usernames) {
		result.usernames.push_back({
			.username = QString::fromStdString(row.username),
			.observedAt = row.observedAt,
		});
	}
	const auto names = ArchiveDb().get_all<NameRecord>(
		where(c(&NameRecord::peerId) == peerStorageId),
		order_by(&NameRecord::observedAt).desc());
	for (const auto &row : names) {
		result.names.push_back({
			.firstName = QString::fromStdString(row.firstName),
			.lastName = QString::fromStdString(row.lastName),
			.observedAt = row.observedAt,
		});
	}
	const auto bios = ArchiveDb().get_all<BioRecord>(
		where(c(&BioRecord::peerId) == peerStorageId),
		order_by(&BioRecord::observedAt).desc());
	for (const auto &row : bios) {
		result.bios.push_back({
			.bio = QString::fromStdString(row.bio),
			.observedAt = row.observedAt,
		});
	}
	const auto userpics = ArchiveDb().get_all<UserpicRecord>(
		where(c(&UserpicRecord::peerId) == peerStorageId),
		order_by(&UserpicRecord::observedAt).desc());
	for (const auto &row : userpics) {
		result.userpics.push_back({
			.localPath = QString::fromStdString(row.localPath),
			.observedAt = row.observedAt,
			.photoId = row.photoId,
		});
	}
	const auto chats = ArchiveDb().get_all<ChatMembershipRecord>(
		where(c(&ChatMembershipRecord::peerId) == peerStorageId),
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

	auto currentPhotoId = long long(0);
	if (const auto peer = session->data().peer(
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
	const auto profiles = ArchiveDb().get_all<ProfileRecord>(
		order_by(&ProfileRecord::lastSeenAt).desc());
	auto result = std::vector<ArchivedUserRow>();
	result.reserve(profiles.size());

	for (const auto &profile : profiles) {
		const auto snapshot = loadProfile(profile.peerId);
		if (!needle.isEmpty() && !SearchBlob(snapshot).contains(needle)) {
			continue;
		}
		result.push_back(BuildListRow(snapshot, session));
	}
	return result;
}

void refreshMembershipFromApi(
		not_null<Main::Session*> session,
		not_null<UserData*> user,
		Fn<void()> done) {
	const auto peerId = StorageId(user->id);
	const auto now = base::unixtime::now();
	const auto rows = ArchiveDb().get_all<ChatMembershipRecord>(
		where(c(&ChatMembershipRecord::peerId) == peerId));
	for (auto row : rows) {
		const auto chatPeer = session->data().peer(PeerId(row.chatId));
		if (!chatPeer) {
			continue;
		}
		row.lastSeenAt = now;
		row.selfInChat = SelfInChat(chatPeer);
		row.chatTitle = chatPeer->name().toStdString();
		ArchiveDb().update(row);
		if (archiveEnabled()) {
			scanOpenedChat(session, chatPeer);
		}
	}
	if (done) {
		done();
	}
}

} // namespace TeleForge::PeerArchive
