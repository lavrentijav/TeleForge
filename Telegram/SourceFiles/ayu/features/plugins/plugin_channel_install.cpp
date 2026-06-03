#include "ayu/features/plugins/plugin_channel_install.h"

#include "ayu/features/plugins/plugin_dev_mode.h"
#include "ayu/features/plugins/plugin_developers_store.h"
#include "ayu/features/plugins/plugin_manager.h"
#include "ayu/features/plugins/plugin_registry.h"
#include "ayu/features/plugins/plugin_trust.h"
#include "apiwrap.h"
#include "data/data_channel.h"
#include "data/data_document.h"
#include "data/data_file_origin.h"
#include "data/data_session.h"
#include "history/history_item.h"
#include "main/main_session.h"
#include "base/flat_map.h"
#include <crl/crl.h>
#include "rpl/rpl.h"
#include "settings.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>

namespace TeleForge::Plugins {
namespace {

constexpr auto kHistoryLimit = 100;

[[nodiscard]] QString DocumentFileName(const MTPDocument &document) {
	auto result = QString();
	document.match([&](const MTPDdocument &data) {
		for (const auto &attr : data.vattributes().v) {
			attr.match([&](const MTPDdocumentAttributeFilename &f) {
				result = qs(f.vfile_name());
			}, [&](const auto &) {
			});
		}
	}, [&](const auto &) {
	});
	return result;
}

[[nodiscard]] bool IsPluginFile(const QString &name) {
	return name.endsWith(u".py"_q, Qt::CaseInsensitive);
}

[[nodiscard]] bool IsSigFile(const QString &name) {
	return name.endsWith(u".py.sig"_q, Qt::CaseInsensitive)
		|| name.endsWith(u".plugin.sig"_q, Qt::CaseInsensitive);
}

[[nodiscard]] QString BasePluginName(const QString &sigName) {
	if (sigName.endsWith(u".py.sig"_q, Qt::CaseInsensitive)) {
		return sigName.left(sigName.size() - 4);
	}
	if (sigName.endsWith(u".plugin.sig"_q, Qt::CaseInsensitive)) {
		return sigName.left(sigName.size() - 8);
	}
	return {};
}

void ScanChannel(
		not_null<Main::Session*> session,
		not_null<ChannelData*> channel,
		const TrustedDeveloper &dev,
		Fn<void(QVector<ChannelPluginOffer>)> done) {
	session->api().request(MTPmessages_GetHistory(
		channel->input(),
		MTP_int(0),
		MTP_int(0),
		MTP_int(0),
		MTP_int(kHistoryLimit),
		MTP_int(0),
		MTP_int(0),
		MTP_long(0)
	)).done([=](const MTPmessages_Messages &result) {
		session->data().processExistingMessages(channel, result);
		auto sigByBase = base::flat_map<QString, QString>();
		auto pyMessages = base::flat_map<QString, int>();

		const auto collect = [&](const QVector<MTPMessage> &list) {
			for (const auto &message : list) {
				message.match([&](const MTPDmessage &data) {
					const auto text = qs(data.vmessage());
					const auto media = data.vmedia();
					if (!media || media->type() != mtpc_messageMediaDocument) {
						return;
					}
					const auto document = media->c_messageMediaDocument().vdocument();
					if (!document) {
						return;
					}
					const auto fileName = DocumentFileName(*document);
					const auto msgId = data.vid().v;
					if (IsPluginFile(fileName)) {
						pyMessages.emplace(fileName, msgId);
						if (text.startsWith(u"TFPLUGIN "_q)) {
							const auto parts = text.mid(9).split(u'|');
							if (parts.size() >= 3
								&& parts[0].trimmed() == dev.devId) {
								sigByBase.emplace(
									fileName,
									parts[2].trimmed().toLower());
							}
						}
						return;
					}
					if (IsSigFile(fileName)) {
						const auto item = session->data().message(
							channel->id,
							MsgId(msgId));
						if (!item || !item->media() || !item->media()->document()) {
							return;
						}
						const auto path = item->media()->document()->filepath(true);
						if (path.isEmpty()) {
							return;
						}
						auto f = QFile(path);
						if (!f.open(QIODevice::ReadOnly)) {
							return;
						}
						const auto line = QString::fromUtf8(f.readAll()).trimmed();
						const auto parts = line.split(u'|');
						if (parts.size() >= 3 && parts[0].trimmed() == dev.devId) {
							sigByBase.emplace(
								BasePluginName(fileName),
								parts[2].trimmed().toLower());
						}
					}
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

		auto offers = QVector<ChannelPluginOffer>();
		for (const auto &[fileName, msgId] : pyMessages) {
			const auto sig = sigByBase.find(fileName);
			if (sig == sigByBase.end()) {
				continue;
			}
			offers.push_back({
				.devId = dev.devId,
				.channelUsername = dev.channelUsername,
				.channelTitle = dev.title,
				.fileName = fileName,
				.channelId = qint64(channel->id.value),
				.messageId = msgId,
				.pluginSignatureHex = sig->second,
			});
		}
		if (done) {
			done(std::move(offers));
		}
	}).fail([=](const MTP::Error &) {
		if (done) {
			done({});
		}
	}).send();
}

void DownloadDocument(
		not_null<Main::Session*> session,
		not_null<DocumentData*> document,
		not_null<HistoryItem*> item,
		const QString &dest,
		Fn<void(bool, QString)> done) {
	if (QFile::exists(dest)) {
		done(true, {});
		return;
	}
	document->save(Data::FileOriginMessage(item->fullId()), dest);
	session->downloaderTaskFinished() | rpl::filter([=] {
		return !document->loading()
			|| document->status == FileDownloadFailed;
	}) | rpl::take(1) | rpl::on_next([=] {
		if (document->status == FileDownloadFailed || !QFile::exists(dest)) {
			done(false, u"Ошибка скачивания."_q);
			return;
		}
		done(true, {});
	}, session->lifetime());
}

} // namespace

void scanDeveloperChannel(
		not_null<Main::Session*> session,
		const QString &devId,
		ChannelOffersCallback done) {
	const auto dev = findDeveloper(devId);
	if (!dev) {
		done({}, u"Разработчик не удостоверен TeleForge."_q);
		return;
	}
	session->api().request(MTPcontacts_ResolveUsername(
		MTP_flags(0),
		MTP_string(dev->channelUsername),
		MTP_string()
	)).done([=](const MTPcontacts_ResolvedPeer &result) {
		auto &data = result.c_contacts_resolvedPeer();
		session->data().processUsers(data.vusers());
		session->data().processChats(data.vchats());
		const auto peer = session->data().peerLoaded(peerFromMTP(data.vpeer()));
		const auto channel = peer ? peer->asChannel() : nullptr;
		if (!channel) {
			done({}, u"Канал разработчика не найден."_q);
			return;
		}
		ScanChannel(session, channel, *dev, [=](QVector<ChannelPluginOffer> offers) {
			const auto error = offers.isEmpty()
				? u"В канале нет подписанных .py + .py.sig."_q
				: QString();
			done(offers, error);
		});
	}).fail([=](const MTP::Error &) {
		done({}, u"Не удалось открыть канал разработчика."_q);
	}).send();
}

void installChannelOffer(
		not_null<Main::Session*> session,
		const ChannelPluginOffer &offer,
		DoneCallback done) {
	const auto dev = findDeveloper(offer.devId);
	if (!dev) {
		done(false, u"Разработчик не удостоверен."_q);
		return;
	}
	const auto peer = session->data().peerLoaded(PeerId(offer.channelId));
	const auto channel = peer ? peer->asChannel() : nullptr;
	if (!channel) {
		done(false, u"Канал недоступен."_q);
		return;
	}
	const auto item = session->data().message(
		channel->id,
		MsgId(offer.messageId));
	if (!item || !item->media() || !item->media()->document()) {
		done(false, u"Сообщение с плагином не найдено."_q);
		return;
	}
	const auto doc = item->media()->document();
	const auto pluginsDir = cWorkingDir() + u"plugins"_q;
	QDir().mkpath(pluginsDir);
	const auto dest = QDir(pluginsDir).absoluteFilePath(offer.fileName);
	DownloadDocument(session, doc, item, dest, [=](bool ok, QString err) {
		if (!ok) {
			done(false, err);
			return;
		}
		auto body = QFile(dest);
		if (!body.open(QIODevice::ReadOnly)) {
			done(false, u"Не удалось прочитать плагин."_q);
			return;
		}
		const auto bytes = body.readAll();
		body.close();
		const auto sha = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
		if (!DevMode::allowUnverifiedInstalls()) {
			const auto pub = Trust::PublicKeyFromHex(dev->pubkeyHex);
			if (!Trust::VerifyDeveloperPluginSignature(
					pub,
					sha,
					offer.devId,
					offer.fileName,
					offer.pluginSignatureHex)) {
				QFile::remove(dest);
				done(false, u"Подпись плагина не прошла проверку."_q);
				return;
			}
		}
		setSource(offer.fileName, u"@%1 · %2"_q.arg(dev->channelUsername, dev->title));
		setEnabled(offer.fileName, true);
		reloadAll();
		const auto suffix = DevMode::allowUnverifiedInstalls()
			? u" (режим разработчика)"_q
			: QString();
		done(true, u"Установлено%1: %2"_q.arg(suffix, offer.fileName));
	});
}

} // namespace TeleForge::Plugins
