// Copyright @Radolyn, 2026

#pragma once

#include "data/data_peer.h"
#include "data/data_peer_id.h"
#include "history/history_item.h"
#include "main/main_session.h"

#include <crl/crl.h>
#include <vector>

namespace TeleForge::PeerArchive {

struct UsernameEntry {
	QString username;
	int observedAt = 0;
};

struct NameEntry {
	QString firstName;
	QString lastName;
	int observedAt = 0;
};

struct BioEntry {
	QString bio;
	int observedAt = 0;
};

struct UserpicEntry {
	QString localPath;
	int observedAt = 0;
	long long photoId = 0;
	bool removed = false;
	bool fileMissing = false;
};

struct ArchivedUserRow {
	long long peerId = 0;
	QString displayName;
	QString subtitle;
	QString userpicPath;
	int deletedUserpics = 0;
};

struct ChatMembershipEntry {
	long long chatId = 0;
	QString chatTitle;
	int firstSeenAt = 0;
	int lastSeenAt = 0;
	bool selfInChat = false;
	bool peerStillInChat = true;
};

struct ProfileSnapshot {
	long long peerId = 0;
	int firstSeenAt = 0;
	int lastSeenAt = 0;
	long long firstChatId = 0;
	int firstMessageId = 0;
	QString firstSource;
	std::vector<UsernameEntry> usernames;
	std::vector<NameEntry> names;
	std::vector<BioEntry> bios;
	std::vector<UserpicEntry> userpics;
	std::vector<ChatMembershipEntry> chats;
	int sharedGroupsCount = 0;
};

struct ObservationSource {
	enum class Kind {
		Update,
		Message,
		Participant,
		Profile,
	};

	Kind kind = Kind::Update;
	long long chatId = 0;
	int messageId = 0;
};

[[nodiscard]] bool archiveEnabled();
void setArchiveEnabled(bool enabled);

void attachSession(not_null<Main::Session*> session);

void noteUserObserved(
	not_null<UserData*> user,
	ObservationSource source = {});
void noteMessageAuthor(
	not_null<UserData*> user,
	not_null<PeerData*> chatPeer,
	MsgId messageId);

[[nodiscard]] ProfileSnapshot loadProfile(long long peerStorageId);
[[nodiscard]] std::vector<ArchivedUserRow> searchArchivedUsers(
	not_null<Main::Session*> session,
	const QString &query);
void refreshMembershipFromApi(
	not_null<Main::Session*> session,
	not_null<UserData*> user,
	Fn<void()> done);

void noteChatPeerObserved(not_null<PeerData*> chatPeer);
[[nodiscard]] QString chatUserpicArchivePath(long long chatStorageId);

} // namespace TeleForge::PeerArchive
