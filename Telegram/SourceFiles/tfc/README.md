# tfc — форковый код TeleForge (C++)

Здесь лежит весь код, которого нет в upstream Telegram Desktop. Всё, что вне
`tfc/`, — upstream; при вливании новых слоёв конфликты ожидаются только здесь.

```
tfc/
  tf_infra  tf_lang  tf_settings  tf_state  tf_ui_settings
  tf_url_handlers  tf_worker              корневые модули
  data/         хранилище форка: БД, сущности, content hash
  features/     plugins filters forward spy ghost presence
                teleforge sync message_shot streamer_mode
                subscription translator
  ui/           экраны настроек, боксы, стили, иконки
  utils/        общие помощники
  libs/         вендорные библиотеки (sqlite3, sqlite_orm, nlohmann json)
```

Rust-часть живёт рядом, в `tfr/`.

## Где `ayu` осталось намеренно

Эти места **нельзя** переименовывать — они не про наш код, а про совместимость
с уже установленными сборками и с лицензией.

### Пути миграции данных

`tdata/ayudata.db`, `tdata/ayu_settings.json`, `tdata/ayu/languages/` — имена
файлов **старых версий**. По ним миграция читает настройки, историю и архив
пользователя, который обновился. Переименование = потеря данных при апгрейде.

### Публичные ссылки

`tg://ayusettings` и `tg://ayu` оставлены алиасами рядом с `tg://tfsettings`:
это внешние ссылки, их могли сохранить в закладки или опубликовать в канале.

### ID поиска настроек

Часть секций настроек использует id вида `ayu/...`. Они сериализуются на диск в
`recentSettingsSearches` аккаунта и входят в диплинки `tg://settings/...`.
Переименование требует маппинга при чтении, иначе пропадут недавние поиски и
сломаются внешние ссылки.

### Атрибуция

Шапки файлов «This is the source code of AyuGram for Desktop» — требование
лицензии исходного проекта. `ayugram::` в сабмодулях `lib_icu`, `lib_tl`,
`codegen` — их код, не наш.

## Чего здесь больше нет

Инфраструктуры AyuGram: автообновление смотрит на наши GitHub Releases
(`lavrentijav/TeleForge`), тег краш-репортов — `teleforge-desktop@`.
