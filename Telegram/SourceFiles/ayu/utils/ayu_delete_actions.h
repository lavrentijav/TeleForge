// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#pragma once

#include "data/data_msg_id.h"

namespace Main {
class Session;
} // namespace Main

namespace AyuDelete {

[[nodiscard]] bool AnyMessageSupportsStubReplace(
	not_null<Main::Session*> session,
	const MessageIdsList &ids);

void DeleteMessages(
	not_null<Main::Session*> session,
	MessageIdsList ids,
	bool revoke,
	bool replaceWithStub);

} // namespace AyuDelete
