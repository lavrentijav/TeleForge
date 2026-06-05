#include "ayu/features/teleforge/teleforge_bootstrap.h"

#include "ayu/features/plugins/plugin_manager.h"
#include "ayu/features/spy/online_history_storage.h"
#include "ayu/features/sync/teleforge_sync.h"
#include "ayu/features/teleforge/teleforge_core.h"
#include "ayu/features/teleforge/teleforge_inference.h"
#include "ayu/features/teleforge/teleforge_llama_runtime.h"
#include "ayu/features/teleforge/teleforge_paths.h"
#include "ayu/features/teleforge/teleforge_storage.h"
#include "ayu/features/teleforge/tf_peer_archive.h"
#include "core/application.h"
#include "main/main_domain.h"
#include "main/main_session.h"

#include "base/flat_set.h"
#include "logs.h"

#include <rpl/rpl.h>

#include <QtCore/QCoreApplication>

namespace TeleForge {

void initialize() {
	LOG(("TeleForge::initialize — chain: Application::run -> AyuInfra::init -> "
		"initDatabase (Ayu) then initTeleForge; opening teleforge.db under cWorkingDir"));
	Storage::initialize();

	if (!LoadPersonalityCore().has_value()) {
		LOG(("TeleForge: no PersonalityCore row yet, inserting DefaultPersonalityCore()"));
		PersistPersonalityCore(DefaultPersonalityCore());
	} else {
		LOG(("TeleForge: PersonalityCore row present, skipping default insert"));
	}
	TeleForgeLlamaBackendInit();
	TeleForgeEnsureModelsDirectoryExists();
	ApplyEndpointsFromStorage();
	TeleForge::Spy::initializeStorage();
	TeleForge::Sync::initialize();
	TeleForge::Plugins::initialize();
	const auto attachPeerArchive = [](not_null<Main::Session*> session) {
		static auto attached = base::flat_set<not_null<Main::Session*>>();
		if (attached.emplace(session).second) {
			TeleForge::PeerArchive::attachSession(session);
		}
	};
	if (const auto session = Core::App().maybePrimarySession()) {
		attachPeerArchive(session);
		// Create the dedicated sync chat right away (no-op when sync is off or
		// the chat already exists), then pull the latest bundle from it.
		TeleForge::Sync::ensureSyncChat(session, [=](bool ok, QString) {
			TeleForge::Sync::tryDownloadOnStartup(session);
		});
	}
	Core::App().domain().activeSessionChanges(
	) | rpl::on_next([=](Main::Session *session) {
		if (session) {
			attachPeerArchive(session);
		}
	});
	LOG(("TeleForge::initialize finished — inference endpoints applied from storage"));

	if (const auto app = QCoreApplication::instance()) {
		QObject::connect(app, &QCoreApplication::aboutToQuit, [] {
			ShutdownNativeChatLlama();
			ShutdownNativeEmbedLlama();
		});
	}
}

} // namespace TeleForge
