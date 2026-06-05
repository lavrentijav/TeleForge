// Copyright @Radolyn, 2026

#pragma once

#include <string>

namespace TeleForge::PeerArchive {

struct ProfileRecord {
	long long peerId = 0;
	int firstSeenAt = 0;
	int lastSeenAt = 0;
	long long firstChatId = 0;
	int firstMessageId = 0;
	std::string firstSource;
};

struct UsernameRecord {
	int id = 0;
	long long peerId = 0;
	std::string username;
	int observedAt = 0;
};

struct NameRecord {
	int id = 0;
	long long peerId = 0;
	std::string firstName;
	std::string lastName;
	int observedAt = 0;
};

struct BioRecord {
	int id = 0;
	long long peerId = 0;
	std::string bio;
	int observedAt = 0;
};

struct UserpicRecord {
	int id = 0;
	long long peerId = 0;
	std::string localPath;
	int observedAt = 0;
	long long photoId = 0;
};

struct ChatMembershipRecord {
	int id = 0;
	long long peerId = 0;
	long long chatId = 0;
	std::string chatTitle;
	int firstSeenAt = 0;
	int lastSeenAt = 0;
	bool selfInChat = false;
	bool peerStillInChat = true;
};

} // namespace TeleForge::PeerArchive
