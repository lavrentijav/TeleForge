#pragma once

#include <memory>
#include <vector>

#include <QtCore/QString>
#include <QtCore/QUrl>

namespace TeleForge {

struct EmbeddingVector {
	QString modelId;
	std::vector<float> values;
};

struct EmbeddingOptions {
	int dimensions = 192;
};

class IEmbeddingProvider {
public:
	virtual ~IEmbeddingProvider() = default;

	[[nodiscard]] virtual EmbeddingVector embed(
		const QString &text,
		const EmbeddingOptions &options = {}) = 0;
	[[nodiscard]] virtual QString modelId() const = 0;
};

class LocalHashEmbeddingProvider final : public IEmbeddingProvider {
public:
	[[nodiscard]] EmbeddingVector embed(
		const QString &text,
		const EmbeddingOptions &options = {}) override;
	[[nodiscard]] QString modelId() const override;
};

/// OpenAI-compatible POST (input + optional model). Blocking; call off the UI thread when possible.
class HttpEmbeddingProvider final : public IEmbeddingProvider {
public:
	HttpEmbeddingProvider(
		QUrl embeddingsEndpoint,
		QString modelId = {},
		QString apiKey = {});

	[[nodiscard]] EmbeddingVector embed(
		const QString &text,
		const EmbeddingOptions &options = {}) override;
	[[nodiscard]] QString modelId() const override;

private:
	QUrl _endpoint;
	QString _modelId;
	QString _apiKey;
};

/// In-process llama.cpp embeddings on the loaded rerank/embed GGUF. Blocking.
class NativeGgufEmbeddingProvider final : public IEmbeddingProvider {
public:
	explicit NativeGgufEmbeddingProvider(QString modelId = {});

	[[nodiscard]] EmbeddingVector embed(
		const QString &text,
		const EmbeddingOptions &options = {}) override;
	[[nodiscard]] QString modelId() const override;

private:
	QString _modelId;
};

[[nodiscard]] std::shared_ptr<IEmbeddingProvider> CurrentEmbeddingProvider();
void SetEmbeddingProvider(std::shared_ptr<IEmbeddingProvider> provider);

[[nodiscard]] EmbeddingVector BuildLocalEmbedding(
	const QString &text,
	int dimensions = 192);

[[nodiscard]] float CosineSimilarity(
	const std::vector<float> &left,
	const std::vector<float> &right);
[[nodiscard]] std::vector<char> SerializeEmbedding(
	const std::vector<float> &values);
[[nodiscard]] std::vector<float> DeserializeEmbedding(
	const std::vector<char> &blob);

} // namespace TeleForge
