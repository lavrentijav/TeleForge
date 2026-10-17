#pragma once

#include <optional>
#include <vector>

#include <QtCore/QString>

namespace TeleForge::VectorDb {

struct FactRow {
	int id = 0;
	QString title;
	QString summary;
	QString scope;
	long long chatId = 0;
	long long userId = 0;
	QString contentHash;
	int updatedAt = 0;
};

struct SearchHit {
	FactRow row;
	float score = 0.f;
};

/// Local vector table over the SQLite-backed memory store, shaped like a
/// LanceDB table: rows carry a vector plus scalar metadata, queries are
/// nearest-neighbour with optional scalar filters. The vectors are held in one
/// contiguous buffer so a query is a single pass of dot products instead of a
/// row-per-item SQL round trip.
class Table {
public:
	struct Filter {
		std::optional<QString> scope;
		std::optional<long long> chatId;
		std::optional<long long> userId;
	};

	void reload();
	void invalidate();

	[[nodiscard]] int rows();
	[[nodiscard]] int dimensions();

	[[nodiscard]] std::vector<SearchHit> search(
		const std::vector<float> &query,
		int limit = 10,
		float minScore = 0.f,
		const Filter &filter = {});

	[[nodiscard]] std::vector<SearchHit> searchText(
		const QString &query,
		int limit = 10,
		float minScore = 0.f,
		const Filter &filter = {});
};

[[nodiscard]] Table &facts();

/// Content fingerprint of the whole local fact set. Two devices holding the
/// same facts produce the same value, so comparing it with the fingerprint the
/// server stored alongside the last uploaded snapshot answers "is my copy
/// stale?" without downloading the snapshot itself.
[[nodiscard]] QString LocalDatasetHash();

void SetKnownRemoteHash(const QString &hash);
[[nodiscard]] QString KnownRemoteHash();
[[nodiscard]] bool IsStale();

} // namespace TeleForge::VectorDb
