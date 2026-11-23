# Разработка и проверки / Development and testing

[Actions](https://github.com/Redaktor-clok/VoronDeck/actions) ·
[Планы и тесты / Roadmap and tests](ROADMAP.md) ·
[Ошибки и предложения / Issues](https://github.com/Redaktor-clok/VoronDeck/issues)

## RU

**Проверка качества кода / Code quality** запускается при изменениях основной
рабочих веток и main, в pull requests, вручную и раз в неделю. Отдельные задания проверяют
синтаксис Python/XML, параметры переводов, обработку JSON null, имена компонентов
XML и регистрацию AMS, опасные шаблоны C++, форматирование, установщик и обновления.
Ошибки одного задания не отменяют остальные проверки. Отчёты хранятся 14 дней;
ошибка остаётся ошибкой, даже если она существовала до включения Actions.

Это статические проверки и регрессионные Python-тесты, а не подтверждение полной
сборки C++, поведения интерфейса или работы оборудования. Проверки используют
Ubuntu 24.04 и Python 3.13. Версия clang-format закреплена в `requirements.txt`
для одинакового форматирования: это инструмент разработки, не библиотека пакета.
Версии инструментов и закреплённые коммиты библиотек печатаются в журнале.

**Очистка устаревших кэшей / Cache cleanup** ежедневно удаляет только старые
поколения кэшей компилятора VoronDeck. Для каждой ветки, набора зависимостей и
формата кэша сохраняются два последних поколения. Записи младше суток не
удаляются. Другие кэши, артефакты проверок и файлы Releases не затрагиваются.
Ручной запуск по умолчанию лишь показывает кандидатов; для удаления снимите
`Preview only`. Ошибка доступа не маскируется под успешную очистку.

**Dependabot** раз в неделю предлагает обновления GitHub Actions и Python-
инструментов через pull requests. Автоматического слияния нет. Обновление
clang-format приходит отдельно. Библиотеки C++ и ARM toolchain требуют отдельной
проверки локальных патчей, API и минимальной поддерживаемой системы.

Ни один из этих процессов не подключается к принтеру и не устанавливает пакеты.
Для проверок достаточно `contents: read`; право `actions: write` есть только
у очистки кэшей на основной ветке. Секреты устройств не нужны.

## EN

**Code quality** runs on working-branch and main changes, pull requests, manual dispatch and
weekly. Independent jobs check Python/XML syntax, translation placeholders,
JSON null handling, XML component names and AMS registrations, C++ patterns and
formatting, installer behavior and updates. One failed job does not cancel the
other checks. Reports are retained for 14 days. Existing violations remain
visible failures rather than being suppressed.

These are static checks and Python regressions, not proof of a complete C++ build,
UI behavior or hardware operation. Runners use Ubuntu 24.04 and Python 3.13.
The clang-format version in `requirements.txt` keeps formatting reproducible;
it is a development tool, not a runtime library. Logs record tool versions and
pinned library commits.

**Cache cleanup** runs daily, retaining two newest compiler-cache generations
per branch, dependency family and cache format. Entries younger than one day
are protected. Other caches, test artifacts and Releases are untouched. Manual
runs default to preview; uncheck `Preview only` to apply. Permission errors fail
the job instead of being reported as successful deletions.

**Dependabot** proposes weekly GitHub Actions and Python-tool updates as pull
requests without automatic merging. Formatter updates are separate. C++ library
and ARM toolchain upgrades require patch, API and minimum-system compatibility
checks.

No workflow connects to a printer or installs packages. Checks have read-only
repository access; only cache cleanup on main can write Actions caches. Device
credentials are not required.

## Что взято из подхода HelixScreen / Upstream workflow review

Изучены [workflows HelixScreen](https://github.com/prestonbrown/helixscreen/tree/main/.github/workflows).
Reviewed the upstream workflows; the adaptation follows VoronDeck's scope.

| Upstream | Назначение / Purpose | VoronDeck |
| --- | --- | --- |
| `quality`, `lint-xml` | Статика и структура UI / Static checks and UI structure | Отдельный Code quality; полный XML schema-lint ещё в плане / Dedicated checks; full schema lint planned |
| `cache-prune` | Старые поколения кэшей / Superseded cache generations | Адаптировано под наши ключи / Adapted to our keys |
| `build` | Сборка и регрессии / Build and regression tests | Linux/ARM workflow подготовлен, приостановлен до обновления зависимостей / Prepared, paused for dependency updates |
| `nightly` | Медленные, скрытые, стресс-тесты, ASan/TSan / Deep tests and sanitizers | После воспроизводимой базовой сборки / After reproducible baseline build |
| `test-quality-deep` | Покрытие изменений и мутационные тесты / Diff coverage and mutation tests | Следующий этап / Later stage |
| `ratchet-report` | Отчёт о росте и сложности кода / Code growth report | В плане; сначала актуализировать baseline / Planned after baseline review |
| `ccache-warm` | Предварительная сборка для прогрева / Prebuild to warm caches | После замера выгоды на новом toolchain / After measuring benefit on new toolchain |
| `ci-watch` | Слежение за сбоями CI / CI failure tracking | Пока штатные статусы Actions; отдельный бот после стабилизации / Native statuses first |
| `auto-milestone` | Автоматический triage Issues / Issue milestone assignment | Карта и Issue Forms; автоназначение пока не требуется / Roadmap and issue forms first |
| `release` | Сборка, подпись происхождения, публикация / Build, provenance and publishing | План: dry-run → draft → ручная публикация / Planned staged release |
| `cleanup-r2`, `recover-r2` | Хранилище Cloudflare R2 / R2 release storage | Не используется / Not used |
| `server-tests`, `telemetry-printers` | Их серверы и телеметрия / Upstream servers and telemetry | Не переносится / Not adopted |
| `esp32-build`, Android jobs | Другие платформы / Other platforms | В текущий Linux ARM64-процесс не входят / Outside current Linux ARM64 scope |

API: [GitHub cache management](https://docs.github.com/en/rest/actions/cache),
[Dependabot configuration](https://docs.github.com/en/code-security/reference/supply-chain-security/dependabot-options-reference).
