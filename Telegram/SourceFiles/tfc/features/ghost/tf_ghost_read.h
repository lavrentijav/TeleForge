// Copyright @Radolyn, 2026
#pragma once

#include "data/data_msg_id.h"

class History;

namespace Main {
class Session;
} // namespace Main

namespace TeleForge::GhostRead {

void rememberShownTill(
	not_null<History*> history,
	MsgId wasReadTill,
	MsgId tillId);

void applyServerEntry(
	not_null<History*> history,
	int serverUnread,
	MsgId serverMaxInboxRead);

void scheduleFlush(not_null<Main::Session*> session);
void flushIfNotGhost(not_null<Main::Session*> session);

[[nodiscard]] bool hasPending(not_null<Main::Session*> session);

void forgetPeer(not_null<Main::Session*> session, PeerId peerId);
void forgetAccount(not_null<Main::Session*> session);

void noteFloodWait(const QString &errorType);

} // namespace TeleForge::GhostRead
