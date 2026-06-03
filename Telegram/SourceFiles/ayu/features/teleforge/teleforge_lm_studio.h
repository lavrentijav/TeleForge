#pragma once

#include <functional>

#include <QtCore/QJsonArray>
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
