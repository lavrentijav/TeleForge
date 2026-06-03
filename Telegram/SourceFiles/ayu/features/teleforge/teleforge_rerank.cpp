#include "ayu/features/teleforge/teleforge_rerank.h"

#include "ayu/features/teleforge/teleforge_llama_runtime.h"
#include "ayu/features/teleforge/teleforge_paths.h"

#include "logs.h"

#include <algorithm>
#include <cmath>

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QEventLoop>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <condition_variable>
#include <mutex>

#include <crl/crl.h>

#include <QtCore/QMutex>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

namespace TeleForge {
namespace {

auto g_mutex = QMutex();
auto g_config = RerankRuntimeConfig{};

[[nodiscard]] float Sigmoid(float x) {
	return 1.f / (1.f + std::exp(-x));
}

[[nodiscard]] std::vector<float> NormalizeScores(std::vector<float> raw) {
	if (raw.empty()) {
		return raw;
	}
	auto maxAbs = 0.f;
	for (const auto v : raw) {
		maxAbs = std::max(maxAbs, std::abs(v));
	}
	if (maxAbs <= 1.0001f) {
		return raw;
	}
	for (auto &v : raw) {
		v = Sigmoid(v);
	}
	return raw;
}

[[nodiscard]] std::optional<std::vector<float>> ParseScoresJson(
		const QJsonObject &root,
		int expected) {
	if (expected <= 0) {
		return std::vector<float>();
	}
	if (const auto results = root.value(QStringLiteral("results")).toArray();
			results.size() == expected) {
		auto out = std::vector<float>();
		out.reserve(expected);
		for (const auto &item : results) {
			const auto o = item.toObject();
			float s = 0.f;
			if (o.contains(QStringLiteral("score"))) {
				s = static_cast<float>(o.value(QStringLiteral("score")).toDouble());
			} else {
				s = static_cast<float>(
					o.value(QStringLiteral("relevance_score")).toDouble());
			}
			out.push_back(s);
		}
		if (int(out.size()) == expected) {
			return NormalizeScores(std::move(out));
		}
	}
	if (const auto scores = root.value(QStringLiteral("scores")).toArray();
			!scores.isEmpty()) {
		auto out = std::vector<float>();
		for (const auto &v : scores) {
			out.push_back(static_cast<float>(v.toDouble()));
		}
		if (int(out.size()) == expected) {
			return NormalizeScores(std::move(out));
		}
	}
	if (const auto data = root.value(QStringLiteral("data")).toArray();
			!data.isEmpty()) {
		auto out = std::vector<float>(expected, 0.f);
		for (const auto &item : data) {
			const auto o = item.toObject();
			const auto idx = o.value(QStringLiteral("index")).toInt(-1);
			float s = 0.f;
			if (o.contains(QStringLiteral("score"))) {
				s = static_cast<float>(o.value(QStringLiteral("score")).toDouble());
			} else if (o.contains(QStringLiteral("relevance_score"))) {
				s = static_cast<float>(
					o.value(QStringLiteral("relevance_score")).toDouble());
			}
			if (idx >= 0 && idx < expected) {
				out[idx] = s;
			}
		}
		return NormalizeScores(std::move(out));
	}
	if (const auto ranked = root.value(QStringLiteral("results")).toArray();
			!ranked.isEmpty()) {
		auto out = std::vector<float>(expected, 0.f);
		for (const auto &item : ranked) {
			const auto o = item.toObject();
			const auto idx = o.value(QStringLiteral("index")).toInt(-1);
			float s = 0.f;
			if (o.contains(QStringLiteral("score"))) {
				s = static_cast<float>(o.value(QStringLiteral("score")).toDouble());
			} else if (o.contains(QStringLiteral("relevance_score"))) {
				s = static_cast<float>(
					o.value(QStringLiteral("relevance_score")).toDouble());
			}
			if (idx >= 0 && idx < expected) {
				out[idx] = s;
			}
		}
		return NormalizeScores(std::move(out));
	}
	return std::nullopt;
}

[[nodiscard]] float DotProduct(
		const std::vector<float> &a,
		const std::vector<float> &b) {
	const auto n = std::min(a.size(), b.size());
	auto s = 0.f;
	for (size_t i = 0; i < n; ++i) {
		s += a[i] * b[i];
	}
	return s;
}

[[nodiscard]] std::vector<float> NormalizeVector(std::vector<float> v) {
	auto sumSq = 0.;
	for (const auto x : v) {
		sumSq += static_cast<double>(x) * static_cast<double>(x);
	}
	const auto norm = std::sqrt(sumSq);
	if (norm <= 1e-12) {
		return v;
	}
	for (auto &x : v) {
		x = static_cast<float>(static_cast<double>(x) / norm);
	}
	return v;
}

[[nodiscard]] std::optional<std::vector<float>> HttpRerankScoresOpenAiEmbeddings(
		const QString &query,
		const QStringList &documents,
		const RerankRuntimeConfig &config) {
	if (documents.isEmpty()) {
		return std::vector<float>();
	}
	const auto base = QUrl(config.endpointUrl.trimmed());
	if (!base.isValid() || base.scheme().isEmpty()) {
		return std::nullopt;
	}
	const auto embedUrl = base.resolved(QUrl(QStringLiteral("v1/embeddings")));

	auto inputs = QJsonArray();
	inputs.push_back(query);
	for (const auto &d : documents) {
		inputs.push_back(d);
	}
	auto payload = QJsonObject{
		{ QStringLiteral("input"), std::move(inputs) },
	};
	if (!config.modelId.trimmed().isEmpty()) {
		payload.insert(QStringLiteral("model"), config.modelId.trimmed());
	}

	auto request = QNetworkRequest(embedUrl);
	request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
	if (!config.apiKey.trimmed().isEmpty()) {
		request.setRawHeader(
			"Authorization",
			QByteArrayLiteral("Bearer ") + config.apiKey.trimmed().toUtf8());
	}

	QNetworkAccessManager nam;
	auto reply = nam.post(
		request,
		QJsonDocument(payload).toJson(QJsonDocument::Compact));

	QEventLoop loop;
	QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
	loop.exec();

	if (reply->error() != QNetworkReply::NoError) {
		LOG(("TeleForge HttpRerankScores embeddings failed: %1")
				.arg(reply->errorString()));
		reply->deleteLater();
		return std::nullopt;
	}

	const auto body = reply->readAll();
	reply->deleteLater();

	const auto root = QJsonDocument::fromJson(body).object();
	const auto data = root.value(QStringLiteral("data")).toArray();
	if (data.isEmpty()) {
		LOG(("TeleForge HttpRerankScores embeddings: empty data"));
		return std::nullopt;
	}

	const auto expected = int(documents.size() + 1);
	auto byIndex = std::vector<std::optional<std::vector<float>>>(expected);
	for (const auto &item : data) {
		const auto o = item.toObject();
		const auto idx = o.value(QStringLiteral("index")).toInt(-1);
		const auto embArr = o.value(QStringLiteral("embedding")).toArray();
		if (idx < 0 || idx >= expected || embArr.isEmpty()) {
			continue;
		}
		auto vec = std::vector<float>();
		vec.reserve(embArr.size());
		for (const auto &v : embArr) {
			vec.push_back(static_cast<float>(v.toDouble()));
		}
		byIndex[idx] = std::move(vec);
	}
	for (int i = 0; i < expected; ++i) {
		if (!byIndex[i].has_value()) {
			LOG(("TeleForge HttpRerankScores embeddings: missing index %1").arg(i));
			return std::nullopt;
		}
	}
	auto queryNorm = NormalizeVector(std::move(*byIndex[0]));
	auto out = std::vector<float>();
	out.reserve(documents.size());
	for (int i = 0; i < documents.size(); ++i) {
		const auto docNorm = NormalizeVector(std::move(*byIndex[i + 1]));
		const auto sim = DotProduct(queryNorm, docNorm);
		out.push_back(std::clamp(0.5f * (sim + 1.f), 0.f, 1.f));
	}
	return out;
}

[[nodiscard]] std::optional<std::vector<float>> RerankScoresNativeEmbeddings(
		const QString &query,
		const QStringList &documents) {
	if (documents.isEmpty()) {
		return std::vector<float>();
	}
	const auto qEmb = NativeEmbedOneBlocking(query);
	if (!qEmb) {
		LOG(("TeleForge NativeRerank: query embed failed"));
		return std::nullopt;
	}
	auto queryNorm = NormalizeVector(*qEmb);
	auto out = std::vector<float>();
	out.reserve(documents.size());
	for (const auto &d : documents) {
		const auto dEmb = NativeEmbedOneBlocking(d);
		if (!dEmb || dEmb->size() != queryNorm.size()) {
			LOG(("TeleForge NativeRerank: doc embed failed or dim mismatch"));
			return std::nullopt;
		}
		const auto docNorm = NormalizeVector(*dEmb);
		const auto sim = DotProduct(queryNorm, docNorm);
		out.push_back(std::clamp(0.5f * (sim + 1.f), 0.f, 1.f));
	}
	return out;
}

} // namespace

std::optional<std::vector<float>> HttpRerankScores(
		const QString &query,
		const QStringList &documents,
		const RerankRuntimeConfig &config) {
	if (documents.isEmpty()) {
		return std::nullopt;
	}
	if (config.apiKind == RerankApiKind::NativeEmbeddings) {
		return RerankScoresNativeEmbeddings(query, documents);
	}
	if (config.apiKind == RerankApiKind::OpenAiEmbeddings) {
		return HttpRerankScoresOpenAiEmbeddings(query, documents, config);
	}
	if (config.endpointUrl.trimmed().isEmpty()) {
		return std::nullopt;
	}
	const auto url = QUrl(config.endpointUrl.trimmed());
	if (!url.isValid() || url.scheme().isEmpty()) {
		return std::nullopt;
	}

	auto docArray = QJsonArray();
	for (const auto &d : documents) {
		docArray.push_back(d);
	}
	auto payload = QJsonObject{
		{ QStringLiteral("query"), query },
		{ QStringLiteral("documents"), std::move(docArray) },
	};
	if (!config.modelId.trimmed().isEmpty()) {
		payload.insert(QStringLiteral("model"), config.modelId.trimmed());
	}
	if (!config.modelPath.trimmed().isEmpty()) {
		payload.insert(QStringLiteral("model_path"), config.modelPath.trimmed());
	}

	auto request = QNetworkRequest(url);
	request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
	if (!config.apiKey.trimmed().isEmpty()) {
		request.setRawHeader(
			"Authorization",
			QByteArrayLiteral("Bearer ") + config.apiKey.trimmed().toUtf8());
	}

	QNetworkAccessManager nam;
	auto reply = nam.post(
		request,
		QJsonDocument(payload).toJson(QJsonDocument::Compact));

	QEventLoop loop;
	QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
	loop.exec();

	if (reply->error() != QNetworkReply::NoError) {
		LOG(("TeleForge HttpRerankScores failed: %1").arg(reply->errorString()));
		reply->deleteLater();
		return std::nullopt;
	}

	const auto body = reply->readAll();
	reply->deleteLater();

	const auto root = QJsonDocument::fromJson(body).object();
	if (const auto parsed = ParseScoresJson(root, documents.size())) {
		return parsed;
	}
	LOG(("TeleForge HttpRerankScores: could not parse scores from response"));
	return std::nullopt;
}

QString DefaultTeleForgeRerankGgufPath() {
	static const auto kName = QStringLiteral(
		"gte-multilingual-reranker-base-Q8_0.gguf");
	const auto appDir = QCoreApplication::applicationDirPath();
	QStringList candidates = {
		QDir::cleanPath(TeleForgeModelsDirectory() + QLatin1Char('/') + kName),
		appDir + QStringLiteral("/tdata/teleforge/models/") + kName,
		QStringLiteral("./tdata/teleforge/models/") + kName,
		appDir + QStringLiteral("/") + kName,
		QStringLiteral("./") + kName,
		QDir::currentPath() + QStringLiteral("/") + kName,
	};
	for (const auto &p : candidates) {
		const auto clean = QDir::cleanPath(p);
		if (QFileInfo::exists(clean)) {
			return clean;
		}
	}
	return {};
}

void SetRerankRuntimeConfig(RerankRuntimeConfig config) {
	QMutexLocker lock(&g_mutex);
	g_config = std::move(config);
}

RerankRuntimeConfig CurrentRerankConfig() {
	QMutexLocker lock(&g_mutex);
	return g_config;
}

} // namespace TeleForge
