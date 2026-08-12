#pragma once

#include <optional>
#include <vector>

#include <QtCore/QDateTime>
#include <QtCore/QString>

namespace TeleForge {

struct PersonalityWeights {
	double aggression = 5.;
	double brevity = 5.;
	double emojis = 5.;
	double toxicity = 5.;
	double creativity = 5.;
};

struct PersonalityCore {
	PersonalityWeights weights;
	QString systemPrompt;
	QString sourceDevice;
	QDateTime updatedAt;
	/// Full URL, e.g. http://127.0.0.1:1234/v1/embeddings — empty = built-in hash or native GGUF embeddings.
	QString embeddingEndpointUrl;
	/// Optional model id for OpenAI-compatible /v1/embeddings (LM Studio, etc.).
	QString embeddingModelId;
	/// Base URL for chat completions, e.g. http://127.0.0.1:1234 — empty = default.
	/// Ignored when chatModelPath is set (in-process llama.cpp chat).
	QString lmStudioBaseUrl;
	/// Local chat *.gguf — loaded with llama.cpp in-process; leave empty for lmStudioBaseUrl only.
	QString chatModelPath;
	/// Optional `model` field for /v1/chat/completions (e.g. served id); empty = local-model.
	QString chatModelId;
	/// Bearer token for OpenAI-compatible APIs (optional).
	QString openAiApiKey;
	/// How many recent chat messages to pass into the model (CollectInferenceTurns).
	int chatContextMessages = 40;
	/// Reserved for Telegram sync-chat pipeline (UI toggle only for now).
	bool memorySyncEnabled = false;
	/// POST endpoint for memory reranking (JSON: query + documents). Empty = cosine only.
	QString rerankEndpointUrl;
	/// Optional rerank model id passed to the endpoint.
	QString rerankModelId;
	/// Path to reranker *.gguf — used by in-process llama.cpp embeddings when rerankEndpointUrl is empty.
	QString rerankModelPath;
	/// When true, tool-aware AI replies are sent to the chat automatically
	/// (ladder = several messages); when false they are inserted as a draft.
	bool autoSendEnabled = false;
	/// When true, recent images from the chat are attached to the request as
	/// image_url parts (requires a vision-capable model).
	bool visionEnabled = false;
	/// Cloud sync backend: 0 = Telegram sync chat, 1 = PostgreSQL, 2 = MySQL.
	int cloudBackend = 0;
	/// Connection string (key=value) shared by the PostgreSQL/MySQL backends.
	QString pgConnString;
	/// Informational PostgreSQL server version selected in the UI (e.g. "18").
	QString pgVersion = u"18"_q;
	/// Local embeddings *.gguf; empty = auto-detect the downloaded model in models/.
	QString embeddingModelPath;
	/// When true, PostgreSQL/MySQL connections go through a local `ssh -L`
	/// tunnel instead of connecting directly.
	bool sshTunnelEnabled = false;
	/// ssh login target for the tunnel, "user@host" or "user@host:port".
	QString sshTunnelTarget;
	/// Optional private key path for the tunnel; empty = ssh's own default.
	QString sshTunnelIdentityFile;
};

PersonalityCore DefaultPersonalityCore();
PersonalityCore BuildPersonalityCoreFromMessages(const std::vector<QString> &messages);

std::optional<PersonalityCore> LoadPersonalityCore();
void PersistPersonalityCore(const PersonalityCore &core);

QString SerializePersonalityCore(const PersonalityCore &core);
std::optional<PersonalityCore> ParsePersonalityCore(const QString &serialized);
bool ExportPersonalityCoreSnapshot(
	const PersonalityCore &core,
	const QString &path = QString());
QString DefaultSnapshotPath();

} // namespace TeleForge
