#include "ayu/features/sync/teleforge_sync.h"

#include "ayu/ayu_settings.h"
#include "ayu/features/sync/teleforge_sync_crypto.h"
#include "ayu/features/sync/teleforge_sync_transport.h"
#include "ayu/features/teleforge/teleforge_core.h"
#include "ayu/features/teleforge/teleforge_inference.h"
#include "base/call_delayed.h"
#include "data/data_user.h"
#include "main/main_session.h"

#include <mutex>

namespace TeleForge::Sync {
namespace {

auto g_exportKey = QByteArray();
auto g_uploadScheduled = false;
auto g_mutex = std::mutex();

[[nodiscard]] QByteArray MasterMaterial(not_null<Main::Session*> session) {
	return QByteArray::number(session->user()->id.value)
		+ session->user()->phone().toUtf8();
}

[[nodiscard]] QByteArray SyncKey(not_null<Main::Session*> session) {
	return DeriveSyncKey(MasterMaterial(session), g_exportKey);
}

void RunUpload(not_null<Main::Session*> session) {
	const auto core = LoadPersonalityCore();
	if (!core || !core->memorySyncEnabled) {
		return;
	}
	RunSyncUpload(session, SyncKey(session), [](bool, QString) {});
}

} // namespace

void initialize() {
}

void scheduleUploadDebounced(not_null<Main::Session*> session) {
	const auto core = LoadPersonalityCore();
	if (!core || !core->memorySyncEnabled) {
		return;
	}
	{
		const auto lock = std::unique_lock(g_mutex);
		if (g_uploadScheduled) {
			return;
		}
		g_uploadScheduled = true;
	}
	base::call_delayed(300000, [=] {
		{
			const auto lock = std::unique_lock(g_mutex);
			g_uploadScheduled = false;
		}
		RunUpload(session);
	});
}

void syncNow(not_null<Main::Session*> session, Fn<void(QString message)> done) {
	const auto core = LoadPersonalityCore();
	if (!core || !core->memorySyncEnabled) {
		if (done) {
			done(u"Синхронизация отключена в настройках."_q);
		}
		return;
	}
	const auto key = SyncKey(session);
	RunSyncUpload(session, key, [=](bool ok, QString error) {
		if (!ok) {
			if (done) {
				done(error.isEmpty()
					? u"Не удалось выполнить синхронизацию."_q
					: error);
			}
			return;
		}
		RunSyncDownload(session, key, [=](bool applied, QString derr) {
			AyuSettings::load();
			ApplyEndpointsFromStorage();
			if (done) {
				done(applied && derr.isEmpty()
					? u"Синхронизация завершена."_q
					: (derr.isEmpty()
						? u"Синхронизация завершена."_q
						: u"Загрузка: "_q + derr));
			}
		});
	});
}

void ensureSyncChat(
		not_null<Main::Session*> session,
		Fn<void(bool ok, QString message)> done) {
	const auto core = LoadPersonalityCore();
	if (!core || !core->memorySyncEnabled) {
		if (done) {
			done(false, u"Синхронизация отключена в настройках."_q);
		}
		return;
	}
	EnsureSyncChannel(session, [=](bool ok, QString error) {
		if (done) {
			done(ok, ok
				? u"Чат синхронизации готов."_q
				: (error.isEmpty()
					? u"Не удалось создать чат синхронизации."_q
					: error));
		}
	});
}

void tryDownloadOnStartup(not_null<Main::Session*> session) {
	const auto core = LoadPersonalityCore();
	if (!core || !core->memorySyncEnabled) {
		return;
	}
	RunSyncDownload(session, SyncKey(session), [=](bool, QString) {
		AyuSettings::load();
		ApplyEndpointsFromStorage();
	});
}

void setExportKey(const QByteArray &key) {
	const auto lock = std::unique_lock(g_mutex);
	g_exportKey = key;
}

} // namespace TeleForge::Sync
