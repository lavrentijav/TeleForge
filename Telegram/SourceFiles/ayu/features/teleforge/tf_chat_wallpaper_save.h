// Copyright @Radolyn, 2026
#pragma once

class PeerData;

namespace Window {
class SessionController;
} // namespace Window

namespace TeleForge::ChatWallpaper {

[[nodiscard]] bool canSaveForPeer(not_null<PeerData*> peer);

void trySaveOnEmptyClick(
	not_null<Window::SessionController*> controller,
	not_null<PeerData*> peer);

} // namespace TeleForge::ChatWallpaper
