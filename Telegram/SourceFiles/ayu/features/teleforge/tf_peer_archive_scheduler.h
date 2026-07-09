// Copyright @Radolyn, 2026

#pragma once

#include "main/main_session.h"

namespace TeleForge::PeerArchive {

void attachScheduler(not_null<Main::Session*> session);

[[nodiscard]] int onlinePollPrecisionSeconds(long long userId);
void setOnlinePollPrecisionSeconds(long long userId, int seconds);

[[nodiscard]] int roundTimestampToPrecision(int timestamp, int precisionSeconds);
[[nodiscard]] QString formatTimestampWithPrecision(
	int timestamp,
	int precisionSeconds);

} // namespace TeleForge::PeerArchive
