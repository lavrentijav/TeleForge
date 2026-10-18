#pragma once

#include "base/basic_types.h"

#include <QtCore/QString>
#include <QtCore/QVector>

namespace TeleForge {

struct ModelCatalogEntry {
	QString id;
	QString title;
	QString fileName;
	QString url;
	qint64 approxBytes = 0;
	bool embedding = false;
};

[[nodiscard]] const QVector<ModelCatalogEntry> &ModelCatalog();
[[nodiscard]] const ModelCatalogEntry *FindModelInCatalog(const QString &id);
[[nodiscard]] ModelCatalogEntry DefaultEmbeddingModel();

[[nodiscard]] QString ModelLocalPath(const ModelCatalogEntry &entry);
[[nodiscard]] bool ModelDownloaded(const ModelCatalogEntry &entry);

/// First catalog embedding model already present in models/, or empty.
[[nodiscard]] QString DownloadedEmbeddingGgufPath();

/// Local GGUF used for in-process embeddings: the stored PersonalityCore path
/// when it exists, otherwise the default catalog file inside models/.
[[nodiscard]] QString DefaultTeleForgeEmbeddingGgufPath();

struct ModelDownloadState {
	QString modelId;
	qint64 received = 0;
	qint64 total = 0;
	bool finished = false;
	bool ok = false;
	QString error;
};

[[nodiscard]] bool ModelDownloadRunning(const QString &modelId);
void StartModelDownload(
	ModelCatalogEntry entry,
	Fn<void(ModelDownloadState)> onProgress);
void CancelModelDownload(const QString &modelId);

/// Shows a progress box for a running (or freshly started) download.
void ShowModelDownloadBox(ModelCatalogEntry entry);

/// Asks the user once per run whether to fetch the missing embedding model,
/// then downloads it and re-applies the inference endpoints.
void EnsureLocalEmbeddingModelReady();

} // namespace TeleForge
