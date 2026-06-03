#include "ayu/features/teleforge/teleforge_inference.h"

#include "ayu/features/teleforge/teleforge_embeddings.h"
#include "ayu/features/teleforge/teleforge_llama_runtime.h"
#include "ayu/features/teleforge/teleforge_memory.h"
#include "ayu/features/teleforge/teleforge_prompt_builder.h"
#include "ayu/features/teleforge/teleforge_rerank.h"

#include "logs.h"

#include <crl/crl.h>

#include <unordered_set>

#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtCore/QRegularExpression>
#include <QtCore/QUrl>

namespace TeleForge {
namespace {

constexpr auto kFactSplitShortMessageMaxChars = 100;

[[nodiscard]] QStringList ParseModelOutputToFacts(const QString &raw) {
	auto out = QStringList();
	static const auto numbering = QRegularExpression(
		QStringLiteral("^\\s*\\d+\\s*[.)\\]]\\s*"));
	for (const auto &line : raw.split(QRegularExpression(QStringLiteral("[\\r\\n]+")))) {
		auto t = line.trimmed();
		if (t.isEmpty()) {
			continue;
		}
		if (t.startsWith(QStringLiteral("```"))) {
			continue;
		}
		t.remove(numbering);
		t = t.trimmed();
		if (!t.isEmpty()
			&& (t.startsWith(u'-') || t.startsWith(u'•') || t.startsWith('*'))) {
			t = t.mid(1).trimmed();
		}
		if (t.size() >= 4) {
			out.push_back(t);
		}
	}
	return out;
}

} // namespace

void ApplyPersonalityEndpoints(const PersonalityCore &core) {
	const auto embeddingEndpoint = core.embeddingEndpointUrl.trimmed();
	const auto embeddingModelId = core.embeddingModelId.trimmed();
	const auto embeddingEndpointSet = !embeddingEndpoint.isEmpty();
	if (embeddingEndpointSet) {
		const auto url = QUrl(embeddingEndpoint);
		if (url.isValid() && !url.scheme().isEmpty()) {
			SetEmbeddingProvider(std::make_shared<HttpEmbeddingProvider>(
				url,
				embeddingModelId,
				core.openAiApiKey.trimmed()));
		} else {
			SetEmbeddingProvider({});
		}
	}

	const auto fallbackChatUrl = core.lmStudioBaseUrl.trimmed().isEmpty()
		? QStringLiteral("http://127.0.0.1:1234")
		: core.lmStudioBaseUrl.trimmed();

	const auto chatPath = core.chatModelPath.trimmed();
	const auto chatLocalOk = !chatPath.isEmpty() && QFileInfo::exists(chatPath);
	if (chatLocalOk) {
		LmStudioBridge::instance().setNativeChatEnabled(false);
		LmStudioBridge::instance().setBaseUrl(fallbackChatUrl);
		LmStudioBridge::instance().setApiKey(core.openAiApiKey.trimmed());
		const auto pathCopy = chatPath;
		const auto apiKeyCopy = core.openAiApiKey.trimmed();
		LOG(("TeleForge: native chat model load on worker thread"));
		EnsureNativeChatLlamaLoadedAsync(pathCopy, [=](bool ok, const QString &error) {
			if (ok) {
				LmStudioBridge::instance().setNativeChatEnabled(true);
			} else {
				LmStudioBridge::instance().setNativeChatEnabled(false);
				LmStudioBridge::instance().setBaseUrl(fallbackChatUrl);
				LmStudioBridge::instance().setApiKey(apiKeyCopy);
				LOG(("TeleForge native chat load failed: %1").arg(error));
			}
		});
	} else {
		ShutdownNativeChatLlama();
		LmStudioBridge::instance().setNativeChatEnabled(false);
		LmStudioBridge::instance().setBaseUrl(fallbackChatUrl);
		LmStudioBridge::instance().setApiKey(core.openAiApiKey.trimmed());
	}

	auto rerankPath = core.rerankModelPath.trimmed();
	if (rerankPath.isEmpty()) {
		rerankPath = DefaultTeleForgeRerankGgufPath();
	}
	const auto rerankEndpoint = core.rerankEndpointUrl.trimmed();
	const auto apiKey = core.openAiApiKey.trimmed();

	if (!rerankEndpoint.isEmpty()) {
		ShutdownNativeEmbedLlama();
		if (!embeddingEndpointSet) {
			SetEmbeddingProvider({});
		}
		const auto url = QUrl(rerankEndpoint);
		const auto useOpenAiEmbeddings = url.path().contains(
			QStringLiteral("embeddings"),
			Qt::CaseInsensitive);
		SetRerankRuntimeConfig({
			.endpointUrl = rerankEndpoint,
			.modelId = core.rerankModelId.trimmed(),
			.modelPath = rerankPath,
			.apiKey = apiKey,
			.apiKind = useOpenAiEmbeddings
				? RerankApiKind::OpenAiEmbeddings
				: RerankApiKind::JsonBody,
		});
	} else if (embeddingEndpointSet) {
		ShutdownNativeEmbedLlama();
		SetRerankRuntimeConfig({
			.endpointUrl = embeddingEndpoint,
			.modelId = core.rerankModelId.trimmed().isEmpty()
				? embeddingModelId
				: core.rerankModelId.trimmed(),
			.modelPath = rerankPath,
			.apiKey = apiKey,
			.apiKind = RerankApiKind::OpenAiEmbeddings,
		});
	} else if (!rerankPath.isEmpty() && QFileInfo::exists(rerankPath)) {
		const auto modelIdCopy = core.rerankModelId.trimmed();
		const auto pathCopy = rerankPath;
		LOG(("TeleForge: native embed/rerank model load on worker thread"));
		EnsureNativeEmbedLlamaLoadedAsync(pathCopy, [=](bool ok, const QString &error) {
			if (ok) {
				SetRerankRuntimeConfig({
					.endpointUrl = {},
					.modelId = modelIdCopy,
					.modelPath = pathCopy,
					.apiKey = apiKey,
					.apiKind = RerankApiKind::NativeEmbeddings,
				});
				if (!embeddingEndpointSet) {
					SetEmbeddingProvider(
						std::make_shared<NativeGgufEmbeddingProvider>(embeddingModelId));
				}
			} else {
				LOG(("TeleForge native embed load failed: %1").arg(error));
				ShutdownNativeEmbedLlama();
				SetRerankRuntimeConfig({
					.endpointUrl = {},
					.modelId = modelIdCopy,
					.modelPath = pathCopy,
					.apiKey = apiKey,
					.apiKind = RerankApiKind::JsonBody,
				});
				if (!embeddingEndpointSet) {
					SetEmbeddingProvider({});
				}
			}
		});
	} else {
		ShutdownNativeEmbedLlama();
		if (!embeddingEndpointSet) {
			SetEmbeddingProvider({});
		}
		SetRerankRuntimeConfig({
			.endpointUrl = {},
			.modelId = core.rerankModelId.trimmed(),
			.modelPath = rerankPath,
			.apiKey = apiKey,
			.apiKind = RerankApiKind::JsonBody,
		});
	}
}

void ApplyEndpointsFromStorage() {
	LOG(("TeleForge: ApplyEndpointsFromStorage — begin"));
	const auto core = LoadPersonalityCore().value_or(DefaultPersonalityCore());
	ApplyPersonalityEndpoints(core);
	LOG(("TeleForge: ApplyEndpointsFromStorage — end (native llama loads may continue async)"));
}

void RequestMemoryFactSplit(
		QString messageText,
		Fn<void(QStringList facts)> onFacts,
		Fn<void()> onFallback) {
	const auto trimmed = messageText.trimmed();
	if (trimmed.isEmpty()) {
		if (onFallback) {
			onFallback();
		}
		return;
	}
	if (trimmed.size() <= kFactSplitShortMessageMaxChars) {
		if (onFacts) {
			onFacts(QStringList{ trimmed });
		}
		return;
	}
	const auto personality = LoadPersonalityCore().value_or(DefaultPersonalityCore());
	const auto system = QStringLiteral(
		"You split a chat message into atomic factual statements for a memory database.\n"
		"Rules: exactly ONE self-contained fact per output line; no numbering, bullets, markdown, or labels; "
		"same language as the message (Russian, English, etc.); skip small talk and questions without factual content; "
		"if there is a single fact, output exactly one line.");
	LmStudioRequestOptions opt;
	opt.model = personality.chatModelId.trimmed();
	opt.temperature = 0.;
	opt.maxTokens = 768;
	LmStudioBridge::instance().requestCompletion(
		system,
		trimmed,
		[onFacts = std::move(onFacts), onFallback = std::move(onFallback)](
				const QString &reply) {
			const auto facts = ParseModelOutputToFacts(reply);
			if (facts.isEmpty()) {
				if (onFallback) {
					onFallback();
				}
				return;
			}
			if (onFacts) {
				onFacts(facts);
			}
		},
		[onFallback = std::move(onFallback)](const QString &) {
			if (onFallback) {
				onFallback();
			}
		},
		opt);
}

PreparedChatCompletion PrepareTeleForgeCompletion(const InferenceParams &params) {
	const auto personality = LoadPersonalityCore().value_or(DefaultPersonalityCore());
	const auto settings = LoadMemorySettings(params.peerId);

	auto uniqueIds = std::vector<long long>();
	{
		auto seen = std::unordered_set<long long>();
		for (const auto &t : params.chronologicalTurns) {
			if (!t.senderUserId) {
				continue;
			}
			if (seen.insert(t.senderUserId).second) {
				uniqueIds.push_back(t.senderUserId);
			}
		}
	}

	auto retrieval = RetrievalRequest{
		.chatId = params.peerId,
		.queryText = params.retrievalQuery,
		.uniqueUserIds = std::move(uniqueIds),
		.settings = settings,
	};
	const auto memory = BuildMemoryContext(retrieval);

	auto turns = std::vector<Prompt::ContextTurn>();
	turns.reserve(params.chronologicalTurns.size());
	for (const auto &t : params.chronologicalTurns) {
		turns.push_back({
			t.role,
			t.content,
			t.senderUserId,
		});
	}

	auto assembly = Prompt::AssemblyOptions{};
	assembly.baseSystemPrompt = personality.systemPrompt;
	assembly.maxMemoryBlockChars = settings.maxMemoryBlockChars;
	const auto built = Prompt::BuildOpenAiStyleMessages(memory, turns, assembly);

	auto messages = QJsonArray();
	for (const auto &[role, content] : built.messages) {
		messages.push_back(QJsonObject{
			{ QStringLiteral("role"), role },
			{ QStringLiteral("content"), content },
		});
	}

	auto lmOptions = params.lmOptions;
	if (lmOptions.model.isEmpty()
		&& !personality.chatModelId.trimmed().isEmpty()) {
		lmOptions.model = personality.chatModelId.trimmed();
	}

	return {
		.messages = std::move(messages),
		.lmOptions = std::move(lmOptions),
	};
}

void RequestTeleForgeCompletion(
		const InferenceParams &params,
		LmStudioBridge::SuccessCallback onSuccess,
		LmStudioBridge::ErrorCallback onError) {
	const auto prepared = PrepareTeleForgeCompletion(params);
	LmStudioBridge::instance().requestChatCompletion(
		prepared.messages,
		std::move(onSuccess),
		std::move(onError),
		prepared.lmOptions);
}

void RequestTeleForgeCompletionAsync(
		InferenceParams params,
		LmStudioBridge::SuccessCallback onSuccess,
		LmStudioBridge::ErrorCallback onError) {
	// teleforge.db is main-thread only; HTTP rerank runs on a worker thread.
	crl::on_main([
			params = std::move(params),
			onSuccess = std::move(onSuccess),
			onError = std::move(onError)]() mutable {
		auto prepared = PrepareTeleForgeCompletion(params);
		const auto messages = prepared.messages;
		const auto lmOptions = prepared.lmOptions;
		LmStudioBridge::instance().requestChatCompletion(
			messages,
			std::move(onSuccess),
			std::move(onError),
			lmOptions);
	});
}

} // namespace TeleForge
