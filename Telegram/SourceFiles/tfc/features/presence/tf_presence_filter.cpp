// Copyright @Radolyn, 2026
#include "tfc/features/presence/tf_presence_filter.h"

#include "apiwrap.h"
#include "ayu/ayu_settings.h"
#include "tfc/features/ghost/tf_ghost_audit.h"
#include "main/main_session.h"

namespace TeleForge::Presence {

bool maySendStatus(
		not_null<Main::Session*> session,
		bool offline) {
	if (offline) {
		// WHY: an offline packet reveals nothing, but withholding it keeps the
		// account "online" on the server forever once an online packet was sent.
		return true;
	}
	return AyuSettings::ghost(session).sendOnlinePackets();
}

void sendStatus(not_null<Main::Session*> session, bool offline) {
	if (!maySendStatus(session, offline)) {
		Ghost::noteWakeSignal(
			session,
			u"account.updateStatus"_q,
			u"suppressed online"_q);
		return;
	}
	Ghost::noteWakeSignal(
		session,
		u"account.updateStatus"_q,
		offline ? u"offline"_q : u"online"_q);
	session->api().request(MTPaccount_UpdateStatus(
		MTP_bool(offline)
	)).send();
}

} // namespace TeleForge::Presence
