# Карта планов и тестов / Roadmap and test plan

Предложение → Согласовано → Разработка → Проверка → Бета → Стабильный выпуск.

Idea → Agreed → Development → Validation → Beta → Stable.

Для каждой задачи: проблема, ожидаемое поведение, затронутые платформы, проверка
результата и ссылка на Issue/PR. Наличие workflow не означает, что код прошёл его.

Each task records the problem, expected behavior, affected platforms, validation
and an Issue/PR link. A configured workflow does not imply passing code.

## Ближайшие этапы / Next stages

| ID | Приоритет / Priority | Задача / Task | Критерий завершения / Completion criterion | Статус / Status |
| --- | --- | --- | --- | --- |
| DEP-01 | P0 | Обновить ARM toolchain и системные зависимости / Update ARM toolchain and dependencies | Версии и поддержка проверены; API/patches и минимальный ABI подтверждены / Versions, patches and ABI verified | Анализ; сборки остановлены / Review; builds paused |
| CI-01 | P1 | Качество кода / Code quality | Независимые статические проверки и Python-регрессии, реальные ошибки видны / Independent checks with visible failures | Workflow добавлен; результаты в Actions / Added; see Actions |
| CI-02 | P1 | Очистка кэшей / Cache cleanup | Два свежих поколения сохранены, dry-run и отказ API проверены / Two generations preserved, dry-run and API failures tested | Workflow добавлен / Added |
| DEP-02 | P1 | Контроль обновлений / Dependency updates | Еженедельные PR без автослияния / Weekly PRs without auto-merge | Dependabot настроен / Configured |
| CI-03 | P1 | Linux и C++ / Linux and C++ | Полная сборка и обычные C++-тесты из одного commit / Build and tests from one commit | Подготовлено, пауза DEP-01 / Prepared, waiting for DEP-01 |
| CI-04 | P1 | ARM64 production | Без mock/debug-сервисов; manifest, SHA256, чистая конфигурация и ABI / Production flags, manifest, checksums, clean config and ABI | Подготовлено, пауза DEP-01 / Prepared, waiting for DEP-01 |
| UI-01 | P1 | Адаптация интерфейса / Responsive UI | Матрица ниже, снимки и ручной просмотр текста / Matrix below, screenshots and text review | Перенос в hosted CI / Moving to hosted CI |
| QC-01 | P1 | Полный XML-lint и переводы / Full XML lint and translations | Исправить baseline, не скрывая ошибки / Resolve existing failures without suppressing them | Требует работы / Pending |
| QC-02 | P1 | Результаты первого Code quality / Initial quality findings | Разобрать повторные XML-имена `unit_tab`/`unit_tab_label`, форматирование и предупреждения LVGL cleanup; отличить дефекты от ложных срабатываний / Review duplicate names, formatting and cleanup false positives | Найдено проверками / Detected by checks |
| CI-05 | P2 | Ночные проверки / Nightly tests | Slow/hidden, event loop, ASan, TSan, BATS; таймауты и отчёты / Bounded deep tests and reports | После CI-03 / After CI-03 |
| CI-06 | P2 | Покрытие и мутационные тесты / Coverage and mutation tests | Проверки доказывают поведение изменённого кода / Tests detect changes in behavior | После CI-05 / After CI-05 |
| REL-01 | P1 | Безопасный выпуск / Release workflow | dry-run → draft → подтверждение; Stable/Beta отдельно / Confirmation and separate channels | После CI-03/04 и аппаратной проверки / After build and hardware validation |
| PERF-01 | P2 | Память, изображения, анимации / Memory, images, animations | Замер до/после на одинаковом оборудовании без потери функций / Comparable measurements without lost features | Запланировано / Planned |

## Матрица проверок / Test matrix

| Область / Area | Сценарии / Scenarios | Доказательство / Evidence |
| --- | --- | --- |
| Дисплей / Display | 800×480, 480×800, 1024×600, 600×1024; светлая/тёмная тема / Light/dark | Снимки, доступность кнопок, отсутствие обрезки / Screenshots, reachable buttons, no clipping |
| Главная / Home | Нагреватели, вентиляторы, охлаждение, редактирование плиток / Heaters, fans, cooldown, tile editing | Сохранение состава и расположения после перезапуска / Layout survives restart |
| Филамент / Filament | Одна катушка; AMS 4/8/12/16/24; одна и несколько систем / Single spool and multiple AMS units | Выбор канала, вкладки, прокрутка, Spoolman/локальный вес / Selection, tabs, scroll, weight |
| Управление / Controls | Только доступные калибровки, PID, BED_SCREWS_ADJUST, движение / Capability discovery and movement | Mock-команды отдельно от физических испытаний / Mock commands separate from hardware |
| Файлы и печать / Files and printing | Предпросмотр, пауза, отмена, настройка, исключение объектов / Preview, pause, cancel, tune, exclude | Переходы и состояния / Transitions and state |
| Первый запуск / Onboarding | Ориентация, сеть, устройства, изображение, пропуск, калибровки / Orientation, network, devices, image, skipping, calibration | Чистый профиль и сохранение завершения / Clean profile and persistence |
| Установка и обновления / Installation and updates | Чистая установка, повторная установка, Stable/Beta, ошибка сети / Fresh install, repeat install, channels, network failure | Сохранение настроек, корректный отказ, проверка SHA / Preserved settings, safe failure, SHA validation |
| Recovery | Наличие плагина, выключатель, версия / Plugin discovery, switch, version | UI/API tests; возобновление на железе отдельно / Physical recovery separately |

Аппаратные проверки нагрева, движения, тача и восстановления печати проводятся
по отдельному разрешённому сценарию. CI не устанавливает ничего на принтер.

Hardware heating, movement, touch and recovery require a separately authorized
procedure. CI does not deploy to printers.

[Как устроены Actions / Workflow guide](DEVELOPMENT.md)
