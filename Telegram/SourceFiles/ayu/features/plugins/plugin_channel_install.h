#pragma once

#include "ayu/features/plugins/plugin_catalog_store.h"

#include <functional>

namespace Main {
class Session;
} // namespace Main

namespace TeleForge::Plugins {

struct ChannelPluginOffer {
	QString devId;
	QString channelUsername;
	QString channelTitle;
	QString fileName;
	int64 channelId = 0;
	int messageId = 0;
	QString pluginSignatureHex;
};

using ChannelOffersCallback = Fn<void(QVector<ChannelPluginOffer> offers, QString error)>;

void scanDeveloperChannel(
	not_null<Main::Session*> session,
	const QString &devId,
	ChannelOffersCallback done);
void installChannelOffer(
	not_null<Main::Session*> session,
	const ChannelPluginOffer &offer,
	DoneCallback done);

} // namespace TeleForge::Plugins
