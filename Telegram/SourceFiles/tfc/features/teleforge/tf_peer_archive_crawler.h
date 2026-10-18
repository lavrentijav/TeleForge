// Copyright @Radolyn, 2026

#pragma once

#include "data/data_peer.h"
#include "main/main_session.h"

namespace TeleForge::PeerArchive {

void attachCrawler(not_null<Main::Session*> session);
void enqueueCrawlPeer(
	not_null<Main::Session*> session,
	not_null<PeerData*> peer);
void enqueueLinksFromText(
	not_null<Main::Session*> session,
	const QString &text);

} // namespace TeleForge::PeerArchive
