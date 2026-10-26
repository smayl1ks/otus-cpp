# Гайд для агента: мобильное приложение «Runner»

Задание для агента, который делает смартфон-клиент (Android / iOS / Flutter —
не важно). Документ описывает **что** клиент обязан делать, **в каком порядке**
строить и **где** лежат контракты. Вопросы «как устроен сервер» — в
`docs/RUN_LIFECYCLE.md`, здесь только клиентская сторона.

---

## 0. Модель системы — прочитать до строчки кода

Клиент **никогда не обращается к БД сервера**. PostgreSQL/Redis — внутреннее
хозяйство бэкенда (`docker/initdb/` — только чтобы понимать, что сервер
сохраняет; читать напрямую из приложения запрещено). Клиент общается ровно
двумя каналами:

| Канал | Для чего | Формат |
|---|---|---|
| **REST (HTTPS, JSON)** | login, обмен JWT → WS-тикет, профиль, чтение | `docs/REST_API.md` |
| **WebSocket + бинарный Protobuf** | весь run-домен: старт/батчи/финиш/отмена, offline-загрузка, радар, карта | `proto/envelope.proto` + `docs/RUN_LIFECYCLE.md` |

На телефоне своя локальная БД (Room / SQLite / Core Data) — только для
offline-очереди: неотправленные треки, флаги синка. Не дублируй туда серверные
данные как «истину».

## 1. Контракты в репозитории

```
proto/
  envelope.proto     — конверт WS: message_id/correlation_id + oneof payload
  session.proto      — жизненный цикл пробежки + offline-загрузка, RunSummary
  telemetry.proto    — LocationPoint, LocationBatch, BatchProcessResponse, AnticheatStatus
  common.proto       — SystemStatus (транспортные коды)
  gamemap.proto      — гексагоны, радар
docs/RUN_LIFECYCLE.md   — все сценарии с диаграммами (главный документ)
docs/RUN_ROLES.md       — пороги античита/валидации (что сервер отбросит)
scripts/ws_e2e_example.py — ИСПОЛНЯЕМЫЙ пример полного WS-цикла на python
tests/service_pipeline_test.cpp — те же сценарии с точки зрения сервера
```

**Генерация клиентского кода**: `protoc --<lang>_out ...` по файлам из `proto/`
(для Flutter — `protoc_plugin`, для Android — `protobuf-kotlin-lite`,
для iOS — SwiftProtobuf). Пример вызова и все зависимости — см. корневой
`CMakeLists.txt` (версии protoc ≥ 3.21). Поле `is_mock` в `LocationPoint`
заполнять честно `false`; отдельные поля сенсоров — `optional`, не подставляй
нули «на всякий случай».

## 2. Этапы работы агента (чек-лист)

### Этап 1 — Auth (REST)
1. `POST /api/v1/auth/login {login, password}` → `access_token` (JWT, срок 10 дней)
   и `ws_ticket` (одноразовый, TTL 30 c).
2. Хранить `access_token` в защищённом хранилище (Keystore/Keychain).
3. Перед подключением WS: `POST /api/v1/auth/ws-ticket` с `Authorization: Bearer`
   → свежий `ws_ticket`. Получать новый тикет **на каждое переподключение** —
   старый одноразовый.
4. Все авторизованные REST-ручки требуют `Authorization: Bearer <JWT>`;
   `401` = переставить токен (перелогин).

### Этап 2 — WS-транспорт
1. `GET wss://<host>/ws?token=<ws_ticket>` (dev: `ws://host:8081/ws`).
2. Каждое исходящее сообщение — бинарь `Envelope`: заполнить `message_id`
   (uuid4) и `protocol_version=1`.
3. Ответы приходят **не по порядку**: сопоставление запрос→ответ по
   `correlation_id` (это твой `message_id` из запроса). Не строй FIFO-очередь
   ожиданий.
4. Разбор `oneof payload` — маршрутизация по типу. Все серверные ответы уже
   содержат `system_status` (`common.SystemStatus`) и бизнес-`reject_reason`.
5. Соединение сервер держит 5 минут простоя (пингует сам); клиент может
   не слать свои пинги, но обязан уметь переподключиться в любой момент.

### Этап 3 — Online-пробежка
1. **Генерируй `session_id` (uuid4) сам** при нажатии «Старт» — это ключ
   идемпотентности всей пробежки. Он переживает обрывы и перезапуск приложения.
2. `StartRunRequest{session_id, team_id, visibility, mode=RUN_MODE_ONLINE,
   start_timestamp}` → в ответе `server_run_id`, `server_start_timestamp` и
   **лимиты**: `max_points_per_batch` (сейчас 100) и `batch_interval_hint_ms`
   (5 с). Соблюдай их — превышение = батч будет отвергнут античитом целиком.
3. Телеметрия: копи GPS-точки, отправляй `LocationBatch{session_id, batch_uuid
   (uuid4 на батч!), sequence_number (монотонно с 1), state, points[]}` каждые
   `batch_interval_hint_ms`. Точки внутри батча — по возрастанию `timestamp`.
4. Каждый батч подтверждается `BatchProcessResponse`: прогресс (дистанция,
   счётчик точек), `anticheat_status`, `last_sequence_number`, `retry_after_ms`
   при `SERVICE_UNAVAILABLE` (backpressure — подожди и продолжи).
