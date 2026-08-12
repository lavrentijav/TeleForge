#include "ayu/features/sync/teleforge_sync.h"

#include "ayu/ayu_settings.h"
#include "ayu/features/sync/teleforge_sync_crypto.h"
#include "ayu/features/sync/teleforge_sync_mysql.h"
#include "ayu/features/sync/teleforge_sync_pg.h"
#include "ayu/features/sync/teleforge_sync_transport.h"
#include "ayu/features/teleforge/teleforge_core.h"
#include "ayu/features/teleforge/teleforge_inference.h"
#include "ayu/features/teleforge/teleforge_vector_db.h"
#include "base/call_delayed.h"
#include "logs.h"
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

enum class Backend {
	TelegramChat = 0,
	Postgres = 1,
	MySql = 2,
};

[[nodiscard]] Backend SelectedBackend() {
	const auto core = LoadPersonalityCore();
	if (!core) {
		return Backend::TelegramChat;
	}
	switch (core->cloudBackend) {
	case int(Backend::Postgres): return Backend::Postgres;
	case int(Backend::MySql): return Backend::MySql;
	default: return Backend::TelegramChat;
	}
}

// Dispatches to the configured cloud backend. The local SQLite unified DB is
// always the source of truth; these are best-effort replication calls.
void BackendUpload(
		not_null<Main::Session*> session,
		const QByteArray &key,
		Fn<void(bool ok, QString error)> done) {
	switch (SelectedBackend()) {
	case Backend::Postgres:
		RunPgUpload(session, key, std::move(done));
		return;
	case Backend::MySql:
		RunMySqlUpload(session, key, std::move(done));
		return;
	case Backend::TelegramChat:
		RunSyncUpload(session, key, std::move(done));
		return;
	}
}

void BackendDownload(
		not_null<Main::Session*> session,
		const QByteArray &key,
		Fn<void(bool ok, QString error)> done) {
	switch (SelectedBackend()) {
	case Backend::Postgres:
		RunPgDownload(session, key, std::move(done));
		return;
	case Backend::MySql:
		RunMySqlDownload(session, key, std::move(done));
		return;
	case Backend::TelegramChat:
		RunSyncDownload(session, key, std::move(done));
		return;
	}
}

void RunUpload(not_null<Main::Session*> session) {
	const auto core = LoadPersonalityCore();
	if (!core || !core->memorySyncEnabled) {
		return;
	}
	BackendUpload(session, SyncKey(session), [](bool, QString) {});
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
	BackendUpload(session, key, [=](bool ok, QString error) {
		if (!ok) {
			if (done) {
				done(error.isEmpty()
					? u"Не удалось выполнить синхронизацию."_q
					: error);
			}
			return;
		}
		BackendDownload(session, key, [=](bool applied, QString derr) {
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
	BackendDownload(session, SyncKey(session), [=](bool, QString) {
		AyuSettings::load();
		ApplyEndpointsFromStorage();
	});
}

void checkRemoteFreshness(
		not_null<Main::Session*> session,
		Fn<void(bool stale, QString error)> done) {
	const auto core = LoadPersonalityCore();
	if (!core || !core->memorySyncEnabled) {
		if (done) {
			done(false, QString());
		}
		return;
	}
	if (core->cloudBackend != 1) {
		// Only the PostgreSQL backend keeps a server-side fingerprint that can
		// be read without pulling the whole snapshot; the other transports fall
		// back to the unconditional startup download.
		tryDownloadOnStartup(session);
		if (done) {
			done(false, QString());
		}
		return;
	}
	PgFetchRemoteStamp(session, [=](
			bool ok,
			QString sha256,
			qint64 revision,
			QString error) {
		if (!ok) {
			if (done) {
				done(false, error);
			}
			return;
		}
		if (sha256.isEmpty()) {
			// Nothing was ever uploaded, so there is nothing to be behind of.
			if (done) {
				done(false, QString());
			}
			return;
		}
		const auto known = VectorDb::KnownRemoteHash();
		const auto stale = (known != sha256);
		VectorDb::SetKnownRemoteHash(sha256);
		LOG(("TeleForge Sync: remote stamp %1 (rev %2), local known %3 -> %4")
			.arg(sha256)
			.arg(revision)
			.arg(known.isEmpty() ? u"<none>"_q : known)
			.arg(stale ? u"stale"_q : u"fresh"_q));
		if (stale) {
			tryDownloadOnStartup(session);
		}
		if (done) {
			done(stale, QString());
		}
	});
}

void setExportKey(const QByteArray &key) {
	const auto lock = std::unique_lock(g_mutex);
	g_exportKey = key;
}

} // namespace TeleForge::Sync
