// Copyright @Radolyn, 2026
#include "ayu/ui/settings/settings_teleforge_sync.h"

#include "ayu/features/sync/teleforge_sync.h"
#include "ayu/features/teleforge/teleforge_core.h"
#include "ayu/features/teleforge/teleforge_storage.h"
#include "ayu/ui/settings/ayu_builder.h"
#include "ayu/ui/settings/settings_main.h"
#include "settings/settings_builder.h"
#include "settings/settings_common.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"
#include "ui/widgets/buttons.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"

#include <QtCore/QDateTime>

namespace Settings {

using namespace Builder;
using namespace AyBuilder;

namespace {

const auto kMeta = BuildHelper({
	.id = TeleForgeSync::Id(),
	.parentId = AyuMain::Id(),
	.title = u"Синхронизация"_q,
	.icon = &st::menuIconDownload,
}, [](SectionBuilder &builder) {
	auto ayu = AyuSectionBuilder(builder);
	const auto controller = builder.controller();

	builder.addSkip();
	builder.addSubsectionTitle(rpl::single(
		u"Синхронизация между устройствами"_q));

	ayu.addToggle({
		.id = u"teleforge/memorySync"_q,
		.title = rpl::single(u"Синхронизация через Telegram"_q),
		.getter = [=] {
			return TeleForge::LoadPersonalityCore().value_or(
				TeleForge::DefaultPersonalityCore()).memorySyncEnabled;
		},
		.setter = [=](bool v) {
			auto p = TeleForge::LoadPersonalityCore().value_or(
				TeleForge::DefaultPersonalityCore());
			p.memorySyncEnabled = v;
			p.updatedAt = QDateTime::currentDateTimeUtc();
			TeleForge::PersistPersonalityCore(p);
			if (v) {
				TeleForge::Sync::ensureSyncChat(
					&controller->session(),
					[=](bool ok, QString message) {
						controller->showToast(message);
					});
			}
		},
	});

	builder.add([&](const BuildContext &ctx) {
		v::match(ctx, [&](const WidgetContext &wctx) {
			const auto c = wctx.container;
			const auto syncNow = c->add(
				object_ptr<Ui::SettingsButton>(
					c,
					rpl::single(u"Синхронизировать сейчас"_q),
					st::settingsButtonNoIcon));
			syncNow->setClickedCallback([=] {
				TeleForge::Sync::syncNow(&controller->session(), [=](QString msg) {
					controller->showToast(msg);
				});
			});
		}, [](const SearchContext &) {});
	});
});

} // namespace

rpl::producer<QString> TeleForgeSync::title() {
	return rpl::single(u"Синхронизация"_q);
}

TeleForgeSync::TeleForgeSync(
	QWidget *parent,
	not_null<Window::SessionController*> controller)
: Section(parent, controller) {
	setupContent();
}

void TeleForgeSync::setupContent() {
	const auto content = Ui::CreateChild<Ui::VerticalLayout>(this);
	build(content, kMeta.build);
	Ui::ResizeFitChild(this, content);
}

Type TeleForgeSyncId() {
	return TeleForgeSync::Id();
}

} // namespace Settings
