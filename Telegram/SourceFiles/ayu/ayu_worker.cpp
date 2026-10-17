// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#include "ayu/ayu_worker.h"

#include "apiwrap.h"
#include "ayu_settings.h"
#include "ayu/features/presence/tf_presence_filter.h"
#include "base/timer.h"
#include "base/unixtime.h"
#include "core/application.h"
#include "data/data_user.h"
#include "data/entities.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"

namespace AyuWorker {
namespace {

constexpr auto kOfflinePacketDelay = crl::time(3000);

std::unordered_map<ID, bool> scheduled;

void runOnce();

base::Timer &workerTimer() {
	static base::Timer timer([] {
		runOnce();
	});
	return timer;
}

[[nodiscard]] bool hasScheduled() {
	for (const auto &entry : scheduled) {
		if (entry.second) {
			return true;
		}
	}
	return false;
}

void armTimer() {
	if (!hasScheduled()) {
		return;
	}
	workerTimer().cancel();
	workerTimer().callOnce(kOfflinePacketDelay);
}

void lateInit() {
	for (const auto &[index, account] : Core::App().domain().accounts()) {
		if (const auto session = account->maybeSession()) {
			scheduled[session->userId().bare] = true;
		}
	}
}

void runOnce() {
	if (!Core::IsAppLaunched() || !Core::App().domain().started() || Core::Quitting()) {
		return;
	}

	if (scheduled.empty()) {
		lateInit();
	}

	const auto t = base::unixtime::now();

	for (const auto &[index, account] : Core::App().domain().accounts()) {
		if (account) {
			if (const auto session = account->maybeSession()) {
				const auto id = session->userId().bare;
				if (!scheduled.contains(id)) {
					scheduled[id] = true;
				}
				if (!scheduled[id] && !session->user()->lastseen().isOnline(t)) {
					continue;
				}

				const auto &ghost = AyuSettings::ghost(session);
				scheduled[id] = false;
				if (!ghost.sendOfflinePacketAfterOnline()) {
					continue;
				}

				TeleForge::Presence::sendStatus(session, true);

				DEBUG_LOG(("[AyuGram] Sent offline for account with id %1").arg(id));
			}
		}
	}
}

} // namespace

void scheduleOfflinePacket(not_null<Main::Session*> session) {
	scheduled[session->userId().bare] = true;
	armTimer();
}

void initialize() {
	lateInit();
	armTimer();
}

}
