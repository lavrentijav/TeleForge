// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#pragma once

#include "tfc/data/entities.h"

#include <QtNetwork/QNetworkReply>

extern std::unordered_set<TfId> default_developers;
extern std::unordered_set<TfId> default_channels;

struct CustomBadge
{
	EmojiStatusId emojiStatusId;
	QString text;
};

class RCManager final : public QObject
{
	Q_OBJECT
public:
	static RCManager &getInstance() {
		static RCManager instance;
		return instance;
	}

	RCManager(const RCManager &) = delete;
	RCManager &operator=(const RCManager &) = delete;
	RCManager(RCManager &&) = delete;
	RCManager &operator=(RCManager &&) = delete;

	void start();

	[[nodiscard]] const std::unordered_set<TfId> &developers() const {
		if (!initialized) {
			return default_developers;
		}
		return _developers;
	}

	[[nodiscard]] const std::unordered_set<TfId> &channels() const {
		if (!initialized) {
			return default_channels;
		}
		return _officialChannels;
	}

	[[nodiscard]] const std::unordered_set<TfId> &supporters() const {
		return _supporters;
	}

	[[nodiscard]] const std::unordered_set<TfId> &supporterChannels() const {
		return _supporterChannels;
	}

	[[nodiscard]] const std::unordered_map<TfId, CustomBadge> &supporterCustomBadges() const {
		return _customBadges;
	}

	[[nodiscard]] QString donateUsername() const {
		return _donateUsername;
	}

	[[nodiscard]] QString donateAmountUsd() const {
		return _donateAmountUsd;
	}

	[[nodiscard]] QString donateAmountTon() const {
		return _donateAmountTon;
	}

	[[nodiscard]] QString donateAmountRub() const {
		return _donateAmountRub;
	}

private:
	RCManager() = default;
	~RCManager();

	void makeRequest();
	void sendRequest();
	bool tryRetryWithExteraFallback();

	void gotResponse();
	void gotFailure(QNetworkReply::NetworkError e);
	void clearSentRequest();
	bool handleResponse(const QByteArray &response);
	bool applyResponse(const QByteArray &response);

	bool initialized = false;

	std::unordered_set<TfId> _developers = {};
	std::unordered_set<TfId> _officialChannels = {};
	std::unordered_set<TfId> _supporters = {};
	std::unordered_set<TfId> _supporterChannels = {};
	std::unordered_map<TfId, CustomBadge> _customBadges = {};

	QString _donateUsername = QString();
	QString _donateAmountUsd = QString("4.60");
	QString _donateAmountTon = QString("3.50");
	QString _donateAmountRub = QString("360");

	QTimer* _timer = nullptr;

	std::unique_ptr<QNetworkAccessManager> _manager = nullptr;
	QNetworkReply *_reply = nullptr;
	bool _useExteraFallback = false;
	bool _retryAttempted = false;

};
