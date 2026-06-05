// Copyright @Radolyn, 2026
#include "ayu/ui/settings/settings_spy.h"

#include "ayu/ayu_settings.h"
#include "ayu/features/spy/online_history_storage.h"
#include "ayu/features/teleforge/tf_archive_users_box.h"
#include "ayu/features/teleforge/tf_peer_archive.h"
#include "ayu/ui/settings/ayu_builder.h"
#include "ayu/ui/settings/settings_ayu_utils.h"
#include "ayu/ui/settings/settings_main.h"
#include "lang_auto.h"
#include "settings/settings_builder.h"
#include "settings/settings_common.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"

namespace Settings {

using namespace Builder;
using namespace AyBuilder;

namespace {

void BuildSpyEssentials(SectionBuilder &builder, AyuSectionBuilder &ayu) {
	const auto controller = builder.controller();

	builder.addSubsectionTitle(tr::ayu_SpyEssentialsHeader());

	ayu.addToggle({
		.id = u"teleforge/spyMode"_q,
		.altIds = { u"ayu/spyMode"_q },
		.title = rpl::single(u"Режим шпиона (глобально)"_q),
		.getter = [] {
			return TeleForge::Spy::spyModeGloballyEnabled();
		},
		.setter = [](bool v) {
			TeleForge::Spy::setSpyModeGloballyEnabled(v);
		},
	});

	builder.addSkip();
	builder.addDividerText(rpl::single(
		u"Отслеживает онлайн-статус пользователей и сохраняет историю. "
		u"Для отдельного человека включите «Отслеживать онлайн» в профиле пользователя."_q));

	ayu.addSlider({
		.id = u"teleforge/spyRetentionDays"_q,
		.title = rpl::single(u"Хранить историю онлайна (дней)"_q),
		.steps = 365,
		.current = TeleForge::Spy::retentionDays() - 1,
		.indexToValue = [](int index) { return index + 1; },
		.onFinalChanged = [](int days) {
			TeleForge::Spy::setRetentionDays(days);
			TeleForge::Spy::purgeOldEvents();
		},
		.formatLabel = [](int x) { return QString::number(x); },
	});

	ayu.addToggle({
		.id = u"teleforge/peerArchive"_q,
		.title = rpl::single(u"Архив пользователей TeleForge"_q),
		.getter = [] {
			return AyuSettings::getInstance().peerArchiveEnabled();
		},
		.setter = [](bool v) {
			TeleForge::PeerArchive::setArchiveEnabled(v);
		},
	});

	builder.addButton({
		.id = u"teleforge/archiveUsersBrowse"_q,
		.title = rpl::single(u"Просмотреть всех пользователей"_q),
		.onClick = [=] {
			TeleForge::PeerArchive::ShowArchiveUsersBox(controller);
		},
	});

	builder.addSkip();
	builder.addDividerText(rpl::single(
		u"Сохраняет имена, юзернеймы, био, аватары (локально) и чаты, где встречался пользователь. "
		u"Имена, юзернеймы и био синхронизируются между устройствами; файлы аватаров — нет. "
		u"Удалённые аватары помечаются значком корзины. По умолчанию выключено."_q));

	ayu.addSectionDivider();
	builder.addSubsectionTitle(rpl::single(u"Сохранение сообщений"_q));

	ayu.addSettingToggle({
		.id = u"ayu/saveDeletedMessages"_q,
		.title = tr::ayu_SaveDeletedMessages(),
		.getter = &AyuSettings::saveDeletedMessages,
		.setter = &AyuSettings::setSaveDeletedMessages,
	});
	ayu.addSettingToggle({
		.id = u"ayu/saveMessagesHistory"_q,
		.title = tr::ayu_SaveMessagesHistory(),
		.getter = &AyuSettings::saveMessagesHistory,
		.setter = &AyuSettings::setSaveMessagesHistory,
	});

	ayu.addSectionDivider();

	ayu.addSettingToggle({
		.id = u"ayu/saveForBots"_q,
		.title = tr::ayu_MessageSavingSaveForBots(),
		.getter = &AyuSettings::saveForBots,
		.setter = &AyuSettings::setSaveForBots,
	});

	builder.addSkip();
	builder.addDividerText(tr::ayu_DeleteStubTextDescription());
	builder.add([&](const BuildContext &ctx) {
		v::match(ctx, [&](const WidgetContext &wctx) {
			const auto c = wctx.container;
			const auto field = c->add(
				object_ptr<Ui::InputField>(
					c,
					st::defaultInputField,
					Ui::InputField::Mode::SingleLine,
					tr::ayu_DeleteStubText(),
					TextWithTags{
						AyuSettings::getInstance().deleteStubText(),
					}),
				st::boxRowPadding);
			field->changes(
			) | rpl::on_next([=] {
				AyuSettings::getInstance().setDeleteStubText(
					field->getLastText().trimmed());
			}, field->lifetime());
		}, [](const SearchContext &) {});
	});
}

const auto kMeta = BuildHelper({
	.id = AyuSpy::Id(),
	.parentId = AyuMain::Id(),
	.title = u"Шпион"_q,
	.icon = &st::menuIconShowAll,
}, [](SectionBuilder &builder) {
	auto ayu = AyuSectionBuilder(builder);
	builder.addSkip();
	BuildSpyEssentials(builder, ayu);
	builder.addSkip();
});

} // namespace

rpl::producer<QString> AyuSpy::title() {
	return rpl::single(u"Шпион"_q);
}

AyuSpy::AyuSpy(
	QWidget *parent,
	not_null<Window::SessionController*> controller)
: Section(parent, controller)
, _controller(controller) {
	setupContent();
}

void AyuSpy::setupContent() {
	const auto content = Ui::CreateChild<Ui::VerticalLayout>(this);
	build(content, kMeta.build);
	Ui::ResizeFitChild(this, content);
}

Type AyuSpyId() {
	return AyuSpy::Id();
}

} // namespace Settings
