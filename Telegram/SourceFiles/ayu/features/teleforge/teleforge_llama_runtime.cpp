#include "ayu/features/teleforge/teleforge_llama_runtime.h"

#include "base/invoke_queued.h"

#include "logs.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonObject>

#include <cstring>
#include <mutex>
#include <thread>

#if defined(TELEFORGE_WITH_LLAMA_CPP)

#include "common.h"
#include "llama.h"

#include <algorithm>
#include <string>
#include <vector>

#endif // TELEFORGE_WITH_LLAMA_CPP

namespace TeleForge {
namespace {

#if defined(TELEFORGE_WITH_LLAMA_CPP)

struct LlamaSlot {
	QMutex mutex;
	QString path;
	llama_model *model = nullptr;
	llama_context *ctx = nullptr;
	bool embedMode = false;

	void unloadLocked() {
		if (ctx) {
			llama_free(ctx);
			ctx = nullptr;
		}
		if (model) {
			llama_model_free(model);
			model = nullptr;
		}
		path.clear();
	}

	[[nodiscard]] bool loadLocked(const QString &modelGgufPath, bool wantEmbeddings) {
		const auto clean = QDir::cleanPath(modelGgufPath.trimmed());
		if (clean.isEmpty() || !QFileInfo::exists(clean)) {
			return false;
		}
		if (model && ctx && path == clean && embedMode == wantEmbeddings) {
			return true;
		}
		unloadLocked();

		llama_model_params mp = llama_model_default_params();
		const auto ngl = qEnvironmentVariableIntValue("TELEFORGE_LLAMA_NGL");
		mp.n_gpu_layers = ngl >= 0 ? ngl : 99;

		const auto pUtf8 = clean.toUtf8();
		auto *m = llama_model_load_from_file(pUtf8.constData(), mp);
		if (!m) {
			LOG(("TeleForge: llama_model_load_from_file failed for %1").arg(clean));
			return false;
		}

		llama_context_params cp = llama_context_default_params();
		const auto n_ctx_train = uint32_t(std::max(
			512,
			std::min(int(llama_model_n_ctx_train(m)), 32 * 1024)));
		cp.n_ctx = n_ctx_train;
		cp.n_batch = std::min(cp.n_ctx, 4096u);
		cp.embeddings = wantEmbeddings;

		auto *c = llama_init_from_model(m, cp);
		if (!c) {
			LOG(("TeleForge: llama_init_from_model failed for %1").arg(clean));
			llama_model_free(m);
			return false;
		}
		model = m;
		ctx = c;
		path = clean;
		embedMode = wantEmbeddings;
		return true;
	}
};

LlamaSlot g_chat;
LlamaSlot g_embed;

std::once_flag g_backendOnce;

void BackendInitOnce() {
	std::call_once(g_backendOnce, [] {
		llama_log_set(
			[](enum ggml_log_level level, const char *text, void *) {
				if (level >= GGML_LOG_LEVEL_ERROR) {
					LOG(("llama: %1").arg(QString::fromUtf8(text)));
				}
			},
			nullptr);
		llama_backend_init();
		ggml_backend_load_all();
	});
}

[[nodiscard]] bool LoadChatSlot(const QString &path) {
	QMutexLocker lock(&g_chat.mutex);
	return g_chat.loadLocked(path, false);
}

[[nodiscard]] bool LoadEmbedSlot(const QString &path) {
	QMutexLocker lock(&g_embed.mutex);
	return g_embed.loadLocked(path, true);
}

#endif // TELEFORGE_WITH_LLAMA_CPP

} // namespace

void TeleForgeLlamaBackendInit() {
#if defined(TELEFORGE_WITH_LLAMA_CPP)
	BackendInitOnce();
#endif
}

void EnsureNativeChatLlamaLoadedAsync(
		const QString &modelGgufPath,
		Fn<void(bool ok, QString error)> onMainThread) {
	auto path = modelGgufPath;
	std::thread([path = std::move(path), cb = std::move(onMainThread)]() mutable {
		if (!cb) {
			return;
		}
#if !defined(TELEFORGE_WITH_LLAMA_CPP)
		const bool ok = false;
		const QString err = QStringLiteral("llama.cpp not built in");
#else
		BackendInitOnce();
		const bool ok = LoadChatSlot(path);
		const QString err = ok ? QString() : QStringLiteral("Failed to load chat model");
#endif
		const auto app = QCoreApplication::instance();
		if (!app) {
			return;
		}
		InvokeQueued(app, [ok, err, cb = std::move(cb)]() mutable {
			cb(ok, err);
		});
	}).detach();
}

void EnsureNativeEmbedLlamaLoadedAsync(
		const QString &modelGgufPath,
		Fn<void(bool ok, QString error)> onMainThread) {
	auto path = modelGgufPath;
	std::thread([path = std::move(path), cb = std::move(onMainThread)]() mutable {
		if (!cb) {
			return;
		}
#if !defined(TELEFORGE_WITH_LLAMA_CPP)
		const bool ok = false;
		const QString err = QStringLiteral("llama.cpp not built in");
#else
		BackendInitOnce();
		const bool ok = LoadEmbedSlot(path);
		const QString err = ok ? QString() : QStringLiteral("Failed to load embed/rerank model");
#endif
		const auto app = QCoreApplication::instance();
		if (!app) {
			return;
		}
		InvokeQueued(app, [ok, err, cb = std::move(cb)]() mutable {
			cb(ok, err);
		});
	}).detach();
}

void ShutdownNativeChatLlama() {
#if defined(TELEFORGE_WITH_LLAMA_CPP)
	QMutexLocker lock(&g_chat.mutex);
	g_chat.unloadLocked();
#endif
}

void ShutdownNativeEmbedLlama() {
#if defined(TELEFORGE_WITH_LLAMA_CPP)
	QMutexLocker lock(&g_embed.mutex);
	g_embed.unloadLocked();
#endif
}

bool NativeChatLlamaReady() {
#if defined(TELEFORGE_WITH_LLAMA_CPP)
	QMutexLocker lock(&g_chat.mutex);
	return g_chat.model && g_chat.ctx;
#else
	return false;
#endif
}

bool NativeEmbedLlamaReady() {
#if defined(TELEFORGE_WITH_LLAMA_CPP)
	QMutexLocker lock(&g_embed.mutex);
	return g_embed.model && g_embed.ctx;
#else
	return false;
#endif
}

bool NativeChatCompleteBlocking(
		const QJsonArray &messages,
		const double temperature,
		const int maxTokens,
		QString *outText,
		QString *outError) {
	if (outText) {
		outText->clear();
	}
	if (outError) {
		outError->clear();
	}
#if !defined(TELEFORGE_WITH_LLAMA_CPP)
	if (outError) {
		*outError = QStringLiteral("llama.cpp not built in");
	}
	return false;
#else
	QMutexLocker lock(&g_chat.mutex);
	if (!g_chat.model || !g_chat.ctx) {
		if (outError) {
			*outError = QStringLiteral("Chat model not loaded");
		}
		return false;
	}
	auto *model = g_chat.model;
	auto *ctx = g_chat.ctx;
	const auto *vocab = llama_model_get_vocab(model);

	llama_memory_clear(llama_get_memory(ctx), true);
	llama_set_embeddings(ctx, false);

	struct MsgBuf {
		std::string role;
		std::string content;
	};
	std::vector<MsgBuf> storage;
	std::vector<llama_chat_message> chat;
	storage.reserve(size_t(messages.size()));
	chat.reserve(size_t(messages.size()));
	for (const auto &v : messages) {
		const auto o = v.toObject();
		const auto role = o.value(QStringLiteral("role")).toString().toStdString();
		const auto content = o.value(QStringLiteral("content")).toString().toStdString();
		if (role.empty()) {
			continue;
		}
		storage.push_back({ role, content });
		chat.push_back({
			storage.back().role.c_str(),
			storage.back().content.c_str(),
		});
	}
	if (chat.empty()) {
		if (outError) {
			*outError = QStringLiteral("No messages");
		}
		return false;
	}

	const char *tmpl = llama_model_chat_template(model, /* name */ nullptr);
	int32_t needed = llama_chat_apply_template(
		tmpl,
		chat.data(),
		int32_t(chat.size()),
		true,
		nullptr,
		0);
	if (needed < 0) {
		if (outError) {
			*outError = QStringLiteral("chat template failed");
		}
		return false;
	}
	std::vector<char> formatted(size_t(needed) + 4096);
	needed = llama_chat_apply_template(
		tmpl,
		chat.data(),
		int32_t(chat.size()),
		true,
		formatted.data(),
		int32_t(formatted.size()));
	if (needed < 0) {
		if (outError) {
			*outError = QStringLiteral("chat template failed (2)");
		}
		return false;
	}
	const std::string prompt(formatted.data(), size_t(needed));

	int genLimit = maxTokens;
	if (genLimit <= 0) {
		genLimit = 512;
	}
	genLimit = std::min(genLimit, 8192);

	llama_sampler *smpl = llama_sampler_chain_init(llama_sampler_chain_default_params());
	llama_sampler_chain_add(smpl, llama_sampler_init_top_p(0.9f, 1));
	llama_sampler_chain_add(smpl, llama_sampler_init_temp(float(temperature)));
	llama_sampler_chain_add(smpl, llama_sampler_init_dist(LLAMA_DEFAULT_SEED));

	std::vector<llama_token> prompt_tokens = common_tokenize(
		vocab,
		prompt,
		true,
		true);
	if (prompt_tokens.empty()) {
		llama_sampler_free(smpl);
		if (outError) {
			*outError = QStringLiteral("tokenize failed");
		}
		return false;
	}

	QString response;
	llama_batch batch = llama_batch_get_one(prompt_tokens.data(), int32_t(prompt_tokens.size()));
	int n_generated = 0;
	while (true) {
		const int n_ctx = llama_n_ctx(ctx);
		const int n_ctx_used = llama_memory_seq_pos_max(llama_get_memory(ctx), 0) + 1;
		if (n_ctx_used + batch.n_tokens > n_ctx) {
			llama_sampler_free(smpl);
			if (outError) {
				*outError = QStringLiteral("context size exceeded");
			}
			return false;
		}
		if (llama_decode(ctx, batch) != 0) {
			llama_sampler_free(smpl);
			if (outError) {
				*outError = QStringLiteral("llama_decode failed");
			}
			return false;
		}
		llama_token new_token_id = llama_sampler_sample(smpl, ctx, -1);
		if (llama_vocab_is_eog(vocab, new_token_id)) {
			break;
		}
		++n_generated;
		char buf[256];
		const int n = llama_token_to_piece(
			vocab,
			new_token_id,
			buf,
			int32_t(sizeof(buf)),
			0,
			true);
		if (n < 0) {
			llama_sampler_free(smpl);
			if (outError) {
				*outError = QStringLiteral("token_to_piece failed");
			}
			return false;
		}
		response += QString::fromUtf8(buf, n);
		if (n_generated >= genLimit) {
			break;
		}
		batch = llama_batch_get_one(&new_token_id, 1);
	}
	llama_sampler_free(smpl);
	if (outText) {
		*outText = std::move(response);
	}
	return true;
#endif
}

std::optional<std::vector<float>> NativeEmbedOneBlocking(const QString &text) {
#if !defined(TELEFORGE_WITH_LLAMA_CPP)
	return std::nullopt;
#else
	QMutexLocker lock(&g_embed.mutex);
	if (!g_embed.model || !g_embed.ctx) {
		return std::nullopt;
	}
	auto *model = g_embed.model;
	auto *ctx = g_embed.ctx;
	const auto *vocab = llama_model_get_vocab(model);

	const auto utf8 = text.toUtf8();
	const auto tokens = common_tokenize(
		vocab,
		std::string(utf8.constData(), size_t(utf8.size())),
		true,
		false);
	if (tokens.empty()) {
		return std::nullopt;
	}

	llama_memory_clear(llama_get_memory(ctx), true);
	llama_set_embeddings(ctx, true);

	llama_batch batch = llama_batch_init(int32_t(tokens.size()), 0, 1);
	for (size_t i = 0; i < tokens.size(); ++i) {
		common_batch_add(batch, tokens[i], llama_pos(i), { 0 }, true);
	}
	if (llama_decode(ctx, batch) != 0) {
		llama_batch_free(batch);
		return std::nullopt;
	}
	const int32_t n_embd = llama_model_n_embd(model);
	const float *emb = llama_get_embeddings_seq(ctx, 0);
	if (!emb) {
		emb = llama_get_embeddings_ith(ctx, int32_t(tokens.size() - 1));
	}
	if (!emb) {
		llama_batch_free(batch);
		return std::nullopt;
	}
	std::vector<float> out(emb, emb + n_embd);
	llama_batch_free(batch);
	return out;
#endif
}

} // namespace TeleForge
