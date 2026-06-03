#include "ayu/features/sync/teleforge_sync_files.h"

#include "api/api_common.h"
#include "apiwrap.h"
#include "base/random.h"
#include "data/data_channel.h"
#include "data/data_histories.h"
#include "data/data_session.h"
#include "history/history.h"
#include "main/main_session.h"

#include "logs.h"

#include <QtCore/QCryptographicHash>

namespace TeleForge::Sync {
namespace {

constexpr auto kPartSize = int(512 * 1024);
constexpr auto kUseBigFilesFrom = int64(10 * 1024 * 1024);
constexpr auto kDownloadPartSize = int(128 * 1024);

struct UploadState {
	not_null<Main::Session*> session;
	not_null<ChannelData*> channel;
	QByteArray payload;
	QString fileName;
	Fn<void(bool ok, SyncDocumentRef ref, QString error)> done;

	int64 fileId = 0;
	int partsCount = 0;
	int nextPart = 0;
	bool big = false;
};

struct DownloadState {
	not_null<Main::Session*> session;
	SyncDocumentRef ref;
	Fn<void(QByteArray payload, QString error)> done;
	QByteArray buffer;
	int64 offset = 0;
};

[[nodiscard]] SyncDocumentRef RefFromDocument(const MTPDocument &document) {
	auto ref = SyncDocumentRef();
	document.match([&](const MTPDdocument &data) {
		ref.id = data.vid().v;
		ref.accessHash = data.vaccess_hash().v;
		ref.fileReference = data.vfile_reference().v;
		ref.dcId = data.vdc_id().v;
		ref.size = data.vsize().v;
	}, [&](const auto &) {
	});
	return ref;
}

void ExtractDocumentFromUpdates(
		const MTPUpdates &updates,
		Fn<void(const SyncDocumentRef &)> consume) {
	const auto consumeDocumentData = [&](const MTPDdocument &data) {
		auto ref = SyncDocumentRef();
		ref.id = data.vid().v;
		ref.accessHash = data.vaccess_hash().v;
		ref.fileReference = data.vfile_reference().v;
		ref.dcId = data.vdc_id().v;
		ref.size = data.vsize().v;
		consume(ref);
	};
	const auto consumeDocument = [&](const auto &documentField) {
		if (const auto document = documentField) {
			document->match([&](const MTPDdocument &data) {
				consumeDocumentData(data);
			}, [](const auto &) {
			});
		}
	};
	const auto consumeMedia = [&](const auto &mediaField) {
		mediaField.match([&](const MTPDmessageMediaDocument &documentMedia) {
			consumeDocument(documentMedia.vdocument());
		}, [](const auto &) {
		});
	};
	updates.match([&](const MTPDupdateShortSentMessage &data) {
		if (const auto media = data.vmedia()) {
			consumeMedia(*media);
		}
	}, [&](const MTPDupdates &data) {
		for (const auto &update : data.vupdates().v) {
			update.match([&](const MTPDupdateNewChannelMessage &messageUpdate) {
				messageUpdate.vmessage().match([&](const MTPDmessage &message) {
					if (const auto media = message.vmedia()) {
						consumeMedia(*media);
					}
				}, [](const auto &) {
				});
			}, [](const auto &) {
			});
		}
	}, [&](const MTPDupdatesCombined &data) {
		for (const auto &update : data.vupdates().v) {
			update.match([&](const MTPDupdateNewChannelMessage &messageUpdate) {
				messageUpdate.vmessage().match([&](const MTPDmessage &message) {
					if (const auto media = message.vmedia()) {
						consumeMedia(*media);
					}
				}, [](const auto &) {
				});
			}, [](const auto &) {
			});
		}
	}, [](const auto &) {
	});
}

void FinishUploadSendMedia(
		const std::shared_ptr<UploadState> &state,
		const MTPUpdates &updates) {
	state->session->api().applyUpdates(updates);

	auto ref = SyncDocumentRef();
	ExtractDocumentFromUpdates(updates, [&](const SyncDocumentRef &parsed) {
		ref = parsed;
	});
	if (!ref.id) {
		if (state->done) {
			state->done(false, {}, u"Не удалось получить документ синхронизации."_q);
		}
		return;
	}
	ref.sha256Hex = QString::fromLatin1(
		QCryptographicHash::hash(state->payload, QCryptographicHash::Sha256).toHex());
	if (state->done) {
		state->done(true, ref, {});
	}
}

void SendUploadedDocument(const std::shared_ptr<UploadState> &state) {
	const auto history = state->session->data().history(state->channel);
	const auto attributes = QVector<MTPDocumentAttribute>(
		1,
		MTP_documentAttributeFilename(MTP_string(state->fileName)));
	const auto media = MTP_inputMediaUploadedDocument(
		MTP_flags(MTPDinputMediaUploadedDocument::Flag::f_force_file),
		MTP_inputFile(
			MTP_long(state->fileId),
			MTP_int(state->partsCount),
			MTP_string(state->fileName),
			MTP_bytes(QByteArray())),
		MTPInputFile(),
		MTP_string("application/octet-stream"),
		MTP_vector<MTPDocumentAttribute>(attributes),
		MTP_vector<MTPInputDocument>(),
		MTPInputPhoto(),
		MTP_int(0),
		MTP_int(0));
	const auto randomId = base::RandomValue<uint64>();
	auto &histories = history->owner().histories();
	histories.sendPreparedMessage(
		history,
		FullReplyTo(),
		randomId,
		Data::Histories::PrepareMessage<MTPmessages_SendMedia>(
			MTP_flags(MTPmessages_SendMedia::Flag::f_silent),
			history->peer->input(),
			Data::Histories::ReplyToPlaceholder(),
			media,
			MTPstring(),
			MTP_long(randomId),
			MTPReplyMarkup(),
			MTP_vector<MTPMessageEntity>(),
			MTPint(),
			MTPint(),
			MTP_inputPeerEmpty(),
			MTPInputQuickReplyShortcut(),
			MTP_long(0),
			MTP_long(0),
			Api::SuggestToMTP({})),
		[=](const MTPUpdates &result, const MTP::Response &) {
			FinishUploadSendMedia(state, result);
		},
		[=](const MTP::Error &error, const MTP::Response &) {
			if (state->done) {
				state->done(false, {}, error.type());
			}
		});
}

void UploadNextPart(const std::shared_ptr<UploadState> &state) {
	if (state->nextPart >= state->partsCount) {
		SendUploadedDocument(state);
		return;
	}
	const auto offset = state->nextPart * kPartSize;
	const auto bytes = state->payload.mid(offset, kPartSize);
	const auto partIndex = state->nextPart++;
	const auto onDone = [=](const MTPBool &) {
		UploadNextPart(state);
	};
	const auto onFail = [=](const MTP::Error &error) {
		if (state->done) {
			state->done(false, {}, error.type());
		}
	};
	if (state->big) {
		state->session->api().request(MTPupload_SaveBigFilePart(
			MTP_long(state->fileId),
			MTP_int(partIndex),
			MTP_int(state->partsCount),
			MTP_bytes(bytes)
		)).done(onDone).fail(onFail).send();
	} else {
		state->session->api().request(MTPupload_SaveFilePart(
			MTP_long(state->fileId),
			MTP_int(partIndex),
			MTP_bytes(bytes)
		)).done(onDone).fail(onFail).send();
	}
}

void DownloadNextPart(const std::shared_ptr<DownloadState> &state) {
	if (state->offset >= state->ref.size) {
		if (state->done) {
			state->done(state->buffer, {});
		}
		return;
	}
	const auto limit = int(std::min<int64>(
		kDownloadPartSize,
		state->ref.size - state->offset));
	state->session->api().request(MTPupload_GetFile(
		MTP_flags(0),
		MTP_inputDocumentFileLocation(
			MTP_long(state->ref.id),
			MTP_long(state->ref.accessHash),
			MTP_bytes(state->ref.fileReference),
			MTP_string("")),
		MTP_long(state->offset),
		MTP_int(limit)
	)).done([=](const MTPupload_File &result) {
		result.match([&](const MTPDupload_file &data) {
			const auto bytes = data.vbytes().v;
			state->buffer.append(
				reinterpret_cast<const char*>(bytes.data()),
				bytes.size());
			state->offset += bytes.size();
			DownloadNextPart(state);
		}, [&](const auto &) {
			if (state->done) {
				state->done({}, u"Неверный ответ upload.getFile."_q);
			}
		});
	}).fail([=](const MTP::Error &error) {
		if (state->done) {
			state->done({}, error.type());
		}
	}).send();
}

} // namespace

void UploadEncryptedDocument(
		not_null<Main::Session*> session,
		not_null<ChannelData*> channel,
		const QByteArray &payload,
		const QString &fileName,
		Fn<void(bool ok, SyncDocumentRef ref, QString error)> done) {
	if (payload.isEmpty()) {
		if (done) {
			done(false, {}, u"Пустой файл синхронизации."_q);
		}
		return;
	}
	auto state = std::make_shared<UploadState>(UploadState{
		.session = session,
		.channel = channel,
		.payload = payload,
		.fileName = fileName,
		.done = std::move(done),
		.fileId = base::RandomValue<int64>(),
		.partsCount = std::max(1, (payload.size() + kPartSize - 1) / kPartSize),
		.big = (payload.size() > kUseBigFilesFrom),
	});
	UploadNextPart(state);
}

void DownloadEncryptedDocument(
		not_null<Main::Session*> session,
		const SyncDocumentRef &ref,
		Fn<void(QByteArray payload, QString error)> done) {
	if (!ref.id || !ref.size) {
		if (done) {
			done({}, u"Некорректная ссылка на документ."_q);
		}
		return;
	}
	auto state = std::make_shared<DownloadState>(DownloadState{
		.session = session,
		.ref = ref,
		.done = std::move(done),
	});
	state->buffer.reserve(int(ref.size));
	DownloadNextPart(state);
}

} // namespace TeleForge::Sync
