## Быстрый старт

Требуется Docker (с BuildKit) и docker compose.

```bash
./scripts/deploy.sh
```

Скрипт получает образ бекенда (`ghcr.io/smayl1ks/otus-cpp/runner:latest`), если его - собирает локально.

Затем поднимает весь стек: `docker compose up -d`.

`JWT_ACCESS_SECRET` генерируется автоматически при первом запуске (`openssl rand -hex 32`)
и сохраняется в `.env`. Секрет общий для всех runner-нод.

## Конфигурация

Основные параметры задаются в `.env` (см. `.env.example`): порты nginx
(`NGINX_PORT`, по умолчанию 80), PostgreSQL (`POSTGRES_PORT`, по умолчанию 5434),
 Grafana (`3500`), Prometheus (`9090`), секреты JWT и БД.

Порты для отладки Redis (master: `16379`, `16381`; sentinel: `36379`–`36381`).

## Документация

- [docs/system_design](docs/system_design) - дизайн системы
- [docs/GAME_RULES.md](docs/GAME_RULES.md), [docs/RUN_LIFECYCLE.md](docs/RUN_LIFECYCLE.md) - правила игры и жизненный цикл пробежек
- [monitoring/](monitoring) - конфигурация Prometheus и дашборды Grafana