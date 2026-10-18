// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#include "tfc/ayu_infra.h"

#include "tfc/ayu_lang.h"
#include "tfc/ayu_settings.h"
#include "tfc/ayu_ui_settings.h"
#include "tfc/ayu_worker.h"
#include "tfc/data/ayu_database.h"
#include "tfc/features/teleforge/teleforge_bootstrap.h"
#include "tfc/features/teleforge/teleforge_storage.h"
#include "tfc/ui/ayu_logo.h"
#include "features/translator/ayu_translator.h"
#include "lang/lang_instance.h"
#include "utils/rc_manager.h"
#include "logs.h"

#ifdef Q_OS_WIN
#include "tfc/utils/windows_utils.h"
#endif

namespace AyuInfra {

void initLang() {
	QString id = Lang::GetInstance().id();
	QString baseId = Lang::GetInstance().baseId();
	if (id.isEmpty()) {
		LOG(("Language is not loaded"));
		return;
	}
	AyuLanguage::init();
	AyuLanguage::currentInstance()->fetchLanguage(id, baseId);
}

void initUiSettings() {
	const auto &settings = AyuSettings::getInstance();

	AyuUiSettings::setMonoFont(settings.monoFont());
	AyuUiSettings::setWideMultiplier(settings.wideMultiplier());
	AyuUiSettings::setMaterialSwitches(settings.materialSwitches());
	AyuUiSettings::setAvatarCorners(settings.avatarCorners());
}

void initDatabase() {
	TeleForge::Storage::initialize();
	AyuDatabase::initialize();
}

void initTeleForge() {
	LOG(("AyuInfra::initTeleForge — after unified teleforge.db init + Ayu message-store migrations"));
	TeleForge::initialize();
	LOG(("AyuInfra::initTeleForge done"));
}

void initWorker() {
	AyuWorker::initialize();
}

void initRCManager() {
	RCManager::getInstance().start();
}

void initTranslator() {
	Ayu::Translator::TranslateManager::init();
}

void initIcon() {
#ifdef Q_OS_WIN
	TeleForgeAssets::loadAppIco();
	reloadAppIconFromTaskBar();
#endif
}

void init() {
	initLang();
	initDatabase();
	initTeleForge();
	initUiSettings();
	initIcon();
	initWorker();
	initRCManager();
	initTranslator();
}

}
