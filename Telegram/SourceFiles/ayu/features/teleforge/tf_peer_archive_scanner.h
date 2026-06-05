// Copyright @Radolyn, 2026

#pragma once

#include "data/data_peer.h"

#include "main/main_session.h"

namespace TeleForge::PeerArchive {

void scanOpenedChat(
	not_null<Main::Session*> session,
	not_null<PeerData*> chatPeer);

} // namespace TeleForge::PeerArchive
