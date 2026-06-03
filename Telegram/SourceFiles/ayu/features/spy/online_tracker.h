#pragma once

#include "data/data_lastseen_status.h"
#include "data/data_user.h"

namespace TeleForge::Spy {

void recordUserStatus(not_null<UserData*> user, Data::LastseenStatus status);
[[nodiscard]] Data::LastseenStatus applyLastseen(
	not_null<UserData*> user,
	Data::LastseenStatus status);

} // namespace TeleForge::Spy
