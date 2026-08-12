// Copyright @Radolyn, 2026

#pragma once

#include <QtCore/QString>
#include <QtCore/QStringList>

namespace Main {
class Session;
} // namespace Main

namespace TeleForge::Ghost {

/// Every outgoing request that the Telegram servers can interpret as account
/// activity is announced here before it is sent. When ghost mode is active the
/// call is logged and counted, which is what makes it possible to tell which
/// feature woke the account instead of guessing from the online indicator.
void noteWakeSignal(
	Main::Session *session,
	const QString &kind,
	const QString &detail = QString());

[[nodiscard]] QStringList recentWakeSignals();
[[nodiscard]] int wakeSignalCount(const QString &kind);
void clearWakeSignals();

} // namespace TeleForge::Ghost
