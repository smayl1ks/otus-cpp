INSERT INTO teams (id, name, tag, color_hex)
VALUES
    (1, 'Red Team', 'RED', '#ff4444'),
    (2, 'Blue Team', 'BLU', '#4444ff'),
    (3, 'Green Team', 'GRN', '#44ff44')
ON CONFLICT (id) DO NOTHING;

SELECT setval(pg_get_serial_sequence('teams', 'id'), COALESCE((SELECT MAX(id) FROM teams), 1));

INSERT INTO users (
    id,
    team_id,
    username,
    email,
    password_hash,
    player_color_hex,
    role
)
SELECT
    i AS id,
    ((i % 3) + 1) AS team_id,
    'runner_' || i AS username,
    'runner_' || i || '@test.local' AS email,
    '$argon2id$v=19$m=65536,t=2,p=1$/Ex2DRB/vQZazrsxP5Wjkw$SMRZnss8UThR+CPGwvtEyXaxIVOkk6wqhuZxsU3o+C0' AS password_hash,
    CASE (i % 3)
        WHEN 0 THEN '#ff4444'
        WHEN 1 THEN '#4444ff'
        ELSE '#44ff44'
    END AS player_color_hex,
    'USER' AS role
FROM generate_series(1, 10000) AS s(i)
ON CONFLICT (username) DO UPDATE
SET
    team_id = EXCLUDED.team_id,
    password_hash = EXCLUDED.password_hash;

INSERT INTO users (
    id,
    team_id,
    username,
    email,
    password_hash,
    player_color_hex,
    role
)
VALUES (
    10001,
    NULL,
    'viewer',
    'viewer@test.local',
    '$argon2id$v=19$m=65536,t=2,p=1$/Ex2DRB/vQZazrsxP5Wjkw$SMRZnss8UThR+CPGwvtEyXaxIVOkk6wqhuZxsU3o+C0',
    '#ffffff',
    'USER'
)
ON CONFLICT (username) DO UPDATE
SET
    team_id = EXCLUDED.team_id,
    password_hash = EXCLUDED.password_hash,
    email = EXCLUDED.email;

SELECT setval(pg_get_serial_sequence('users', 'id'), COALESCE((SELECT MAX(id) FROM users), 1));
