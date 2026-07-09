#pragma once

#include <functional>

#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtCore/QString>
#include <QtNetwork/QNetworkAccessManager>

namespace TeleForge {

struct LmStudioRequestOptions {
	QString model;
	double temperature = 0.7;
	int maxTokens = 512;
};

class LmStudioBridge final {
public:
	using SuccessCallback = std::function<void(const QString &)>;
	using ErrorCallback = std::function<void(const QString &)>;
	// Receives the raw `choices[0].message` object (content + tool_calls).
	using RawSuccessCallback = std::function<void(const QJsonObject &)>;

	static LmStudioBridge &instance();

	void setBaseUrl(const QString &baseUrl);

	void setApiKey(const QString &apiKey);

	[[nodiscard]] QString baseUrl() const;

	void requestCompletion(
		const QString &systemPrompt,
		const QString &userPrompt,
		SuccessCallback onSuccess,
		ErrorCallback onError = {},
		const LmStudioRequestOptions &options = {});

	void requestChatCompletion(
		const QJsonArray &messages,
		SuccessCallback onSuccess,
		ErrorCallback onError = {},
		const LmStudioRequestOptions &options = {});

	// Like requestChatCompletion, but hands back the whole assistant message
	// (so callers can inspect tool_calls) and lets the caller supply the tools
	// array explicitly. When `tools` is empty no tools are advertised.
	void requestChatCompletionRaw(
		const QJsonArray &messages,
		const QJsonArray &tools,
		RawSuccessCallback onSuccess,
		ErrorCallback onError = {},
		const LmStudioRequestOptions &options = {});

	void setNativeChatEnabled(bool enabled);
	[[nodiscard]] bool nativeChatEnabled() const;

private:
	LmStudioBridge() = default;

	[[nodiscard]] QString chatCompletionsUrl() const;

	QString _baseUrl = QStringLiteral("http://127.0.0.1:1234");
	QString _apiKey;
	bool _nativeChat = false;
	QNetworkAccessManager _manager;
};

} // namespace TeleForge
