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

/// Account that owns archive rows: the signed-in account's self UserId.
[[nodiscard]] long long archiveAccountId(not_null<Main::Session*> session);

/// Milliseconds until the archive may issue its next API request; 0 means now.
[[nodiscard]] int archiveApiWaitMs();
/// Takes one request slot from the shared archive budget.
[[nodiscard]] bool archiveTakeApiSlot();
/// Parses FLOOD_WAIT_X / FLOOD_PREMIUM_WAIT_X into seconds; 0 otherwise.
[[nodiscard]] int archiveFloodWaitSeconds(const QString &errorType);
/// Logs an API failure and pauses archive traffic on FLOOD_WAIT.
void archiveNoteApiFailure(const QString &errorType);
void archiveNoteFloodWait(int seconds);

void attachSession(not_null<Main::Session*> session);

void noteUserObserved(
	not_null<UserData*> user,
	ObservationSource source = {});
void noteMessageAuthor(
	not_null<UserData*> user,
	not_null<PeerData*> chatPeer,
	MsgId messageId);

/// Coalescing entry point: the observation lands in the batched queue that is
/// drained in bounded, transactional slices instead of writing to sqlite from
/// the caller's stack. Use this from bulk scans (history sweeps, participant
/// pages) so a large chat cannot stall the UI thread.
void queueMessageAuthor(
	not_null<Main::Session*> session,
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
	Fn<void()> done,
	Fn<void(int processed, int total)> progress = nullptr);

void noteChatPeerObserved(not_null<PeerData*> chatPeer);
[[nodiscard]] QString chatUserpicArchivePath(long long chatStorageId);

} // namespace TeleForge::PeerArchive
