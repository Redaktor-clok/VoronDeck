# GitHub Actions

Служебные материалы CI собраны здесь / CI support files live here:

- `scripts/` — проверки и очистка кэшей / checks and cache maintenance.
- `tests/` — тесты этих инструментов / maintenance tool tests.
- `init-submodules/` — получение закреплённых библиотек / pinned submodule setup.
- [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md) — устройство процессов / workflow guide.
- [docs/ROADMAP.md](docs/ROADMAP.md) — планы и матрица тестов / roadmap and test matrix.

Файлы запуска остаются в [../workflows](../workflows), где GitHub обнаруживает их.
Workflow entry points stay in `../workflows`, as required by GitHub.
