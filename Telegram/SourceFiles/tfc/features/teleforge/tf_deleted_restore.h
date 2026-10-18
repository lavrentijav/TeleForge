// Copyright @Radolyn, 2026

#pragma once

#include "data/data_peer.h"
#include "main/main_session.h"

class History;

namespace TeleForge::DeletedRestore {

/// Re-creates locally stored deleted messages as local items inside the live
/// chat history, so a chat opened after a restart still shows what was removed
/// instead of only carrying it in the database.
void restoreForHistory(not_null<History*> history);

void attachSession(not_null<Main::Session*> session);

} // namespace TeleForge::DeletedRestore
