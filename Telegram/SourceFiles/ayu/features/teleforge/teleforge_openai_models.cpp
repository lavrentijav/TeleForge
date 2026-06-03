#include "ayu/features/teleforge/teleforge_openai_models.h"

#include "base/invoke_queued.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QEventLoop>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QUrl>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

#include <thread>

namespace TeleForge {
namespace {

[[nodiscard]] QString TrimTrailingSlash(QString u) {
	u = u.trimmed();
	while (u.endsWith(QLatin1Char('/'))) {
		u.chop(1);
	}
	return u;
}

} // namespace

QString NormalizeOpenAiBaseUrl(QString urlOrBase) {
	auto u = TrimTrailingSlash(std::move(urlOrBase));
	if (u.isEmpty()) {
		return {};
	}
	static const auto kEmb = QStringLiteral("/v1/embeddings");
	static const auto kChat = QStringLiteral("/v1/chat/completions");
	static const auto kModels = QStringLiteral("/v1/models");
	if (u.endsWith(kEmb, Qt::CaseInsensitive)) {
		u.chop(kEmb.size());
		return TrimTrailingSlash(u);
	}
	if (u.endsWith(kChat, Qt::CaseInsensitive)) {
		u.chop(kChat.size());
		return TrimTrailingSlash(u);
	}
	if (u.endsWith(kModels, Qt::CaseInsensitive)) {
		u.chop(kModels.size());
		return TrimTrailingSlash(u);
	}
	return u;
}

QString OpenAiOriginBaseForModelsList(const QString &urlOrBase) {
	const auto n = NormalizeOpenAiBaseUrl(urlOrBase);
	if (!n.isEmpty()) {
		return n;
	}
	const auto q = QUrl::fromUserInput(urlOrBase.trimmed());
	if (!q.isValid() || q.scheme().isEmpty()) {
		return {};
	}
	auto o = q;
	o.setPath(QString());
	o.setQuery(QString());
	o.setFragment(QString());
	return o.toString();
}

QString EmbeddingsUrlFromChatBase(const QString &urlOrBase) {
	const auto n = NormalizeOpenAiBaseUrl(urlOrBase);
	if (n.isEmpty()) {
		return {};
	}
	return n + QStringLiteral("/v1/embeddings");
}

void FetchOpenAiModelIdsAsync(
		const QString &baseUrl,
		Fn<void(QStringList ids, QString error)> onMainThread,
		const QString &authorizationBearer) {
	const auto normalized = NormalizeOpenAiBaseUrl(baseUrl);
	const auto bearer = authorizationBearer.trimmed();
	if (!onMainThread) {
		return;
	}
	if (normalized.isEmpty()) {
		const auto app = QCoreApplication::instance();
		if (app) {
			InvokeQueued(app, [onMainThread = std::move(onMainThread)]() mutable {
				onMainThread({}, QStringLiteral("Пустой URL"));
			});
		}
		return;
	}
	std::thread([normalized, bearer, cb = std::move(onMainThread)]() mutable {
		QStringList ids;
		QString err;
		const auto url = QUrl(normalized + QStringLiteral("/v1/models"));
		if (!url.isValid() || url.scheme().isEmpty()) {
			err = QStringLiteral("Некорректный URL");
		} else {
			auto request = QNetworkRequest(url);
			request.setTransferTimeout(15000);
			if (!bearer.isEmpty()) {
				request.setRawHeader(
					"Authorization",
					QByteArrayLiteral("Bearer ") + bearer.toUtf8());
			}
			QNetworkAccessManager nam;
			auto reply = nam.get(request);
			QEventLoop loop;
			QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
			loop.exec();
			if (reply->error() != QNetworkReply::NoError) {
				err = reply->errorString();
			} else {
				const auto root = QJsonDocument::fromJson(reply->readAll()).object();
				for (const auto &item : root.value(QStringLiteral("data")).toArray()) {
					const auto id = item.toObject().value(QStringLiteral("id")).toString();
					if (!id.isEmpty()) {
						ids.push_back(id);
					}
				}
				if (ids.isEmpty()) {
					err = QStringLiteral("Список моделей пуст");
				}
			}
			reply->deleteLater();
		}
		const auto app = QCoreApplication::instance();
		if (!app) {
			return;
		}
		InvokeQueued(app, [
				ids = std::move(ids),
				err = std::move(err),
				cb = std::move(cb)]() mutable {
			cb(std::move(ids), std::move(err));
		});
	}).detach();
}

} // namespace TeleForge
