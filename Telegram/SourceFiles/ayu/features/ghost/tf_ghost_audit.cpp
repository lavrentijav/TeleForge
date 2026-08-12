// Copyright @Radolyn, 2026

#include "ayu/features/ghost/tf_ghost_audit.h"

#include "ayu/ayu_settings.h"
#include "base/unixtime.h"

#include "logs.h"

#include <map>

#include <QtCore/QDateTime>

namespace TeleForge::Ghost {
namespace {

constexpr auto kKeepRecent = 200;

[[nodiscard]] QStringList &Recent() {
	static auto value = QStringList();
	return value;
}

[[nodiscard]] std::map<QString, int> &Counters() {
	static auto value = std::map<QString, int>();
	return value;
}

} // namespace

void noteWakeSignal(
		Main::Session *session,
		const QString &kind,
		const QString &detail) {
	if (!session) {
		return;
	}
	const auto &ghost = AyuSettings::ghost(session);
	if (!ghost.isGhostModeActive()) {
		return;
	}
	++Counters()[kind];
	auto line = QDateTime::currentDateTime().toString(u"HH:mm:ss"_q)
		+ u" · "_q
		+ kind;
	if (!detail.isEmpty()) {
		line += u" · "_q + detail;
	}
	auto &recent = Recent();
	recent.push_back(line);
	while (recent.size() > kKeepRecent) {
		recent.removeFirst();
	}
	LOG(("[Ghost] wake signal: %1").arg(line));
}

QStringList recentWakeSignals() {
	return Recent();
}

int wakeSignalCount(const QString &kind) {
	const auto i = Counters().find(kind);
	return (i != Counters().end()) ? i->second : 0;
}

void clearWakeSignals() {
	Recent().clear();
	Counters().clear();
}

} // namespace TeleForge::Ghost
