#pragma once

#include "data/data_lastseen_status.h"

#include <optional>
#include <vector>

namespace TeleForge::Spy {

struct OnlineEvent {
	long long userId = 0;
	int timestamp = 0;
	int kind = 0; // 0=offline, 1=online, 2=hidden
	int onlineTill = 0;
	int manualLastSeen = 0;
};

void initializeStorage();
void appendEvents(const std::vector<OnlineEvent> &events);
[[nodiscard]] std::vector<OnlineEvent> loadRecentForUser(long long userId, int sinceTs);
[[nodiscard]] std::optional<int> manualLastSeenForUser(long long userId);
void setSpyTargetEnabled(long long userId, bool enabled);
[[nodiscard]] bool isSpyTargetEnabled(long long userId);
[[nodiscard]] bool spyModeGloballyEnabled();
void setSpyModeGloballyEnabled(bool enabled);
void setRetentionDays(int days);
[[nodiscard]] int retentionDays();
void purgeOldEvents();

} // namespace TeleForge::Spy
