// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#include "ayu/data/ayu_database.h"

#include "ayu/data/ayu_content_hash.h"
#include "ayu/data/entities.h"
#include "ayu/features/teleforge/teleforge_unified_db.h"
#include "base/unixtime.h"
#include "logs.h"

#include <QtCore/QFile>

using namespace sqlite_orm;

namespace {
auto &db() {
	return TeleForge::Storage::Db();
}
}

namespace AyuMigrations {

void migrateToV1(decltype(db()) &store) {
	// drop RegexFilter table as we've added primary_key()
	try {
		store.drop_table_if_exists("RegexFilter");
		LOG(("Migration to V1 successful."));
	} catch (const std::exception &ex) {
		LOG(("Migration to V1 failed: %1").arg(ex.what()));
	}
}

void migrateToV2(decltype(db()) &store) {
	try {
		for (auto &row : store.get_all<EditedMessage>()) {
			if (row.contentHash.empty()) {
				row.contentHash = AyuContentHash::ComputeEdited(row);
				store.update(row);
			}
		}
		for (auto &row : store.get_all<DeletedMessage>()) {
			if (row.contentHash.empty()) {
				row.contentHash = AyuContentHash::ComputeDeleted(row);
				store.update(row);
			}
		}
		LOG(("Migration to V2 successful."));
	} catch (const std::exception &ex) {
		LOG(("Migration to V2 failed: %1").arg(ex.what()));
	}
}

}

void runMigrations(decltype(db()) &store) {
	constexpr int kLatestVersion = 2;

	const std::map<int, Fn<void(decltype(db()) &)>> migrations = {
		{1, AyuMigrations::migrateToV1},
		{2, AyuMigrations::migrateToV2},
	};

	int currentVersion = 0;
	try {
		if (auto versionRow = store.get_pointer<AyuDataSchemaVersion>(1)) {
			currentVersion = versionRow->version;
		} else {
			store.insert(AyuDataSchemaVersion{1, 0});
		}
	} catch (...) {
		LOG(("No AyuDataSchemaVersion, assuming 0"));
		store.insert(AyuDataSchemaVersion{1, 0});
	}

	if (currentVersion >= kLatestVersion) {
		LOG(("Ayu message store schema is ok"));
		return;
	}

	LOG(("Ayu message store version: %1. Latest version: %2.").arg(currentVersion).arg(kLatestVersion));

	for (int v = currentVersion + 1; v <= kLatestVersion; ++v) {
		if (migrations.contains(v)) {
			try {
				LOG(("Ayu migration for version: %1").arg(v));
				store.begin_transaction();

				migrations.at(v)(store);

				store.update_all(set(c(&AyuDataSchemaVersion::version) = v), where(c(&AyuDataSchemaVersion::id) == 1));
				store.commit();
				LOG(("Applied Ayu migration for version: %1.").arg(v));
			} catch (...) {
				store.rollback();
				LOG(("Failed to apply Ayu migration for version: %1.").arg(v));
				return;
			}
		}
	}
}

