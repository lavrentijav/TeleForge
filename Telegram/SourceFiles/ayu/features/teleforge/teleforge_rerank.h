#pragma once

#include <optional>
#include <vector>

#include <QtCore/QString>

namespace TeleForge {

/// Resolved path if `gte-multilingual-reranker-base-Q8_0.gguf` exists under known locations.
[[nodiscard]] QString DefaultTeleForgeRerankGgufPath();

enum class RerankApiKind {
	/// POST JSON `{ query, documents }` to `endpointUrl` (custom or compatible service).
	JsonBody,
	/// POST OpenAI-style `/v1/embeddings` on `endpointUrl` host (remote server).
	OpenAiEmbeddings,
	/// In-process llama.cpp embeddings on `modelPath` GGUF (no HTTP).
	NativeEmbeddings,
};

struct RerankRuntimeConfig {
	QString endpointUrl;
	QString modelId;
	QString modelPath;
	QString apiKey;
	RerankApiKind apiKind = RerankApiKind::JsonBody;
};

void SetRerankRuntimeConfig(RerankRuntimeConfig config);
[[nodiscard]] RerankRuntimeConfig CurrentRerankConfig();

/// POST JSON to endpointUrl; blocking (call off UI thread for large batches).
/// Request body: { "query", "documents": [...], "model"?, "model_path"? }
/// Response: { "scores": [float,...] } or { "results":[{ "index", "score"|"relevance_score" }] }
[[nodiscard]] std::optional<std::vector<float>> HttpRerankScores(
	const QString &query,
	const QStringList &documents,
	const RerankRuntimeConfig &config);

} // namespace TeleForge