5. Пауза — `LocationBatch` c `state=RUNNING_STATE_PAUSED` и пустым `points`
   (heartbeat, продлевает жизнь сессии).
6. Финиш — `FinishRunRequest` → `FinishRunResponse.summary` (`RunSummary`:
   дистанция, время, захваченные гексагоны, очки, `is_valid` +
   `validation_reason`). Пробежки < 100 м / < 60 с сервер сохранит как
   невалидные (`docs/RUN_ROLES.md`).
7. Отмена — `CancelRunRequest` (трек сохранится, очков не будет).

### Этап 4 — Обрыв связи и reconnect
1. WS упал → копи точки в локальную БД (одна запись = будущий батч:
   `batch_uuid`, `sequence_number`, точки, статус «не отправлено»).
2. Переподключился → `GetActiveRunRequest` (без полей — user_id берётся из
   авторизации соединения) → `ActiveRunSnapshot` с `last_sequence_number`.
3. Доиграй все сохранённые батчи с `sequence_number > last_sequence_number`
   (те же `LocationBatch` — сервер отличит replay по старым таймштампам).
4. Если приложение умерло и перезапустилось: `StartRunRequest` с **тем же
   `session_id`** вернёт `resumed=true` + `snapshot` вместо второй пробежки.

### Этап 5 — Полностью offline-пробежка и загрузка
Если сеть не вернулась до конца пробежки:
1. Сохрани локально: `session_id`, границы времени, все точки.
2. При появлении сети — трёхфазная загрузка (тот же WS):
   `OfflineRunInitRequest` (объяви `total_chunks`, `total_points`) → ответ
   содержит `max_points_per_chunk` (1000) и `accepted_chunk_indexes` —
   **доиграй чанки, начиная с недостающих**;
3. `OfflineRunChunkRequest{session_id, chunk_index, total_chunks, points}`
   — чанки можно слать в любом порядке, ретраи бесплатны (дедуп по
   `(session_id, chunk_index)`);
4. `OfflineRunCommitRequest` → `OfflineRunCommitResponse` с тем же `RunSummary`.
   Повторный commit вернёт сохранённый summary (`ALREADY_COMMITTED`).
5. Отметь пробежку в локальной БД как синхронизированную только после 2xx/успешного
   commit-ответа.

### Этап 6 — Остальное
- Профиль: `GET /api/v1/users/me` (статистика приходит из серверной БД).
- Карта/радар: подписка `SubscribeViewportRequest`, события `HexagonCaptureEvent`
  и `RadarPing` приходят сервером сами (сервер→клиент, отвечать не нужно).
- История прогонов: ручка `GET /api/v1/runs` пока заглушка — показывай локально
  кэшированные `RunSummary`.

## 3. Ограничения, за которые сервер накажет (античит)

Пороги — `docs/RUN_ROLES.md`; кратко, что обязан обеспечить клиент:
- GPS-точка с `accuracy > 30 м` будет отброшена — фильтруй на устройстве.
- Скорость между соседними точками > 35 км/ч, прыжок > 100 м → батч целиком
  отклонён (`ANTICHEAT_STATUS_SPEED_LIMIT_EXCEEDED` / `TELEPORTATION_DETECTED`).
- `timestamp` — epoch **миллисекунды** устройства, строго монотонно; точки
  «из будущего» (> 5 с от серверных часов) отбрасываются — не доверяй
  системному времени после ручной смены часового пояса, синхронизируйся по
  `server_timestamp_ms` из ответов.
- `is_mock=true` (фейковая локация) → батч отклонён, пользователь получит
  `USER_BLOCKED` на следующих стартах.

## 4. Коды ошибок и ретраи

- Транспорт: `common.SystemStatus` (`UNAUTHORIZED`, `INVALID_PAYLOAD`,
  `INTERNAL_SERVER_ERROR`, `SERVICE_UNAVAILABLE` + `retry_after_ms`).
- Бизнес: `reject_reason` в каждом ответе (перечисления в `session.proto`).
- Ретраи безопасны и дедупятся на сервере: батч — по `batch_uuid`, чанк — по
  `(session_id, chunk_index)`, финиш/commit — по `session_id` (вернут
  сохранённый `RunSummary`). **Не создавай новый `session_id` при ретрае.**

## 5. Порядок проверки агента

1. Кодген proto, сборка пустого приложения с конвертом.
2. Логин + ws-ticket на тестовом сервере (`docs/REST_API.md`), WS-handshake.
3. Online-пробежка против dev-сервера: сверяй `RunSummary` с примером
   `scripts/ws_e2e_example.py` (логин → старт → 2 батча → финиш → строки в БД).
4. Обрыв сети на середине (airplane mode) → reconnect → доигрывание батчей.
5. Полностью offline-пробежка → upload Init/Chunk/Commit после включения сети.
6. Пауза/heartbeat, убийство приложения и resume по `session_id`.

Dev-сервер: `docker compose up -d postgres`, затем
`POSTGRES_HOST=localhost POSTGRES_DB=run_uram_db POSTGRES_USER=postgres_user
POSTGRES_PASSWORD=postgres_password_secure JWT_ACCESS_SECRET=... ./runner
--logger.dir /tmp/runner/logs --server.tmp-dir /tmp/runner/tmp`
(пользователь создаётся в `users` напрямую, пароль — argon2id).
