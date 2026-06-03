#include "ayu/features/teleforge/teleforge_embeddings.h"

#include "ayu/features/teleforge/teleforge_llama_runtime.h"

#include "logs.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <QtCore/QCryptographicHash>
#include <QtCore/QEventLoop>
#include <QtCore/QMutex>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QRegularExpression>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

namespace TeleForge {
namespace {

[[nodiscard]] QByteArray TokenFingerprint(const QString &token) {
	return QCryptographicHash::hash(token.toUtf8(), QCryptographicHash::Sha256);
}

void Normalize(std::vector<float> &values) {
	auto squared = 0.f;
	for (const auto value : values) {
		squared += value * value;
	}
	const auto length = std::sqrt(squared);
	if (length <= 0.000001f) {
		return;
	}
	for (auto &value : values) {
		value /= length;
	}
}

auto g_mutex = QMutex();
auto g_provider = std::shared_ptr<IEmbeddingProvider>(
	std::make_shared<LocalHashEmbeddingProvider>());

struct CacheEntry {
	EmbeddingVector vector;
	int lastUsed = 0;
};
auto g_cache = QHash<QByteArray, CacheEntry>();
auto g_cacheOrder = QList<QByteArray>();
constexpr auto kCacheCapacity = 256;
auto g_cacheTick = 0;

} // namespace

QString LocalHashEmbeddingProvider::modelId() const {
	return QStringLiteral("local-hash-embedding-v1");
}

HttpEmbeddingProvider::HttpEmbeddingProvider(
		QUrl embeddingsEndpoint,
		QString modelId,
		QString apiKey)
: _endpoint(std::move(embeddingsEndpoint))
, _modelId(std::move(modelId))
, _apiKey(std::move(apiKey).trimmed()) {
}

QString HttpEmbeddingProvider::modelId() const {
	if (!_modelId.isEmpty()) {
		return QStringLiteral("http-embedding:") + _modelId;
	}
	return QStringLiteral("http-embedding:") + _endpoint.host();
}

EmbeddingVector HttpEmbeddingProvider::embed(
		const QString &text,
		const EmbeddingOptions &options) {
	Q_UNUSED(options);
	auto payload = QJsonObject{
		{ QStringLiteral("input"), text },
	};
	if (!_modelId.isEmpty()) {
		payload.insert(QStringLiteral("model"), _modelId);
	}

	auto request = QNetworkRequest(_endpoint);
	request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
	if (!_apiKey.isEmpty()) {
		request.setRawHeader(
			"Authorization",
			QByteArrayLiteral("Bearer ") + _apiKey.toUtf8());
	}

	QNetworkAccessManager nam;
	auto reply = nam.post(
		request,
		QJsonDocument(payload).toJson(QJsonDocument::Compact));

	QEventLoop loop;
	QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
	loop.exec();

	if (reply->error() != QNetworkReply::NoError) {
		LOG(("TeleForge HttpEmbeddingProvider failed: %1").arg(reply->errorString()));
		reply->deleteLater();
		LocalHashEmbeddingProvider fallback;
		return fallback.embed(text, options);
	}

	const auto body = reply->readAll();
	reply->deleteLater();

	const auto root = QJsonDocument::fromJson(body).object();
	QJsonArray embeddingArray;
	const auto data = root.value(QStringLiteral("data")).toArray();
	if (!data.isEmpty()) {
		embeddingArray = data[0].toObject().value(QStringLiteral("embedding")).toArray();
	}
	if (embeddingArray.isEmpty()) {
		embeddingArray = root.value(QStringLiteral("embedding")).toArray();
	}
	if (embeddingArray.isEmpty()) {
		LOG(("TeleForge HttpEmbeddingProvider: empty embedding array"));
		LocalHashEmbeddingProvider fallback;
		return fallback.embed(text, options);
	}

	auto values = std::vector<float>();
	values.reserve(embeddingArray.size());
	for (const auto &v : embeddingArray) {
		values.push_back(static_cast<float>(v.toDouble()));
	}

	return {
		.modelId = modelId(),
		.values = std::move(values),
	};
}

NativeGgufEmbeddingProvider::NativeGgufEmbeddingProvider(QString modelId)
: _modelId(std::move(modelId)) {
}

QString NativeGgufEmbeddingProvider::modelId() const {
	if (!_modelId.isEmpty()) {
		return QStringLiteral("native-gguf:") + _modelId;
	}
	return QStringLiteral("native-gguf");
}

EmbeddingVector NativeGgufEmbeddingProvider::embed(
		const QString &text,
		const EmbeddingOptions &options) {
	Q_UNUSED(options);
	if (!NativeEmbedLlamaReady()) {
		LocalHashEmbeddingProvider fallback;
		return fallback.embed(text, options);
	}
	const auto vec = NativeEmbedOneBlocking(text);
	if (!vec || vec->empty()) {
		LocalHashEmbeddingProvider fallback;
		return fallback.embed(text, options);
	}
	return {
		.modelId = modelId(),
		.values = *vec,
	};
}

EmbeddingVector LocalHashEmbeddingProvider::embed(
		const QString &text,
		const EmbeddingOptions &options) {
	auto cleaned = text.toLower();
	cleaned.replace(QRegularExpression("\\s+"), " ");
	const auto tokens = cleaned.split(
		QRegularExpression("[^\\p{L}\\p{N}_-]+"),
		Qt::SkipEmptyParts);

	const auto dimensions = std::max(options.dimensions, 32);
	auto values = std::vector<float>(dimensions, 0.f);
	for (const auto &token : tokens) {
		const auto hash = TokenFingerprint(token);
		for (auto i = 0; i != 4; ++i) {
			const auto offset = i * 4;
			const auto raw = static_cast<quint32>(
				static_cast<unsigned char>(hash[offset + 0]) << 24
				| static_cast<unsigned char>(hash[offset + 1]) << 16
				| static_cast<unsigned char>(hash[offset + 2]) << 8
				| static_cast<unsigned char>(hash[offset + 3]));
			const auto index = static_cast<int>(raw % values.size());
			const auto sign = ((raw >> 31) & 1U) ? -1.f : 1.f;
			values[index] += sign;
		}
	}

	Normalize(values);
	return {
		.modelId = modelId(),
		.values = std::move(values),
	};
}

std::shared_ptr<IEmbeddingProvider> CurrentEmbeddingProvider() {
	QMutexLocker lock(&g_mutex);
	return g_provider;
}

void SetEmbeddingProvider(std::shared_ptr<IEmbeddingProvider> provider) {
	QMutexLocker lock(&g_mutex);
	g_provider = provider ? std::move(provider)
		: std::make_shared<LocalHashEmbeddingProvider>();
}

EmbeddingVector BuildLocalEmbedding(const QString &text, int dimensions) {
	const auto provider = CurrentEmbeddingProvider();
	Expects(provider != nullptr);
	const auto key = TokenFingerprint(text + provider->modelId());
	{
		QMutexLocker lock(&g_mutex);
		if (const auto it = g_cache.constFind(key); it != g_cache.cend()) {
			return it->vector;
		}
	}
	auto options = EmbeddingOptions{};
	options.dimensions = dimensions;
	const auto result = provider->embed(text, options);
	{
		QMutexLocker lock(&g_mutex);
		++g_cacheTick;
		if (g_cache.size() >= kCacheCapacity && !g_cacheOrder.isEmpty()) {
			g_cache.remove(g_cacheOrder.takeFirst());
		}
		g_cache.insert(key, { result, g_cacheTick });
		g_cacheOrder.push_back(key);
	}
	return result;
}

float CosineSimilarity(
		const std::vector<float> &left,
		const std::vector<float> &right) {
	if (left.empty() || right.empty() || left.size() != right.size()) {
		return 0.f;
	}
	auto dot = 0.f;
	auto leftSq = 0.f;
	auto rightSq = 0.f;
	for (auto i = 0U; i != left.size(); ++i) {
		dot += left[i] * right[i];
		leftSq += left[i] * left[i];
		rightSq += right[i] * right[i];
	}
	const auto denom = std::sqrt(leftSq) * std::sqrt(rightSq);
	return (denom <= 0.000001f) ? 0.f : (dot / denom);
}

std::vector<char> SerializeEmbedding(const std::vector<float> &values) {
	auto blob = std::vector<char>(values.size() * sizeof(float));
	if (!blob.empty()) {
		std::memcpy(blob.data(), values.data(), blob.size());
	}
	return blob;
}

std::vector<float> DeserializeEmbedding(const std::vector<char> &blob) {
	if (blob.empty() || (blob.size() % sizeof(float)) != 0) {
		return {};
	}
	auto values = std::vector<float>(blob.size() / sizeof(float));
	std::memcpy(values.data(), blob.data(), blob.size());
	return values;
}

} // namespace TeleForge
