#pragma once

#include "base/basic_types.h"

#include <QtCore/QString>
#include <QtCore/QStringList>

namespace TeleForge {

[[nodiscard]] QString NormalizeOpenAiBaseUrl(QString urlOrBase);

[[nodiscard]] QString OpenAiOriginBaseForModelsList(const QString &urlOrBase);

[[nodiscard]] QString EmbeddingsUrlFromChatBase(const QString &urlOrBase);

struct ParsedOpenAiEndpoint {
	QString url;
	QString modelId;
};

[[nodiscard]] ParsedOpenAiEndpoint ParseOpenAiEndpointUrl(
	const QString &urlOrBase,
	const QString &explicitModelId = {});

void FetchOpenAiModelIdsAsync(
	const QString &baseUrl,
	Fn<void(QStringList ids, QString error)> onMainThread,
	const QString &authorizationBearer = {});

} // namespace TeleForge
