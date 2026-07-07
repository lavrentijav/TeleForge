// Copyright @Radolyn, 2026

#include "ayu/features/subscription/teleforge_subscription.h"

#include "ayu/features/plugins/plugin_catalog.h"
#include "apiwrap.h"
#include "data/data_channel.h"
#include "data/data_peer.h"
#include "data/data_peer_id.h"
#include "data/data_session.h"
#include "main/main_session.h"

#include <rpl/variable.h>

#include <memory>

namespace TeleForge::Subscription {
namespace {

auto g_active = rpl::variable<bool>(false);

void ApplyResolved(
		std::shared_ptr<int> pending,
		std::shared_ptr<bool> anyIn,
		ChannelData *channel) {
	if (channel && channel->amIn()) {
		*anyIn = true;
	}
	if (--(*pending) == 0) {
		g_active = *anyIn;
	}
}

} // namespace

bool active() {
	return g_active.current();
}

rpl::producer<bool> activeValue() {
	return g_active.value();
}

void refresh(not_null<Main::Session*> session) {
	const auto &channels = Plugins::Catalog::kSubscribeChannels;
	const auto pending = std::make_shared<int>(int(channels.size()));
	const auto anyIn = std::make_shared<bool>(false);
	for (const auto &ch : channels) {
		const auto username = QString::fromLatin1(ch.username);
		if (const auto peer = session->data().peerByUsername(username)) {
			if (const auto channel = peer->asChannel()) {
				ApplyResolved(pending, anyIn, channel);
				continue;
			}
		}
		session->api().request(MTPcontacts_ResolveUsername(
			MTP_flags(0),
			MTP_string(username),
			MTP_string()
		)).done([=](const MTPcontacts_ResolvedPeer &result) {
			const auto &data = result.data();
			session->data().processUsers(data.vusers());
			session->data().processChats(data.vchats());
			const auto peer = session->data().peerLoaded(
				peerFromMTP(data.vpeer()));
			ApplyResolved(
				pending,
				anyIn,
				peer ? peer->asChannel() : nullptr);
		}).fail([=](const MTP::Error &) {
			ApplyResolved(pending, anyIn, nullptr);
		}).send();
	}
}

void attachSession(not_null<Main::Session*> session) {
	refresh(session);
}

} // namespace TeleForge::Subscription
