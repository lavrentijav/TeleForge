// Copyright @Radolyn, 2026

#pragma once

#include "ayu/features/teleforge/tf_peer_archive.h"

namespace Window {
class SessionController;
} // namespace Window

namespace TeleForge::PeerArchive {

void ShowArchiveUserpicsBox(
	not_null<Window::SessionController*> controller,
	const std::vector<UserpicEntry> &userpics);

} // namespace TeleForge::PeerArchive
