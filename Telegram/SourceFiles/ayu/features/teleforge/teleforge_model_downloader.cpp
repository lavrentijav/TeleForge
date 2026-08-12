#include "ayu/features/teleforge/teleforge_model_downloader.h"

#include "ayu/features/teleforge/teleforge_core.h"
#include "ayu/features/teleforge/teleforge_inference.h"
#include "ayu/features/teleforge/teleforge_paths.h"

#include "base/call_delayed.h"
#include "base/weak_qptr.h"
#include "boxes/abstract_box.h"
#include "core/application.h"
#include "core/core_settings.h"
#include "styles/style_layers.h"
#include "ui/boxes/confirm_box.h"
#include "ui/layers/generic_box.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/vertical_layout.h"

#include "logs.h"

#include <map>
#include <memory>
#include <vector>

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QPointer>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

namespace TeleForge {
namespace {

constexpr auto kDeclinedPrefKey = "teleforge_embedding_model_declined";
constexpr auto kPromptDelay = crl::time(6000);

struct ActiveDownload {
	ModelCatalogEntry entry;
	QPointer<QNetworkReply> reply;
	std::unique_ptr<QFile> file;
	std::vector<Fn<void(ModelDownloadState)>> listeners;
	qint64 offset = 0;
	qint64 received = 0;
	qint64 total = 0;
	bool headerChecked = false;
	bool cancelled = false;
};

auto g_downloads = std::map<QString, std::shared_ptr<ActiveDownload>>();

[[nodiscard]] QNetworkAccessManager &Manager() {
	static auto manager = QNetworkAccessManager();
	return manager;
}

[[nodiscard]] QString PartPath(const ModelCatalogEntry &entry) {
	return ModelLocalPath(entry) + QStringLiteral(".part");
}

[[nodiscard]] QString HumanSize(qint64 bytes) {
	if (bytes <= 0) {
		return QStringLiteral("—");
	} else if (bytes < 1024 * 1024) {
		return QString::number(bytes / 1024.) + QStringLiteral(" КБ");
	}
	return QString::number(bytes / (1024. * 1024.), 'f', 1)
		+ QStringLiteral(" МБ");
}

void Notify(const std::shared_ptr<ActiveDownload> &download, ModelDownloadState state) {
	const auto listeners = download->listeners;
	for (const auto &listener : listeners) {
		if (listener) {
			listener(state);
		}
	}
}

void Finish(
		const std::shared_ptr<ActiveDownload> &download,
		bool ok,
		const QString &error) {
	if (download->file) {
		download->file->close();
		download->file = nullptr;
	}
	const auto id = download->entry.id;
	if (ok) {
		const auto target = ModelLocalPath(download->entry);
		QFile::remove(target);
		if (!QFile::rename(PartPath(download->entry), target)) {
			g_downloads.erase(id);
			Notify(download, {
				.modelId = id,
				.received = download->received,
				.total = download->total,
				.finished = true,
				.ok = false,
				.error = QStringLiteral("Не удалось сохранить файл модели"),
			});
			return;
		}
	}
	g_downloads.erase(id);
	Notify(download, {
		.modelId = id,
		.received = download->received,
		.total = download->total,
		.finished = true,
		.ok = ok,
		.error = error,
	});
}

// The .part file is reused across launches, so a resumed request may be
// answered either with 206 (append at the byte we asked for) or with a plain
// 200 that restarts the whole body. Detecting that once, on the first header
// callback, is what keeps a resumed download from concatenating two copies of
// the model into the same file.
void HandleHeaders(const std::shared_ptr<ActiveDownload> &download) {
	if (download->headerChecked || !download->reply || !download->file) {
		return;
	}
	download->headerChecked = true;
	const auto status = download->reply->attribute(
		QNetworkRequest::HttpStatusCodeAttribute).toInt();
	if (download->offset > 0 && status != 206) {
		download->file->seek(0);
		download->file->resize(0);
		download->offset = 0;
		download->received = 0;
	}
	const auto length = download->reply->header(
		QNetworkRequest::ContentLengthHeader).toLongLong();
	download->total = (length > 0)
		? (length + download->offset)
		: download->entry.approxBytes;
}

} // namespace

const QVector<ModelCatalogEntry> &ModelCatalog() {
	static const auto kCatalog = QVector<ModelCatalogEntry>{
		{
			.id = u"bge-m3-q4"_q,
			.title = u"BGE-M3 · мультиязычные эмбеддинги (Q4_K_M)"_q,
			.fileName = u"bge-m3-Q4_K_M.gguf"_q,
			.url = u"https://huggingface.co/gpustack/bge-m3-GGUF/resolve/main/bge-m3-Q4_K_M.gguf?download=true"_q,
			.approxBytes = 366ll * 1024 * 1024,
			.embedding = true,
		},
		{
			.id = u"bge-m3-q8"_q,
			.title = u"BGE-M3 · мультиязычные эмбеддинги (Q8_0)"_q,
			.fileName = u"bge-m3-Q8_0.gguf"_q,
			.url = u"https://huggingface.co/gpustack/bge-m3-GGUF/resolve/main/bge-m3-Q8_0.gguf?download=true"_q,
			.approxBytes = 605ll * 1024 * 1024,
			.embedding = true,
		},
		{
			.id = u"gte-reranker-q8"_q,
			.title = u"GTE multilingual reranker (Q8_0)"_q,
			.fileName = u"gte-multilingual-reranker-base-Q8_0.gguf"_q,
			.url = u"https://huggingface.co/gpustack/gte-multilingual-reranker-base-GGUF/resolve/main/gte-multilingual-reranker-base-Q8_0.gguf?download=true"_q,
			.approxBytes = 321ll * 1024 * 1024,
			.embedding = false,
		},
	};
	return kCatalog;
}

const ModelCatalogEntry *FindModelInCatalog(const QString &id) {
	for (const auto &entry : ModelCatalog()) {
		if (entry.id == id) {
			return &entry;
		}
	}
	return nullptr;
}

ModelCatalogEntry DefaultEmbeddingModel() {
	for (const auto &entry : ModelCatalog()) {
		if (entry.embedding) {
			return entry;
		}
	}
	return {};
}

QString ModelLocalPath(const ModelCatalogEntry &entry) {
	if (entry.fileName.isEmpty()) {
		return {};
	}
	return QDir::cleanPath(
		TeleForgeModelsDirectory() + QLatin1Char('/') + entry.fileName);
}

bool ModelDownloaded(const ModelCatalogEntry &entry) {
	const auto path = ModelLocalPath(entry);
	return !path.isEmpty() && QFileInfo::exists(path);
}

QString DownloadedEmbeddingGgufPath() {
	for (const auto &entry : ModelCatalog()) {
		if (entry.embedding && ModelDownloaded(entry)) {
			return ModelLocalPath(entry);
		}
	}
	return {};
}

QString DefaultTeleForgeEmbeddingGgufPath() {
	const auto core = LoadPersonalityCore().value_or(DefaultPersonalityCore());
	const auto stored = core.embeddingModelPath.trimmed();
	if (!stored.isEmpty() && QFileInfo::exists(stored)) {
		return QDir::cleanPath(stored);
	}
	return DownloadedEmbeddingGgufPath();
}

bool ModelDownloadRunning(const QString &modelId) {
	return g_downloads.find(modelId) != g_downloads.end();
}

void StartModelDownload(
		ModelCatalogEntry entry,
		Fn<void(ModelDownloadState)> onProgress) {
	if (entry.url.isEmpty() || entry.fileName.isEmpty()) {
		if (onProgress) {
			onProgress({
				.modelId = entry.id,
				.finished = true,
				.ok = false,
				.error = QStringLiteral("Модель не задана"),
			});
		}
		return;
	}
	if (const auto i = g_downloads.find(entry.id); i != g_downloads.end()) {
		if (onProgress) {
			i->second->listeners.push_back(onProgress);
			onProgress({
				.modelId = entry.id,
				.received = i->second->received,
				.total = i->second->total,
			});
		}
		return;
	}

	TeleForgeEnsureModelsDirectoryExists();

	auto download = std::make_shared<ActiveDownload>();
	download->entry = entry;
	if (onProgress) {
		download->listeners.push_back(onProgress);
	}

	download->file = std::make_unique<QFile>(PartPath(entry));
	if (!download->file->open(QIODevice::ReadWrite)) {
		if (onProgress) {
			onProgress({
				.modelId = entry.id,
				.finished = true,
				.ok = false,
				.error = QStringLiteral("Нет доступа к каталогу models"),
			});
		}
		return;
	}
	download->offset = download->file->size();
	download->received = download->offset;
	download->file->seek(download->offset);
	download->total = entry.approxBytes;

	auto request = QNetworkRequest(QUrl(entry.url));
	request.setAttribute(
		QNetworkRequest::RedirectPolicyAttribute,
		QNetworkRequest::NoLessSafeRedirectPolicy);
	request.setRawHeader("User-Agent", "TeleForge");
	if (download->offset > 0) {
		request.setRawHeader(
			"Range",
			"bytes=" + QByteArray::number(download->offset) + "-");
	}

	const auto reply = Manager().get(request);
	download->reply = reply;
	g_downloads.emplace(entry.id, download);

	QObject::connect(reply, &QNetworkReply::metaDataChanged, reply, [=] {
		HandleHeaders(download);
	});
	QObject::connect(reply, &QNetworkReply::readyRead, reply, [=] {
		HandleHeaders(download);
		if (download->cancelled || !download->file) {
			return;
		}
		const auto chunk = reply->readAll();
		if (chunk.isEmpty()) {
			return;
		}
		if (download->file->write(chunk) != chunk.size()) {
			download->cancelled = true;
			reply->abort();
			return;
		}
		download->received += chunk.size();
		Notify(download, {
			.modelId = entry.id,
			.received = download->received,
			.total = download->total,
		});
	});
	QObject::connect(reply, &QNetworkReply::finished, reply, [=] {
		reply->deleteLater();
		if (download->cancelled) {
			Finish(download, false, QStringLiteral("Загрузка отменена"));
			return;
		}
		if (reply->error() != QNetworkReply::NoError) {
			Finish(download, false, reply->errorString());
			return;
		}
		HandleHeaders(download);
		if (download->file) {
			download->file->write(reply->readAll());
			download->file->flush();
		}
		Finish(download, true, QString());
	});
}

void CancelModelDownload(const QString &modelId) {
	const auto i = g_downloads.find(modelId);
	if (i == g_downloads.end()) {
		return;
	}
	i->second->cancelled = true;
	if (i->second->reply) {
		i->second->reply->abort();
	}
}

void ShowModelDownloadBox(ModelCatalogEntry entry) {
	Ui::show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(entry.title));