namespace AyuDatabase {

void moveCurrentDatabase() {
	const auto time = base::unixtime::now();

	if (QFile::exists("./tdata/ayudata.db")) {
		QFile::rename("./tdata/ayudata.db", QString("./tdata/ayudata_%1.db").arg(time));
	}

	if (QFile::exists("./tdata/ayudata.db-shm")) {
		QFile::rename("./tdata/ayudata.db-shm", QString("./tdata/ayudata_%1.db-shm").arg(time));
	}

	if (QFile::exists("./tdata/ayudata.db-wal")) {
		QFile::rename("./tdata/ayudata.db-wal", QString("./tdata/ayudata_%1.db-wal").arg(time));
	}
}

void initialize() {
	try {
		runMigrations(db());
	} catch (const std::exception &ex) {
		LOG(("Ayu message store migration failed: %1").arg(ex.what()));
	}
}

void addEditedMessage(const EditedMessage &message) {
	try {
		auto row = message;
		if (row.contentHash.empty()) {
			row.contentHash = AyuContentHash::ComputeEdited(row);
		}
		db().begin_transaction();
		db().insert(row);
		db().commit();
	} catch (std::exception &ex) {
		LOG(("Failed to save edited message for some reason: %1").arg(ex.what()));
	}
}

std::vector<EditedMessage> getEditedMessages(ID userId, ID dialogId, ID messageId, ID minId, ID maxId, int totalLimit) {
	return db().get_all<EditedMessage>(
		where(
			column<EditedMessage>(&EditedMessage::userId) == userId and
			column<EditedMessage>(&EditedMessage::dialogId) == dialogId and
			column<EditedMessage>(&EditedMessage::messageId) == messageId and
			(column<EditedMessage>(&EditedMessage::fakeId) > minId or minId == 0) and
			(column<EditedMessage>(&EditedMessage::fakeId) < maxId or maxId == 0)
		),
		order_by(column<EditedMessage>(&EditedMessage::fakeId)).desc(),
		limit(totalLimit)
	);
}

bool hasRevisions(ID userId, ID dialogId, ID messageId) {
	try {
		return !db().select(
			columns(column<EditedMessage>(&EditedMessage::messageId)),
			where(
				column<EditedMessage>(&EditedMessage::userId) == userId and
				column<EditedMessage>(&EditedMessage::dialogId) == dialogId and
				column<EditedMessage>(&EditedMessage::messageId) == messageId
			),
			limit(1)
		).empty();
	} catch (std::exception &ex) {
		LOG(("Failed to check if message has revisions: %1").arg(ex.what()));
		return false;
	}
}

void addDeletedMessage(const DeletedMessage &message) {
	try {
		auto row = message;
		if (row.contentHash.empty()) {
			row.contentHash = AyuContentHash::ComputeDeleted(row);
		}
		db().begin_transaction();
		db().insert(row);
		db().commit();
	} catch (std::exception &ex) {
		LOG(("Failed to save edited message for some reason: %1").arg(ex.what()));
	}
}

std::vector<DeletedMessage> getDeletedMessages(ID userId, ID dialogId, ID topicId, ID minId, ID maxId, int totalLimit, const std::string &searchQuery) {
	if (searchQuery.empty()) {
		return db().get_all<DeletedMessage>(
			where(
				column<DeletedMessage>(&DeletedMessage::userId) == userId and
				column<DeletedMessage>(&DeletedMessage::dialogId) == dialogId and
				(column<DeletedMessage>(&DeletedMessage::topicId) == topicId or topicId == 0) and
				(column<DeletedMessage>(&DeletedMessage::messageId) > minId or minId == 0) and
				(column<DeletedMessage>(&DeletedMessage::messageId) < maxId or maxId == 0)
			),
			order_by(column<DeletedMessage>(&DeletedMessage::messageId)).desc(),
			limit(totalLimit)
		);
	}

	std::string escaped;
	escaped.reserve(searchQuery.size());
	for (const auto c : searchQuery) {
		if (c == '%' || c == '_' || c == '\\') {
			escaped += '\\';
		}
		escaped += c;
	}
	const auto pattern = "%" + escaped + "%";
	return db().get_all<DeletedMessage>(
		where(
			column<DeletedMessage>(&DeletedMessage::userId) == userId and
			column<DeletedMessage>(&DeletedMessage::dialogId) == dialogId and
			(column<DeletedMessage>(&DeletedMessage::topicId) == topicId or topicId == 0) and
			(column<DeletedMessage>(&DeletedMessage::messageId) > minId or minId == 0) and
			(column<DeletedMessage>(&DeletedMessage::messageId) < maxId or maxId == 0) and
			like(column<DeletedMessage>(&DeletedMessage::text), pattern, "\\")
		),
		order_by(column<DeletedMessage>(&DeletedMessage::messageId)).desc(),
		limit(totalLimit)
	);
}

bool hasDeletedMessages(ID userId, ID dialogId, ID topicId) {
	try {
		return !db().select(
			columns(column<DeletedMessage>(&DeletedMessage::dialogId)),
			where(
				column<DeletedMessage>(&DeletedMessage::userId) == userId and
				column<DeletedMessage>(&DeletedMessage::dialogId) == dialogId and
				(column<DeletedMessage>(&DeletedMessage::topicId) == topicId or topicId == 0)
			),
			limit(1)
		).empty();
	} catch (std::exception &ex) {
		LOG(("Failed to check if dialog has deleted message: %1").arg(ex.what()));
		return false;
	}
}

template<typename T>
std::vector<T> getAllT() {
	try {
		return db().get_all<T>();
	} catch (std::exception &ex) {
		LOG(("Failed to get all: %1").arg(ex.what()));
		return {};
	}
}

std::vector<RegexFilter> getAllRegexFilters() {
	return getAllT<RegexFilter>();
}

std::vector<RegexFilterGlobalExclusion> getAllFiltersExclusions() {
	return getAllT<RegexFilterGlobalExclusion>();
}

std::vector<RegexFilter> getExcludedByDialogId(ID dialogId) {
	try {
		return db().get_all<RegexFilter>(
			where(in(&RegexFilter::id,
					 db().select(columns(&RegexFilterGlobalExclusion::filterId),
									where(is_equal(&RegexFilterGlobalExclusion::dialogId, dialogId))
					 )
			))
		);
	} catch (std::exception &ex) {
		LOG(("Failed to get excluded by dialog id: %1").arg(ex.what()));
		return {};
	}
}

int getCount() {
	try {
		return db().count<RegexFilter>();
	} catch (std::exception &ex) {
		LOG(("Failed to get count: %1").arg(ex.what()));
		return 0;
	}
}

RegexFilter getById(std::vector<char> id) {
	try {
		return db().get<RegexFilter>(
			where(column<RegexFilter>(&RegexFilter::id) == std::move(id))
		);
	} catch (std::exception &ex) {
		LOG(("Failed to get filters by id: %1").arg(ex.what()));
		return {};
	}
}

std::vector<RegexFilter> getShared() {
	try {
		return db().get_all<RegexFilter>(
			where(is_null(column<RegexFilter>(&RegexFilter::dialogId)))
		);
	} catch (std::exception &ex) {
		LOG(("Failed to get shared filters: %1").arg(ex.what()));
		return {};
	}
}

std::vector<RegexFilter> getByDialogId(ID dialogId) {
	try {
		return db().get_all<RegexFilter>(
			where(column<RegexFilter>(&RegexFilter::dialogId) == dialogId)
		);
	} catch (std::exception &ex) {
		LOG(("Failed to get filters by dialog id: %1").arg(ex.what()));
		return {};
	}
}

void addRegexFilter(const RegexFilter &filter) {
	try {
		db().begin_transaction();
		db().replace(filter); // we're using replace as we set std::vector<char> as primary key
		db().commit();
	} catch (std::exception &ex) {
		db().rollback();
		LOG(("Failed to save regex filter for some reason: %1").arg(ex.what()));
	}
}

void addRegexExclusion(const RegexFilterGlobalExclusion &exclusion) {
	try {
		db().begin_transaction();
		db().insert(exclusion);
		db().commit();
	} catch (std::exception &ex) {
		LOG(("Failed to save regex filter exclusion for some reason: %1").arg(ex.what()));
	}
}

void updateRegexFilter(const RegexFilter &filter) {
	try {
		db().update_all(
			set(
				c(&RegexFilter::text) = filter.text,
				c(&RegexFilter::enabled) = filter.enabled,
				c(&RegexFilter::reversed) = filter.reversed,
				c(&RegexFilter::caseInsensitive) = filter.caseInsensitive,
				c(&RegexFilter::dialogId) = filter.dialogId
			),
			where(c(&RegexFilter::id) == filter.id)
		);
	} catch (std::exception &ex) {
		LOG(("Failed to update regex filter for some reason: %1").arg(ex.what()));
	}
}

void deleteFilter(const std::vector<char> &id) {
	try {
		db().remove_all<RegexFilter>(
			where(column<RegexFilter>(&RegexFilter::id) == id)
		);
	} catch (std::exception &ex) {
		LOG(("Failed to delete regex filter for some reason: %1").arg(ex.what()));
	}
}

void deleteExclusionsByFilterId(const std::vector<char> &id) {
	try {
		db().remove_all<RegexFilterGlobalExclusion>(
			where(column<RegexFilterGlobalExclusion>(&RegexFilterGlobalExclusion::filterId) == id)
		);
	} catch (std::exception &ex) {
		LOG(("Failed to delete regex filter exclusion by filter id for some reason: %1").arg(ex.what()));
	}
}

void deleteExclusion(ID dialogId, std::vector<char> filterId) {
	try {
		db().remove_all<RegexFilterGlobalExclusion>(
			where(column<RegexFilterGlobalExclusion>(&RegexFilterGlobalExclusion::filterId) == filterId and
				column<RegexFilterGlobalExclusion>(&RegexFilterGlobalExclusion::dialogId) == dialogId
			)
		);
	} catch (std::exception &ex) {
		LOG(("Failed to delete regex filter exclusion for some reason: %1").arg(ex.what()));
	}
}

void deleteAllFilters() {
	try {
		db().remove_all<RegexFilter>();
	} catch (std::exception &ex) {
		LOG(("Failed to delete all regex filter for some reason: %1").arg(ex.what()));
	}
}

void deleteAllExclusions() {
	try {
		db().remove_all<RegexFilterGlobalExclusion>();
	} catch (std::exception &ex) {
		LOG(("Failed to delete all regex filter exclusions for some reason: %1").arg(ex.what()));
	}
}

bool hasFilters() {
	try {
		return !db().select(
			columns(column<RegexFilter>(&RegexFilter::id)),
			limit(1)
		).empty();
	} catch (std::exception &ex) {
		LOG(("Failed to check if there's any filters: %1").arg(ex.what()));
		return false;
	}
}

bool hasPerDialogFilters() {
	try {
		return
			!db().select(
				columns(column<RegexFilter>(&RegexFilter::id)),
				where(is_not_null(column<RegexFilter>(&RegexFilter::dialogId))),
				limit(1)
			).empty() ||
			!db().select(
				columns(column<RegexFilterGlobalExclusion>(&RegexFilterGlobalExclusion::fakeId)),
				limit(1)
			).empty();
	} catch (std::exception &ex) {
		LOG(("Failed to check if there's any filters: %1").arg(ex.what()));
		return false;
	}
}

void insertOnlineEvent(const OnlineEvent &event) {
	try {
		db().insert(event);
	} catch (const std::exception &ex) {
		LOG(("Failed to insert OnlineEvent: %1").arg(ex.what()));
	}
}

std::vector<OnlineEvent> loadOnlineEventsForUser(ID userId, int sinceTs) {
	try {
		return db().get_all<OnlineEvent>(
			where(
				column<OnlineEvent>(&OnlineEvent::userId) == userId
				and column<OnlineEvent>(&OnlineEvent::timestamp) >= sinceTs),
			order_by(column<OnlineEvent>(&OnlineEvent::timestamp)).desc(),
			limit(500));
	} catch (const std::exception &ex) {
		LOG(("Failed to load OnlineEvent: %1").arg(ex.what()));
		return {};
	}
}

std::optional<int> manualLastSeenForUser(ID userId) {
	try {
		const auto rows = db().select(
			columns(column<OnlineEvent>(&OnlineEvent::manualLastSeen)),
			where(
				column<OnlineEvent>(&OnlineEvent::userId) == userId
				and column<OnlineEvent>(&OnlineEvent::manualLastSeen) > 0),
			order_by(column<OnlineEvent>(&OnlineEvent::timestamp)).desc(),
			limit(1));
		if (rows.empty()) {
			return std::nullopt;
		}
		return std::get<0>(rows.front());
	} catch (const std::exception &ex) {
		LOG(("Failed manualLastSeenForUser: %1").arg(ex.what()));
		return std::nullopt;
	}
}

void upsertSpyTarget(ID userId, bool enabled) {
	try {
		SpyTarget row{ userId, enabled, base::unixtime::now() };
		db().replace(row);
	} catch (const std::exception &ex) {
		LOG(("Failed upsertSpyTarget: %1").arg(ex.what()));
	}
}

bool hasSpyTargetOverride(ID userId) {
	try {
		return db().get_pointer<SpyTarget>(userId) != nullptr;
	} catch (const std::exception &ex) {
		LOG(("Failed hasSpyTargetOverride: %1").arg(ex.what()));
		return false;
	}
}

bool isSpyTargetEnabled(ID userId) {
	try {
		if (const auto row = db().get_pointer<SpyTarget>(userId)) {
			return row->enabled;
		}
	} catch (const std::exception &ex) {
		LOG(("Failed isSpyTargetEnabled: %1").arg(ex.what()));
	}
	return false;
}

void purgeOnlineEventsBefore(int timestamp) {
	try {
		db().remove_all<OnlineEvent>(
			where(column<OnlineEvent>(&OnlineEvent::timestamp) < timestamp));
	} catch (const std::exception &ex) {
		LOG(("Failed purgeOnlineEventsBefore: %1").arg(ex.what()));
	}
}

}
