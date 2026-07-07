// Copyright @Radolyn, 2026

#pragma once

#include "base/basic_types.h"

#include <rpl/producer.h>

namespace Main {
class Session;
} // namespace Main

namespace TeleForge::Subscription {

[[nodiscard]] bool active();
[[nodiscard]] rpl::producer<bool> activeValue();

void attachSession(not_null<Main::Session*> session);
void refresh(not_null<Main::Session*> session);

} // namespace TeleForge::Subscription
