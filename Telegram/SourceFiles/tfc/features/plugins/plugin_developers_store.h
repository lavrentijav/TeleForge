#pragma once

#include <QString>
#include <QVector>
#include <functional>

namespace TeleForge::Plugins {

struct TrustedDeveloper {
	QString devId;
	QString channelUsername;
	QString title;
	QString pubkeyHex;
	QString rootSignatureHex;
	bool rootVerified = false;
};

using DevelopersCallback = Fn<void(QVector<TrustedDeveloper> devs, QString error)>;

void fetchTrustedDevelopers(DevelopersCallback done);
[[nodiscard]] QVector<TrustedDeveloper> cachedTrustedDevelopers();
[[nodiscard]] const TrustedDeveloper *findDeveloper(const QString &devId);
[[nodiscard]] bool isTrustedChannel(const QString &username);

} // namespace TeleForge::Plugins
