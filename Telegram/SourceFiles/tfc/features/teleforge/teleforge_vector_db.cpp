#include "tfc/features/teleforge/teleforge_vector_db.h"

#include "tfc/features/teleforge/teleforge_embeddings.h"
#include "tfc/features/teleforge/teleforge_storage.h"

#include "base/unixtime.h"
#include "logs.h"

#include <algorithm>
#include <optional>
#include <cmath>

#include <QtCore/QCryptographicHash>
#include <QtCore/QMutex>
#include <QtCore/QStringList>

namespace TeleForge::VectorDb {
namespace {

constexpr auto kDatasetHashArtifactType = "dataset_version";
constexpr auto kRemoteHashArtifactName = "remote";

struct Index {
	bool valid = false;
	int dimensions = 0;
	std::vector<FactRow> rows;
	// Row-major [rows.size() * dimensions] of int8 components plus the per-row
	// scale; every vector is L2-normalised before quantising so a query only
	// needs a dot product, not a per-row norm.
	std::vector<signed char> vectors;
	std::vector<float> scales;
};

auto g_mutex = QMutex();
auto g_index = Index();

void NormalizeInPlace(float *values, int count) {
	auto squared = 0.f;
	for (auto i = 0; i != count; ++i) {
		squared += values[i] * values[i];
	}
	const auto length = std::sqrt(squared);
	if (length <= 0.000001f) {
		return;
	}
	for (auto i = 0; i != count; ++i) {
		values[i] /= length;
	}
}

// Returns the scale a reader multiplies the quantised components by.
float Quantize(const float *values, int count, std::vector<signed char> *out) {
	auto maxAbs = 0.f;
	for (auto i = 0; i != count; ++i) {
		maxAbs = std::max(maxAbs, std::abs(values[i]));
	}
	const auto scale = maxAbs / 127.f;
	out->resize(count);
	if (maxAbs <= 0.f) {
		out->assign(count, 0);
		return 0.f;
	}
	for (auto i = 0; i != count; ++i) {
		out->data()[i] = static_cast<signed char>(
			std::clamp<long>(std::lround(values[i] / scale), -127, 127));
	}
	return scale;
}

void RebuildLocked() {
	auto index = Index();
	const auto items = Storage::loadMemoryItems();
	index.rows.reserve(items.size());

	for (const auto &item : items) {
		if (item.manualHidden) {
			continue;
		}
		const auto embedding = Storage::loadMemoryEmbedding(item.id);
		if (!embedding || embedding->vectorBlob.empty()) {
			continue;
		}
		auto values = DeserializeEmbedding(embedding->vectorBlob);
		if (values.empty()) {
			continue;
		}
		if (!index.dimensions) {
			index.dimensions = int(values.size());
		} else if (int(values.size()) != index.dimensions) {
			// A model switch leaves rows of two different widths behind; the
			// narrower generation is simply not searchable until it is
			// re-embedded, and mixing them would silently score garbage.
			continue;
		}
		NormalizeInPlace(values.data(), int(values.size()));
		auto quantized = std::vector<signed char>();
		const auto scale = Quantize(
			values.data(),
			int(values.size()),
			&quantized);
		index.rows.push_back(FactRow{
			.id = item.id,
			.title = QString::fromStdString(item.title),
			.summary = QString::fromStdString(item.summary),
			.scope = QString::fromStdString(item.scopeType),
			.chatId = item.chatId.value_or(0),
			.userId = item.userId.value_or(0),
			.contentHash = QString::fromStdString(item.contentHash),
			.updatedAt = item.updatedAt,
		});
		index.vectors.insert(
			index.vectors.end(),
			quantized.begin(),
			quantized.end());
		index.scales.push_back(scale);
	}
	index.valid = true;
	g_index = std::move(index);
	LOG(("TeleForge VectorDb: index rebuilt, %1 rows of %2 dims")
		.arg(g_index.rows.size())
		.arg(g_index.dimensions));
}

void EnsureLoadedLocked() {
	if (!g_index.valid) {
		RebuildLocked();
	}
}

[[nodiscard]] bool Matches(const FactRow &row, const Table::Filter &filter) {
	if (filter.scope && row.scope != *filter.scope) {
		return false;
	}
	if (filter.chatId && row.chatId != *filter.chatId) {
		return false;
	}
	if (filter.userId && row.userId != *filter.userId) {
		return false;
	}
	return true;
}

} // namespace

void Table::reload() {
	QMutexLocker lock(&g_mutex);
	RebuildLocked();
}

void Table::invalidate() {
	QMutexLocker lock(&g_mutex);
	g_index.valid = false;
}

int Table::rows() {
	QMutexLocker lock(&g_mutex);
	EnsureLoadedLocked();
	return int(g_index.rows.size());
}

int Table::dimensions() {
	QMutexLocker lock(&g_mutex);
	EnsureLoadedLocked();
	return g_index.dimensions;
}

std::vector<SearchHit> Table::search(
		const std::vector<float> &query,
		int limit,
		float minScore,
		const Filter &filter) {
	if (query.empty() || limit <= 0) {
		return {};
	}
	QMutexLocker lock(&g_mutex);
	EnsureLoadedLocked();
	if (g_index.rows.empty() || int(query.size()) != g_index.dimensions) {
		return {};
	}
	auto normalized = query;
	NormalizeInPlace(normalized.data(), int(normalized.size()));

	const auto dims = g_index.dimensions;
	auto hits = std::vector<SearchHit>();
	hits.reserve(g_index.rows.size());
	for (auto i = 0U; i != g_index.rows.size(); ++i) {
		const auto &row = g_index.rows[i];
		if (!Matches(row, filter)) {
			continue;
		}
		const auto *vector = g_index.vectors.data() + (i * dims);
		auto dot = 0.f;
		for (auto d = 0; d != dims; ++d) {
			dot += float(vector[d]) * normalized[d];
		}
		dot *= g_index.scales[i];
		if (dot < minScore) {
			continue;
		}
		hits.push_back({ .row = row, .score = dot });
	}
	const auto keep = std::min(std::size_t(limit), hits.size());
	std::partial_sort(
		hits.begin(),
		hits.begin() + keep,
		hits.end(),
		[](const SearchHit &a, const SearchHit &b) {
			return a.score > b.score;
		});
	hits.resize(keep);
	return hits;
}

std::vector<SearchHit> Table::searchText(
		const QString &query,
		int limit,
		float minScore,
		const Filter &filter) {
	if (query.trimmed().isEmpty()) {
		return {};
	}
	const auto embedding = BuildLocalEmbedding(query);
	return search(embedding.values, limit, minScore, filter);
}

Table &facts() {
	static auto table = Table();
	return table;
}

QString LocalDatasetHash() {
	auto hashes = QStringList();
	for (const auto &item : Storage::loadMemoryItems()) {
		if (item.manualHidden) {
			continue;
		}
		const auto content = item.contentHash.empty()
			? (std::to_string(item.id) + ":" + std::to_string(item.updatedAt))
			: item.contentHash;
		hashes.push_back(QString::fromStdString(content));
	}
	if (hashes.isEmpty()) {
		return QString();
	}
	hashes.sort();
	auto hash = QCryptographicHash(QCryptographicHash::Sha256);
	hash.addData(QByteArray::number(hashes.size()));
	for (const auto &value : hashes) {
		hash.addData(value.toUtf8());
	}
	return QString::fromLatin1(hash.result().toHex());
}

void SetKnownRemoteHash(const QString &hash) {
	// storeSyncArtifact() appends a new row whenever id is unset, so the
	// existing row id has to be carried over or the table grows one entry per
	// freshness check.
	auto record = Storage::SyncArtifactRecord{
		.artifactType = kDatasetHashArtifactType,
		.artifactName = kRemoteHashArtifactName,
		.payload = hash.toStdString(),
		.updatedAt = base::unixtime::now(),
	};
	for (const auto &artifact
			: Storage::loadSyncArtifacts(kDatasetHashArtifactType)) {
		if (artifact.artifactName == kRemoteHashArtifactName) {
			record.id = artifact.id;
			break;
		}
	}
	Storage::storeSyncArtifact(record);
}

QString KnownRemoteHash() {
	auto best = std::optional<Storage::SyncArtifactRecord>();
	for (const auto &artifact
			: Storage::loadSyncArtifacts(kDatasetHashArtifactType)) {
		if (artifact.artifactName != kRemoteHashArtifactName) {
			continue;
		}
		if (!best || artifact.updatedAt >= best->updatedAt) {
			best = artifact;
		}
	}
	return best ? QString::fromStdString(best->payload) : QString();
}

bool IsStale() {
	const auto remote = KnownRemoteHash();
	if (remote.isEmpty()) {
		return false;
	}
	return (remote != LocalDatasetHash());
}

} // namespace TeleForge::VectorDb