		auto text = box->lifetime().make_state<rpl::variable<QString>>(
			QStringLiteral("Подготовка…"));
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			text->value(),
			st::boxLabel));

		const auto weak = base::make_weak(box.get());
		StartModelDownload(entry, [=](ModelDownloadState state) {
			if (!weak) {
				return;
			}
			if (state.finished) {
				if (state.ok) {
					text->force_assign(
						QStringLiteral("Модель загружена. Применяю…"));
					ApplyEndpointsFromStorage();
					weak->closeBox();
				} else {
					text->force_assign(
						QStringLiteral("Ошибка: ") + state.error);
				}
				return;
			}
			const auto percent = (state.total > 0)
				? QString::number(state.received * 100 / state.total)
					+ QStringLiteral("% · ")
				: QString();
			text->force_assign(percent
				+ HumanSize(state.received)
				+ QStringLiteral(" из ")
				+ HumanSize(state.total));
		});

		box->addButton(rpl::single(QStringLiteral("Свернуть")), [=] {
			box->closeBox();
		});
		box->addLeftButton(rpl::single(QStringLiteral("Отменить")), [=] {
			CancelModelDownload(entry.id);
			box->closeBox();
		});
	}));
}

void EnsureLocalEmbeddingModelReady() {
#ifdef TELEFORGE_WITH_LLAMA_CPP
	static auto asked = false;
	if (asked) {
		return;
	}
	asked = true;

	base::call_delayed(kPromptDelay, [] {
		if (!DefaultTeleForgeEmbeddingGgufPath().isEmpty()) {
			LOG(("TeleForge: local embedding model present, no download needed"));
			return;
		}
		if (Core::App().settings().readPref<bool>(kDeclinedPrefKey, false)) {
			LOG(("TeleForge: embedding model download declined earlier"));
			return;
		}
		const auto entry = DefaultEmbeddingModel();
		if (entry.url.isEmpty() || !Core::App().maybePrimarySession()) {
			return;
		}
		Ui::show(Ui::MakeConfirmBox({
			.text = QStringLiteral(
				"Локальная модель эмбеддингов не найдена в каталоге models.\n\n"
				"Скачать %1 (~%2) с Hugging Face? Без неё память и поиск "
				"по фактам работают на упрощённом хеш-эмбеддинге."
			).arg(entry.fileName, HumanSize(entry.approxBytes)),
			.confirmed = [=](Fn<void()> &&close) {
				close();
				ShowModelDownloadBox(entry);
			},
			.cancelled = [=](Fn<void()> &&close) {
				Core::App().settings().writePref<bool>(kDeclinedPrefKey, true);
				close();
			},
			.confirmText = QStringLiteral("Скачать"),
			.cancelText = QStringLiteral("Не сейчас"),
			.title = QStringLiteral("TeleForge · модель эмбеддингов"),
			.strictCancel = true,
		}));
	});
#endif // TELEFORGE_WITH_LLAMA_CPP
}

} // namespace TeleForge
