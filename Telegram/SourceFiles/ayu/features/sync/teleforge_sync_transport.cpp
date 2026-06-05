#include "ayu/features/sync/teleforge_sync_transport.h"

#include "ayu/features/sync/teleforge_sync_files.h"
#include "ayu/features/sync/teleforge_sync_merger.h"
#include "ayu/utils/telegram_helpers.h"
#include "api/api_common.h"
#include "apiwrap.h"
#include "api/api_common.h"
#include "api/api_updates.h"
#include "base/call_delayed.h"
#include "base/random.h"
#include "base/unixtime.h"
#include "data/data_channel.h"
#include "data/data_document.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "data/notify/data_notify_settings.h"
#include "history/history.h"
#include "main/main_session.h"
#include "settings.h"

#include "logs.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QSysInfo>

#include <algorithm>

namespace TeleForge::Sync {
namespace {

const auto kMetaTag = u"TFMETA"_q;
const auto kLockTag = u"TFLOCK"_q;
constexpr auto kHistoryScan = 200;
constexpr auto kElectionWaitMs = 30000;
constexpr auto kLockTtlSec = 300;

[[nodiscard]] QString ChannelIdPath(not_null<Main::Session*> session) {
	return cWorkingDir()
		+ u"tdata/teleforge_sync_channel_%1.id"_q.arg(session->user()->id.value);
}

[[nodiscard]] QString SessionIdPath(not_null<Main::Session*> session) {
	return cWorkingDir()
		+ u"tdata/teleforge_sync_session_%1.id"_q.arg(session->user()->id.value);
}

struct StoredChannel {
	int64 id = 0;
	uint64 accessHash = 0;
};

[[nodiscard]] std::optional<StoredChannel> ReadStoredChannel(
		not_null<Main::Session*> session) {
	auto file = QFile(ChannelIdPath(session));
	if (!file.open(QIODevice::ReadOnly)) {
		return std::nullopt;
	}
	const auto parts = QString::fromUtf8(file.readAll()).split(':');
	if (parts.size() != 2) {
		return std::nullopt;
	}
	auto stored = StoredChannel();
	stored.id = parts[0].toLongLong();
	stored.accessHash = parts[1].toULongLong();
	if (!stored.id) {
		return std::nullopt;
	}
	return stored;
}

void WriteStoredChannel(
		not_null<Main::Session*> session,
		int64 id,
		uint64 accessHash) {
	auto file = QFile(ChannelIdPath(session));
	if (file.open(QIODevice::WriteOnly)) {
		file.write(QString::number(id).toUtf8()
			+ ':'
			+ QString::number(accessHash).toUtf8());
	}
}

[[nodiscard]] QString SessionHash(not_null<Main::Session*> session) {
	auto file = QFile(SessionIdPath(session));
	if (!file.exists()) {
		const auto id = QString::fromLatin1(
			QCryptographicHash::hash(
				QByteArray::number(base::RandomValue<uint64>())
				+ QSysInfo::machineUniqueId(),
				QCryptographicHash::Sha256).toHex());
		file.open(QIODevice::WriteOnly);
		file.write(id.toUtf8());
		file.close();
	}
	file.open(QIODevice::ReadOnly);
	return QString::fromUtf8(file.readAll());
}

void ForceGhostOffline(not_null<Main::Session*> session) {
	session->updates().updateOnline(crl::now());
}

[[nodiscard]] crl::time SyncGhostExtraWaitMs(
		not_null<Main::Session*> session,
		int delaySeconds = 12) {
	auto options = Api::SendOptions{};
	applyGhostScheduling(session, options, delaySeconds);
	if (!options.scheduled) {
		return 0;
	}
	return crl::time(std::max(0, options.scheduled - base::unixtime::now())) * 1000;
}

void ApplyGhostSyncOptions(
		not_null<Main::Session*> session,
		Api::SendOptions &options,
		int delaySeconds = 12) {
	applyGhostScheduling(session, options, delaySeconds);
}

void MuteSyncChannel(
		not_null<Main::Session*> session,
		not_null<ChannelData*> channel) {
	session->data().notifySettings().update(
		channel,
		Data::MuteValue{ .forever = true },
		true,
		Data::NotifySound{ .none = true },
		std::nullopt);
}

struct ParsedMessage {
	int id = 0;
	int date = 0;
	QString text;
};

struct LockClaim {
	QString sessionHash;
	int date = 0;
	int messageId = 0;
	int ttlSec = 0;
};

struct MetaShard {
	SyncDatabaseKind kind = SyncDatabaseKind::Data;
	SyncDocumentRef document;
	int dateFrom = 0;
	int dateTo = 0;
};

struct SyncManifest {
	int generation = 0;
	QString sessionHash;
	std::vector<MetaShard> shards;
	int messageDate = 0;
	int messageId = 0;
};

[[nodiscard]] SyncDatabaseKind DbKindFromString(const QString &value) {
	// Legacy manifests used separate "teleforge" / "ayudata" shards.
	return SyncDatabaseKind::Data;
}

[[nodiscard]] QString DbKindToString(SyncDatabaseKind kind) {
	Q_UNUSED(kind);
	return u"data"_q;
}

[[nodiscard]] SyncDocumentRef DocumentFromJson(const QJsonObject &object) {
	auto ref = SyncDocumentRef();
	ref.id = object.value(u"docId"_q).toString().toLongLong();
	ref.accessHash = object.value(u"accessHash"_q).toString().toULongLong();
	ref.fileReference = QByteArray::fromBase64(
		object.value(u"fileReference"_q).toString().toLatin1());
	ref.dcId = object.value(u"dcId"_q).toInt();
	ref.size = object.value(u"size"_q).toString().toLongLong();
	ref.sha256Hex = object.value(u"sha256"_q).toString();
	return ref;
}

[[nodiscard]] QJsonObject DocumentToJson(const SyncDocumentRef &ref, const MetaShard &shard) {
	return QJsonObject{
		{ u"db"_q, DbKindToString(shard.kind) },
		{ u"dateFrom"_q, shard.dateFrom },
		{ u"dateTo"_q, shard.dateTo },
		{ u"docId"_q, QString::number(ref.id) },
		{ u"accessHash"_q, QString::number(ref.accessHash) },
		{ u"fileReference"_q, QString::fromLatin1(ref.fileReference.toBase64()) },
		{ u"dcId"_q, ref.dcId },
		{ u"size"_q, QString::number(ref.size) },
		{ u"sha256"_q, ref.sha256Hex },
	};
}

void FetchChannelMessages(
		not_null<Main::Session*> session,
		not_null<ChannelData*> channel,
		Fn<void(QVector<ParsedMessage>)> done) {
	session->api().request(MTPmessages_GetHistory(
		channel->input(),
		MTP_int(0),
		MTP_int(0),
		MTP_int(0),
		MTP_int(kHistoryScan),
		MTP_int(0),
		MTP_int(0),
		MTP_long(0)
	)).done([=](const MTPmessages_Messages &result) {
		auto messages = QVector<ParsedMessage>();
		const auto collect = [&](const QVector<MTPMessage> &list) {
			for (const auto &message : list) {
				message.match([&](const MTPDmessage &data) {
					messages.push_back({
						.id = data.vid().v,
						.date = data.vdate().v,
						.text = qs(data.vmessage()),
					});
				}, [&](const auto &) {
				});
			}
		};
		result.match([&](const MTPDmessages_messages &d) {
			collect(d.vmessages().v);
		}, [&](const MTPDmessages_messagesSlice &d) {
			collect(d.vmessages().v);
		}, [&](const MTPDmessages_channelMessages &d) {
			collect(d.vmessages().v);
		}, [&](const auto &) {
		});
		if (done) {
			done(messages);
		}
	}).fail([=](const MTP::Error &) {
		if (done) {
			done({});
		}
	}).send();
}

[[nodiscard]] std::optional<SyncManifest> ParseLatestManifest(
		const QVector<ParsedMessage> &messages) {
	auto best = SyncManifest();
	auto found = false;
	for (const auto &message : messages) {
		if (!message.text.startsWith(kMetaTag + '|')) {
			continue;
		}
		const auto pipe = message.text.indexOf('|', kMetaTag.size());
		if (pipe < 0) {
			continue;
		}
		const auto json = QJsonDocument::fromJson(
			message.text.mid(pipe + 1).toUtf8()).object();
		if (json.isEmpty()) {
			continue;
		}
		if (found && message.date <= best.messageDate) {
			continue;
		}
		found = true;
		best.generation = json.value(u"gen"_q).toInt();
		best.sessionHash = json.value(u"sessionHash"_q).toString();
		best.messageDate = message.date;
		best.messageId = message.id;
		best.shards.clear();
		for (const auto &item : json.value(u"shards"_q).toArray()) {
			const auto object = item.toObject();
			best.shards.push_back({
				.kind = DbKindFromString(object.value(u"db"_q).toString()),
				.document = DocumentFromJson(object),
				.dateFrom = object.value(u"dateFrom"_q).toInt(),
				.dateTo = object.value(u"dateTo"_q).toInt(),
			});
		}
	}
	return found ? std::make_optional(best) : std::nullopt;
}

[[nodiscard]] std::vector<LockClaim> ParseLockClaims(
		const QVector<ParsedMessage> &messages) {
	const auto now = base::unixtime::now();
	auto claims = std::vector<LockClaim>();
	for (const auto &message : messages) {
		if (!message.text.startsWith(kLockTag + '|')) {
			continue;
		}
		const auto parts = message.text.split('|');
		if (parts.size() < 3) {
			continue;
		}
		const auto ttl = parts[2].toInt();
		if (ttl > 0 && message.date + ttl < now) {
			continue;
		}
		claims.push_back({
			.sessionHash = parts[1],
			.date = message.date,
			.messageId = message.id,
			.ttlSec = ttl,
		});
	}
	return claims;
}

[[nodiscard]] bool WeWinElection(
		const std::vector<LockClaim> &claims,
		const QString &ourHash) {
	if (claims.empty()) {
		return true;
	}
	const auto winner = std::min_element(claims.begin(), claims.end(), [&](const LockClaim &a, const LockClaim &b) {
		if (a.date != b.date) {
			return a.date < b.date;
		}
		return a.sessionHash < b.sessionHash;
	});
	return winner != claims.end() && winner->sessionHash == ourHash;
}

void PostLockClaim(
		not_null<Main::Session*> session,
		not_null<ChannelData*> channel,
		const QString &sessionHash) {
	const auto history = session->data().history(channel);
	const auto text = kLockTag
		+ '|' + sessionHash
		+ '|' + QString::number(kLockTtlSec);
	auto message = Api::MessageToSend(Api::SendAction(history));
	message.textWithTags = { text };
	message.action.options.silent = true;
	ApplyGhostSyncOptions(session, message.action.options);
	session->api().sendMessage(std::move(message));
	ForceGhostOffline(session);
}

void RunLeaderElection(
		not_null<Main::Session*> session,
		not_null<ChannelData*> channel,
		Fn<void(bool won, QString error)> done) {
	const auto sessionHash = SessionHash(session);
	const auto waitMs = kElectionWaitMs + SyncGhostExtraWaitMs(session);
	PostLockClaim(session, channel, sessionHash);
	base::call_delayed(waitMs, session, [=] {
		FetchChannelMessages(session, channel, [=](QVector<ParsedMessage> messages) {
			const auto claims = ParseLockClaims(messages);
			if (WeWinElection(claims, sessionHash)) {
				if (done) {
					done(true, {});
				}
			} else if (done) {
				done(false, u"Другая сессия выполняет синхронизацию."_q);
			}
		});
	});
}

void ApplyManifest(
		not_null<Main::Session*> session,
		const SyncManifest &manifest,
		const QByteArray &syncKey,
		Fn<void(bool ok)> done) {
	if (manifest.shards.empty()) {
		if (done) {
			done(true);
		}
		return;
	}
	auto state = std::make_shared<int>(int(manifest.shards.size()));
	auto okState = std::make_shared<bool>(true);
	for (const auto &shard : manifest.shards) {
		DownloadEncryptedDocument(session, shard.document, [=](QByteArray payload, QString error) {
			if (!error.isEmpty() || payload.isEmpty()) {
				*okState = false;
			} else if (!ApplyEncryptedDatabaseShard(shard.kind, payload, syncKey)) {
				*okState = false;
			}
			if (--(*state) == 0 && done) {
				done(*okState);
			}
		});
	}
}

void PostManifest(
		not_null<Main::Session*> session,
		not_null<ChannelData*> channel,
		const SyncManifest &manifest) {
	const auto history = session->data().history(channel);
	auto shards = QJsonArray();
	for (const auto &shard : manifest.shards) {
		shards.push_back(DocumentToJson(shard.document, shard));
	}
	const auto json = QJsonDocument(QJsonObject{
		{ u"gen"_q, manifest.generation },
		{ u"sessionHash"_q, manifest.sessionHash },
		{ u"shards"_q, shards },
	}).toJson(QJsonDocument::Compact);
	const auto text = kMetaTag + '|' + QString::fromUtf8(json);
	auto message = Api::MessageToSend(Api::SendAction(history));
	message.textWithTags = { text };
	message.action.options.silent = true;
	ApplyGhostSyncOptions(session, message.action.options);
	session->api().sendMessage(std::move(message));
	ForceGhostOffline(session);
}

void UploadShardsSequential(
		not_null<Main::Session*> session,
		not_null<ChannelData*> channel,
		std::vector<EncryptedDbShard> shards,
		const QString &sessionHash,
		Fn<void(bool ok, QString error)> done) {
	if (shards.empty()) {
		if (done) {
			done(false, u"Не удалось подготовить снимки баз данных."_q);
		}
		return;
	}
	auto manifest = SyncManifest{
		.generation = int(base::unixtime::now()),
		.sessionHash = sessionHash,
	};
	auto index = std::make_shared<size_t>(0);
	const auto uploadNext = std::make_shared<Fn<void()>>();
	*uploadNext = [=]() mutable {
		if (*index >= shards.size()) {
			PostManifest(session, channel, manifest);
			if (done) {
				done(true, {});
			}
			return;
		}
		const auto &shard = shards[*index];
		UploadEncryptedDocument(
			session,
			channel,
			shard.encrypted,
			shard.fileName,
			[=](bool ok, SyncDocumentRef ref, QString error) mutable {
				if (!ok) {
					if (done) {
						done(false, error);
					}
					return;
				}
				manifest.shards.push_back({
					.kind = shard.kind,
					.document = ref,
					.dateFrom = shard.dateFrom,
					.dateTo = shard.dateTo,
				});
				++(*index);
				(*uploadNext)();
			});
	};
	(*uploadNext)();
}

} // namespace

ChannelData *ResolveSyncChannel(not_null<Main::Session*> session) {
	const auto stored = ReadStoredChannel(session);
	if (!stored) {
		return nullptr;
	}
	const auto channel = session->data().channel(ChannelId(stored->id));
	if (!channel->accessHash()) {
		channel->setAccessHash(stored->accessHash);
	}
	return channel;
}

bool IsSyncChannel(not_null<Main::Session*> session, PeerId peerId) {
	const auto stored = ReadStoredChannel(session);
	if (!stored) {
		return false;
	}
	return peerToChannel(peerId).bare == stored->id;
}

void EnsureSyncChannel(
		not_null<Main::Session*> session,
		Fn<void(bool ok, QString error)> done) {
	if (const auto existing = ResolveSyncChannel(session)) {
		MuteSyncChannel(session, existing);
		if (done) {
			done(true, {});
		}
		return;
	}
	using Flag = MTPchannels_CreateChannel::Flag;
	session->api().request(MTPchannels_CreateChannel(
		MTP_flags(Flag::f_broadcast),
		MTP_string("TeleForge Sync"),
		MTP_string("Encrypted TeleForge settings & memory. Do not delete."),
		MTPInputGeoPoint(),
		MTPstring(),
		MTP_int(0)
	)).done([=](const MTPUpdates &result) {
		session->api().applyUpdates(result);

		const QVector<MTPChat> *chats = nullptr;
		result.match([&](const MTPDupdates &d) {
			chats = &d.vchats().v;
		}, [&](const MTPDupdatesCombined &d) {
			chats = &d.vchats().v;
		}, [&](const auto &) {
		});
		if (!chats
			|| chats->isEmpty()
			|| chats->front().type() != mtpc_channel) {
			if (done) {
				done(false, u"Не удалось создать чат синхронизации."_q);
			}
			return;
		}
		const auto &data = chats->front().c_channel();
		const auto channel = session->data().channel(ChannelId(data.vid().v));
		WriteStoredChannel(
			session,
			peerToChannel(channel->id).bare,
			channel->accessHash());
		MuteSyncChannel(session, channel);
		if (done) {
			done(true, {});
		}
	}).fail([=](const MTP::Error &error) {
		if (done) {
			done(false, error.type());
		}
	}).send();
}

void RunSyncDownload(
		not_null<Main::Session*> session,
		const QByteArray &syncKey,
		Fn<void(bool ok, QString error)> done) {
	const auto channel = ResolveSyncChannel(session);
	if (!channel) {
		if (done) {
			done(false, u"Чат синхронизации не найден."_q);
		}
		return;
	}
	FetchChannelMessages(session, channel, [=](QVector<ParsedMessage> messages) {
		const auto manifest = ParseLatestManifest(messages);
		if (!manifest) {
			if (done) {
				done(true, {});
			}
			return;
		}
		ApplyManifest(session, *manifest, syncKey, [=](bool ok) {
			if (done) {
				done(ok, ok ? QString() : u"Ошибка применения снимка."_q);
			}
		});
	});
}

void RunSyncUpload(
		not_null<Main::Session*> session,
		const QByteArray &syncKey,
		Fn<void(bool ok, QString error)> done) {
	EnsureSyncChannel(session, [=](bool ok, QString error) {
		if (!ok) {
			if (done) {
				done(false, error);
			}
			return;
		}
		const auto channel = ResolveSyncChannel(session);
		if (!channel) {
			if (done) {
				done(false, u"Чат синхронизации недоступен."_q);
			}
			return;
		}
		RunLeaderElection(session, channel, [=](bool won, QString electionError) {
			if (!won) {
				if (done) {
					done(false, electionError);
				}
				return;
			}
			RunSyncDownload(session, syncKey, [=](bool, QString) {
				const auto sessionHash = SessionHash(session);
				const auto shards = BuildEncryptedDatabaseShards(syncKey);
				UploadShardsSequential(
					session,
					channel,
					std::move(shards),
					sessionHash,
					done);
			});
		});
	});
}

} // namespace TeleForge::Sync
