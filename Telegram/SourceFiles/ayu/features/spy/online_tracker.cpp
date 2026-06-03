#include "ayu/features/spy/online_tracker.h"

#include "ayu/features/spy/online_history_storage.h"
#include "base/call_delayed.h"
#include "base/unixtime.h"

#include <condition_variable>
#include <crl/crl.h>
#include <mutex>

namespace TeleForge::Spy {
namespace {

auto g_pending = std::vector<OnlineEvent>();
auto g_mutex = std::mutex();
auto g_flushScheduled = false;

void FlushPending() {
	std::vector<OnlineEvent> batch;
	{
		const auto lock = std::unique_lock(g_mutex);
		batch = std::move(g_pending);
		g_flushScheduled = false;
	}
	if (!batch.empty()) {
		appendEvents(batch);
	}
}

void ScheduleFlush() {
	const auto lock = std::unique_lock(g_mutex);
	if (g_flushScheduled) {
		return;
	}
	g_flushScheduled = true;
	crl::on_main([] {
		base::call_delayed(10000, [] { FlushPending(); });
	});
}

[[nodiscard]] int KindFromStatus(
		const Data::LastseenStatus &status,
		TimeId now) {
	if (status.isOnline(now)) {
		return 1;
	}
	if (status.isRecently() || status.isWithinWeek() || status.isWithinMonth()) {
		return 2;
	}
	return 0;
}

[[nodiscard]] int ManualLastSeenFromStatus(
		not_null<UserData*> user,
		Data::LastseenStatus status,
		TimeId now) {
	if (status.isOnline(now)) {
		return now;
	}
	if (const auto till = status.onlineTill(); till > 0) {
		return int(till);
	}
	const auto prev = user->lastseen();
	if (prev.isOnline(now)) {
		return now;
	}
	if (const auto existing = manualLastSeenForUser(user->id.value)) {
		return *existing;
	}
	return now;
}

} // namespace

void recordUserStatus(not_null<UserData*> user, Data::LastseenStatus status) {
	if (!isSpyEnabledForUser(user->id.value)) {
		return;
	}
	if (user->isSelf()) {
		return;
	}
	const auto now = base::unixtime::now();
	auto event = OnlineEvent{
		.userId = static_cast<long long>(user->id.value),
		.timestamp = now,
		.kind = KindFromStatus(status, now),
		.onlineTill = int(status.onlineTill()),
		.manualLastSeen = ManualLastSeenFromStatus(user, status, now),
	};
	noteManualLastSeen(event.userId, event.manualLastSeen);
	{
		const auto lock = std::unique_lock(g_mutex);
		g_pending.push_back(event);
	}
	ScheduleFlush();
}

Data::LastseenStatus applyLastseen(
		not_null<UserData*> user,
		Data::LastseenStatus status) {
	recordUserStatus(user, status);
	return effectiveLastseen(user->id.value, status);
}

} // namespace TeleForge::Spy
