# REST API (v1)

Транспорт для аутентификации и чтения. Весь run-домен (старт/батчи/финиш,
offline-загрузка, радар) — WebSocket с бинарным protobuf:
см. `docs/RUN_LIFECYCLE.md` и `proto/envelope.proto`.

Базовый префикс: `/api/v1`. Ответы — JSON. Ошибки: `{"status":"error","message":"..."}`.

## Аутентификация (без токена)

### POST /api/v1/auth/login
```json
{"login": "pguser", "password": "secret"}
```
→ `200 {"status":"success","access_token":"<JWT>","ws_ticket":"<uuid>","expires_in":864000}`
→ `401` неверные креды, `400` битый JSON.

### POST /api/v1/auth/ws-ticket
Заголовок `Authorization: Bearer <JWT>` (или `{"access_token": "..."}` в теле).
→ `200 {"status":"success","ws_ticket":"<uuid>"}`
→ `401` токен невалиден.
Одноразовый тикет, TTL 30 c — только для WS-upgrade: `GET /ws?token=<ticket>`.

## Служебное (без токена)

### GET /api/v1/health
→ `200 {"status":"ok"}`

## Профиль и чтение (Bearer JWT обязателен)

### GET /api/v1/users/me
→ `200 {"status":"success","profile":{...,"stats":{...}}}`

### GET /api/v1/users/{user_id}
То же, для любого пользователя. `404` — нет такого.

### GET /api/v1/hexagons/{h3_index}/details
**Заглушка**: эхо `h3_index`, `owner_*`/`top_score` = `null`.

### GET /api/v1/runs
**Заглушка**: пустой список. История прогонов будет добавлена позже;
live-данные — по WS (`FinishRunResponse.summary`).
