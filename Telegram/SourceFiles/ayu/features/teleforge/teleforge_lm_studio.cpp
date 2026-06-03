#include "ayu/features/teleforge/teleforge_lm_studio.h"

#include "ayu/features/plugins/plugin_tool_registry.h"
#include "ayu/features/teleforge/teleforge_llama_runtime.h"

#include "base/invoke_queued.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

#include <algorithm>
#include <thread>

namespace TeleForge {
namespace {

[[nodiscard]] QString NormalizeBaseUrl(QString url) {
	url = url.trimmed();
	while (url.endsWith('/')) {
		url.chop(1);
	}
	return url;
}

} // namespace

LmStudioBridge &LmStudioBridge::instance() {
	static auto bridge = LmStudioBridge();
	return bridge;
}

void LmStudioBridge::setBaseUrl(const QString &baseUrl) {
	const auto normalized = NormalizeBaseUrl(baseUrl);
	_baseUrl = normalized.isEmpty()
		? QStringLiteral("http://127.0.0.1:1234")
		: normalized;
}

void LmStudioBridge::setApiKey(const QString &apiKey) {
	_apiKey = apiKey.trimmed();
}

QString LmStudioBridge::baseUrl() const {
	return _baseUrl;
}

void LmStudioBridge::setNativeChatEnabled(const bool enabled) {
	_nativeChat = enabled;
}

bool LmStudioBridge::nativeChatEnabled() const {
	return _nativeChat;
}

QString LmStudioBridge::chatCompletionsUrl() const {
	return _baseUrl + QStringLiteral("/v1/chat/completions");
}

void LmStudioBridge::requestChatCompletion(
		const QJsonArray &messages,
		SuccessCallback onSuccess,
		ErrorCallback onError,
		const LmStudioRequestOptions &options) {
	if (_nativeChat) {
		const auto msgs = messages;
		const auto opt = options;
		const auto succ = std::move(onSuccess);
		const auto err = std::move(onError);
		std::thread([msgs, opt, succ, err]() mutable {
			QString outText;
			QString outErr;
			auto mt = opt.maxTokens;
			if (mt <= 0) {
				mt = 512;
			}
			mt = std::min(mt, 8192);
			const bool ok = NativeChatCompleteBlocking(
				msgs,
				opt.temperature,
				mt,
				&outText,
				&outErr);
			const auto app = QCoreApplication::instance();
			if (!app) {
				return;
			}
			InvokeQueued(app, [=]() mutable {
				if (!ok) {
					if (err) {
						err(outErr.isEmpty()
							? QStringLiteral("Native chat failed.")
							: outErr);
					}
					return;
				}
				if (succ) {
					succ(outText);
				}
			});
		}).detach();
		return;
	}

	auto payload = QJsonObject{
		{ "model", options.model.isEmpty() ? QString("local-model") : options.model },
		{ "temperature", options.temperature },
		{ "max_tokens", options.maxTokens },
		{ "messages", messages },
	};
	const auto tools = Plugins::PluginToolsForInference();
	if (!tools.isEmpty()) {
		payload.insert(QStringLiteral("tools"), tools);
	}

	auto request = QNetworkRequest(QUrl(chatCompletionsUrl()));
	request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
	if (!_apiKey.isEmpty()) {
		request.setRawHeader(
			"Authorization",
			QByteArrayLiteral("Bearer ") + _apiKey.toUtf8());
	}
	const auto reply = _manager.post(
		request,
		QJsonDocument(payload).toJson(QJsonDocument::Compact));

	QObject::connect(reply, &QNetworkReply::finished, [
			reply,
			onSuccess = std::move(onSuccess),
			onError = std::move(onError)] {
		if (reply->error() != QNetworkReply::NoError) {
			if (onError) {
				onError(reply->errorString());
			}
			reply->deleteLater();
			return;
		}

		const auto document = QJsonDocument::fromJson(reply->readAll());
		const auto root = document.object();
		const auto choices = root.value("choices").toArray();
		if (choices.isEmpty()) {
			if (onError) {
				onError("LM Studio returned an empty choices array.");
			}
			reply->deleteLater();
			return;
		}

		const auto content = choices[0].toObject()
			.value("message").toObject()
			.value("content").toString();
		if (content.isEmpty()) {
			if (onError) {
				onError("LM Studio returned an empty content field.");
			}
			reply->deleteLater();
			return;
		}

		if (onSuccess) {
			onSuccess(content);
		}
		reply->deleteLater();
	});
}

void LmStudioBridge::requestCompletion(
		const QString &systemPrompt,
		const QString &userPrompt,
		SuccessCallback onSuccess,
		ErrorCallback onError,
		const LmStudioRequestOptions &options) {
	auto messages = QJsonArray();
	messages.push_back(QJsonObject{
		{ QStringLiteral("role"), QStringLiteral("system") },
		{ QStringLiteral("content"), systemPrompt },
	});
	messages.push_back(QJsonObject{
		{ QStringLiteral("role"), QStringLiteral("user") },
		{ QStringLiteral("content"), userPrompt },
	});
	requestChatCompletion(
		messages,
		std::move(onSuccess),
		std::move(onError),
		options);
}

} // namespace TeleForge
