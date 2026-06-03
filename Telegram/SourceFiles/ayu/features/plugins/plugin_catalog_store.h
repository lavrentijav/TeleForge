#pragma once

#include <QString>
#include <QVector>
#include <functional>

namespace TeleForge::Plugins {

struct CatalogEntry {
	QString fileName;
	QString title;
	QString downloadUrl;
	QString devId;
	QString pluginSignatureHex;
	QString sourceLabel;
};

using CatalogCallback = Fn<void(QVector<CatalogEntry> entries, QString error)>;
using DoneCallback = Fn<void(bool ok, QString message)>;

void fetchCatalog(CatalogCallback done);
void installCatalogEntry(const CatalogEntry &entry, DoneCallback done);

[[nodiscard]] QVector<CatalogEntry> cachedCatalog();
void clearCatalogCache();

} // namespace TeleForge::Plugins
