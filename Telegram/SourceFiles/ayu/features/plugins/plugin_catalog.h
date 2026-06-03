#pragma once

#include <array>

#include <QString>

namespace TeleForge::Plugins::Catalog {

// Trusted plugin developers (TeleForge root attestation). Host on your VPS / Pages.
inline constexpr auto kDevelopersUrl =
	"https://lavrentijav.github.io/TeleForge/developers.txt";

// Optional plugin index (must be signed by a trusted developer).
inline constexpr auto kCatalogUrl =
	"https://lavrentijav.github.io/TeleForge/plugins.txt";

// developers.txt: dev_id|channel|title|ed25519_pubkey_hex|root_signature_hex
// plugins.txt:    file.py|Title|https://url|dev_id|plugin_signature_hex

struct SubscribeChannel {
	const char *username = "";
	const char *title = "";
	const char *description = "";
};

inline constexpr std::array<SubscribeChannel, 2> kSubscribeChannels = { {
	{
		"teleforge_official",
		"TeleForge Official",
		"Новости, релизы и обновления каталога",
	},
	{
		"teleforgechat",
		"TeleForge Chat",
		"Сообщество и помощь",
	},
} };

} // namespace TeleForge::Plugins::Catalog
