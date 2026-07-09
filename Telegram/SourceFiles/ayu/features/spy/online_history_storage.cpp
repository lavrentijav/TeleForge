#include "ayu/features/spy/online_history_storage.h"

#include "ayu/ayu_settings.h"
#include "ayu/features/teleforge/tf_peer_archive_scheduler.h"
#include "ayu/data/ayu_database.h"
#include "base/unixtime.h"

#include <algorithm>
#include <map>
#include <mutex>

namespace TeleForge::Spy {
namespace {

auto g_mutex = std::mutex();
auto g_globalSpy = true;
auto g_retentionDays = 30;
auto g_manualCache = std::map<long long, int>();
auto g_spyApproximate = std::map<long long, bool>();

void NoteManualLastSeen(long long userId, int timestamp) {
	if (timestamp <= 0) {
		return;
	}
	const auto lock = std::unique_lock(g_mutex);
	const auto i = g_manualCache.find(userId);
	if (i != end(g_manualCache) && i->second >= timestamp) {
		return;
	}
	g_manualCache[userId] = timestamp;
}

[[nodiscard]] QString FormatSpyOnlineText(
		TimeId till,
		TimeId now,
		int precisionSeconds) {
	const auto rounded = TeleForge::PeerArchive::roundTimestampToPrecision(
		int(till),
		precisionSeconds);
	const auto onlineFull = base::unixtime::parse(TimeId(rounded));
	const auto nowFull = base::unixtime::parse(now);
	const auto locale = QLocale();
	if (precisionSeconds >= 3600) {
		const auto time = locale.toString(onlineFull.time(), QLocale::ShortFormat);
		if (onlineFull.date() == nowFull.date()) {
			return u"~"_q + time.left(2) + u":00"_q;
		}
		const auto date = locale.toString(onlineFull.date(), QLocale::ShortFormat);
		return u"~%1 %2:00"_q.arg(date, time.left(2));
	}
	if (precisionSeconds >= 60) {
		const auto time = locale.toString(onlineFull.time(), QLocale::ShortFormat);
		if (onlineFull.date() == nowFull.date()) {
			return u"~"_q + time;
		}
		const auto date = locale.toString(onlineFull.date(), QLocale::ShortFormat);
		return u"~%1 %2"_q.arg(date, time);
	}
	const auto &settings = AyuSettings::getInstance();
	const auto timeFmt = settings.showMessageSeconds()
		? QLocale::LongFormat
		: QLocale::ShortFormat;
	const auto time = locale.toString(onlineFull.time(), timeFmt);
	if (onlineFull.date() == nowFull.date()) {
		return u"~"_q + time;
	}
	const auto date = locale.toString(onlineFull.date(), QLocale::ShortFormat);
	return u"~%1 %2"_q.arg(date, time);
}

void SyncFromSettings() {
	const auto &settings = AyuSettings::getInstance();
	const auto lock = std::unique_lock(g_mutex);
	g_globalSpy = settings.spyModeGloballyEnabled();
	g_retentionDays = settings.spyRetentionDays();
}

} // namespace

void initializeStorage() {
	SyncFromSettings();
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
		if (e.manualLastSeen > 0) {
			NoteManualLastSeen(e.userId, e.manualLastSeen);
		}
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
	{
		const auto lock = std::unique_lock(g_mutex);
		const auto i = g_manualCache.find(userId);
		if (i != end(g_manualCache)) {
			return i->second;
		}
	}
	return AyuDatabase::manualLastSeenForUser(userId);
}

void setSpyTargetEnabled(long long userId, bool enabled) {
	AyuDatabase::upsertSpyTarget(userId, enabled);
}

bool hasSpyTargetOverride(long long userId) {
	return AyuDatabase::hasSpyTargetOverride(userId);
}

bool isSpyTargetEnabled(long long userId) {
	return AyuDatabase::isSpyTargetEnabled(userId);
}

bool isSpyEnabledForUser(long long userId) {
	if (AyuDatabase::hasSpyTargetOverride(userId)) {
		return isSpyTargetEnabled(userId);
	}
	return spyModeGloballyEnabled();
}

bool spyModeGloballyEnabled() {
	const auto lock = std::unique_lock(g_mutex);
	return g_globalSpy;
}

void setSpyModeGloballyEnabled(bool enabled) {
	{
		const auto lock = std::unique_lock(g_mutex);
		if (g_globalSpy == enabled) {
			return;
		}
		g_globalSpy = enabled;
	}
	AyuSettings::getInstance().setSpyModeGloballyEnabled(enabled);
}

void setRetentionDays(int days) {
	const auto clamped = std::clamp(days, 1, 365);
	{
		const auto lock = std::unique_lock(g_mutex);
		if (g_retentionDays == clamped) {
			return;
		}
		g_retentionDays = clamped;
	}
	AyuSettings::getInstance().setSpyRetentionDays(clamped);
}

int retentionDays() {
	const auto lock = std::unique_lock(g_mutex);
	return g_retentionDays;
}

void purgeOldEvents() {
	const auto cutoff = base::unixtime::now() - retentionDays() * 86400;
	AyuDatabase::purgeOnlineEventsBefore(cutoff);
}

Data::LastseenStatus effectiveLastseen(
		long long userId,
		Data::LastseenStatus status) {
	if (!isSpyEnabledForUser(userId)) {
		return status;
	}
	const auto now = base::unixtime::now();
	if (status.isOnline(now)) {
		{
			const auto lock = std::unique_lock(g_mutex);
			g_spyApproximate[userId] = false;
		}
		return status;
	}
	if (const auto till = status.onlineTill(); till > 0) {
		{
			const auto lock = std::unique_lock(g_mutex);
			g_spyApproximate[userId] = false;
		}
		return status;
	}
	if (const auto manual = manualLastSeenForUser(userId)) {
		{
			const auto lock = std::unique_lock(g_mutex);
			g_spyApproximate[userId] = true;
		}
		return Data::LastseenStatus::OnlineTill(*manual);
	}
	{
		const auto lock = std::unique_lock(g_mutex);
		g_spyApproximate[userId] = false;
	}
	return status;
}

bool lastseenUsesSpyApproximation(long long userId) {
	const auto lock = std::unique_lock(g_mutex);
	const auto i = g_spyApproximate.find(userId);
	return i != end(g_spyApproximate) && i->second;
}

std::optional<QString> spyOnlineText(not_null<UserData*> user, TimeId now) {
	if (!isSpyEnabledForUser(user->id.value) || user->isSelf()) {
		return std::nullopt;
	}
	if (!lastseenUsesSpyApproximation(user->id.value)) {
		return std::nullopt;
	}
	const auto status = user->lastseen();
	if (status.isOnline(now)) {
		return std::nullopt;
	}
	auto till = status.onlineTill();
	if (till <= 0) {
		if (const auto manual = manualLastSeenForUser(user->id.value)) {
			till = *manual;
		}
	}
	if (till <= 0) {
		return std::nullopt;
	}
	const auto precision = TeleForge::PeerArchive::onlinePollPrecisionSeconds(
		user->id.value);
	return FormatSpyOnlineText(till, now, precision);
}

void noteManualLastSeen(long long userId, int timestamp) {
	NoteManualLastSeen(userId, timestamp);
}

} // namespace TeleForge::Spy
