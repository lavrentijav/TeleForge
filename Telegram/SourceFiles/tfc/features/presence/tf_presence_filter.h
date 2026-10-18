// Copyright @Radolyn, 2026
#pragma once

#include "base/basic_types.h"

namespace Main {
class Session;
} // namespace Main

namespace TeleForge::Presence {

[[nodiscard]] bool maySendStatus(
	not_null<Main::Session*> session,
	bool offline);

void sendStatus(not_null<Main::Session*> session, bool offline);

} // namespace TeleForge::Presence
