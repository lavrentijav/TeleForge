// Copyright @Radolyn, 2026

#pragma once

#include "ayu/features/teleforge/tf_peer_archive.h"

namespace Window {
class SessionController;
} // namespace Window

namespace TeleForge::PeerArchive {

void ShowArchiveUsersBox(not_null<Window::SessionController*> controller);

} // namespace TeleForge::PeerArchive
