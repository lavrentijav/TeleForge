#include "ayu/features/spy/online_history_storage.h"

#include "ayu/data/ayu_database.h"
#include "base/unixtime.h"

#include <mutex>

namespace TeleForge::Spy {
namespace {

auto g_mutex = std::mutex();
auto g_globalSpy = true;
auto g_retentionDays = 30;

} // namespace

void initializeStorage() {
	purgeOldEvents();
}

void appendEvents(const std::vector<OnlineEvent> &events) {
	for (const auto &e : events) {
		::OnlineEvent row;
		row.userId = e.userId;
		row.timestamp = e.timestamp;
		row.kind = e.kind;
		row.onlineTill = e.onlineTill;
		row.manualLastSeen = e.manualLastSeen;
		AyuDatabase::insertOnlineEvent(row);
	}
}

std::vector<OnlineEvent> loadRecentForUser(long long userId, int sinceTs) {
	auto out = std::vector<OnlineEvent>();
	for (const auto &row : AyuDatabase::loadOnlineEventsForUser(userId, sinceTs)) {
		out.push_back({
			.userId = row.userId,
			.timestamp = row.timestamp,
			.kind = row.kind,
			.onlineTill = row.onlineTill,
			.manualLastSeen = row.manualLastSeen,
		});
	}
	return out;
}

std::optional<int> manualLastSeenForUser(long long userId) {
	return AyuDatabase::manualLastSeenForUser(userId);
}

void setSpyTargetEnabled(long long userId, bool enabled) {
	AyuDatabase::upsertSpyTarget(userId, enabled);
}

bool isSpyTargetEnabled(long long userId) {
	return AyuDatabase::isSpyTargetEnabled(userId);
}

bool spyModeGloballyEnabled() {
	const auto lock = std::unique_lock(g_mutex);
	return g_globalSpy;
}

void setSpyModeGloballyEnabled(bool enabled) {
	const auto lock = std::unique_lock(g_mutex);
	g_globalSpy = enabled;
}

void setRetentionDays(int days) {
	const auto lock = std::unique_lock(g_mutex);
	g_retentionDays = std::clamp(days, 1, 365);
}

int retentionDays() {
	const auto lock = std::unique_lock(g_mutex);
	return g_retentionDays;
}

void purgeOldEvents() {
	const auto cutoff = base::unixtime::now() - retentionDays() * 86400;
	AyuDatabase::purgeOnlineEventsBefore(cutoff);
}

} // namespace TeleForge::Spy
