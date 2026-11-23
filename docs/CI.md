# Сборки и проверки / Builds and checks

[GitHub Actions](https://github.com/Redaktor-clok/VoronDeck/actions/workflows/ci.yml)

## RU

Изменения кода в `main` и pull requests запускают проверки установщика и обновлений,
разбор XML/Python, проверку параметров переводов, сборку Linux, C++-тесты и
проверку навигации интерфейса. Матрица интерфейса использует отдельные mock-профили:
800×480, 480×800, 1024×600 и 600×1024, светлую/тёмную темы, с AMS и без неё.
Снимки доступны в артефакте `linux-tests-<commit>` для визуального просмотра.
Проверка размеров снимков и навигации не заменяет проверку наложений текста человеком.

Задание ARM64 собирает production-пакет без mock-режима, удалённого управления и
отправки диагностики. Перед сохранением проверяются manifest, контрольные суммы,
архитектура, ABI и чистые настройки первого запуска. Артефакт
`VoronDeck-arm64-<commit>` содержит архив, `SHA256SUMS`, `verified.json` и сведения ELF.
Это результат CI указанного commit, а не новый опубликованный релиз.

Запуск можно повторить вручную через **Actions → VoronDeck CI → Run workflow**.
Отчёты и пакеты хранятся 14 дней. Проверки используют GitHub-hosted runners и
права только на чтение. Секреты принтера не нужны; CI ничего не устанавливает на
принтер и не меняет Stable/Beta-каналы.

Текущий набор не является полным upstream `land-gate`: глубокий XML-lint,
покрытие всех переводов, медленные/скрытые C++-тесты, ASan/TSan и аппаратные
проверки остаются отдельными этапами. В существующем полном наборе обнаружены
ошибки; они не подавляются через `continue-on-error` и не объявляются пройденными.

## EN

Code pushes to `main` and pull requests run installer/update regressions, XML and
Python parsing, translation-format checks, a Linux build, C++ tests and UI
navigation checks. Disposable mock profiles cover 800×480, 480×800, 1024×600 and
600×1024 in both themes, with and without AMS. The `linux-tests-<commit>` artifact
contains screenshots for visual review. Navigation and image-dimension assertions
do not replace human review for overlapping text.

The ARM64 job builds a production package with mocks, remote control and diagnostic
uploads disabled. It verifies the manifest, checksums, architecture, ABI and clean
first-run settings before retaining the package. `VoronDeck-arm64-<commit>` contains
the archive, `SHA256SUMS`, `verified.json` and ELF information. This is a CI artifact
for the indicated commit, not a newly published release.

Use **Actions → VoronDeck CI → Run workflow** to run checks manually. Artifacts
expire after 14 days. Jobs run on GitHub-hosted runners with read-only permissions,
without printer credentials. They do not deploy to printers or change Stable/Beta
update channels.

This is not the full upstream `land-gate`: deep XML lint, complete translation
coverage, slow/hidden C++ tests, ASan/TSan and hardware checks remain separate
stages. Known failures in the existing full suite are not suppressed with
`continue-on-error` or represented as passing.
