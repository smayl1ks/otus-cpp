CREATE TABLE IF NOT EXISTS teams (
    id BIGSERIAL PRIMARY KEY,
    name VARCHAR(100) UNIQUE NOT NULL,
    tag VARCHAR(10) UNIQUE NOT NULL,
    color_hex VARCHAR(7) NOT NULL DEFAULT '#ffffff',
    avatar_url VARCHAR(512) DEFAULT NULL,
    created_at TIMESTAMP WITH TIME ZONE DEFAULT NOW()
);

CREATE TABLE IF NOT EXISTS users (
    id BIGSERIAL PRIMARY KEY,
    team_id BIGINT REFERENCES teams(id) ON DELETE SET NULL,
    username VARCHAR(50) UNIQUE NOT NULL,
    email VARCHAR(255) UNIQUE NOT NULL,
    password_hash VARCHAR(255) NOT NULL,
    avatar_url VARCHAR(512) DEFAULT NULL,
    player_color_hex VARCHAR(7) NOT NULL DEFAULT '#ffffff',
    role VARCHAR(20) NOT NULL DEFAULT 'USER' CHECK (role IN ('USER', 'MODERATOR', 'ROOT')),
    created_at TIMESTAMP WITH TIME ZONE DEFAULT NOW(),
    last_active_at TIMESTAMP WITH TIME ZONE DEFAULT NOW(),
    updated_at TIMESTAMP WITH TIME ZONE DEFAULT NOW()
);

CREATE UNIQUE INDEX IF NOT EXISTS idx_users_email_lower ON users (LOWER(email));
CREATE UNIQUE INDEX IF NOT EXISTS idx_users_username_lower ON users (LOWER(username));

CREATE INDEX IF NOT EXISTS idx_users_role ON users (role) WHERE role != 'USER';

CREATE INDEX IF NOT EXISTS idx_users_last_active ON users (last_active_at DESC);
CREATE INDEX IF NOT EXISTS idx_users_team_id ON users(team_id);

CREATE TABLE IF NOT EXISTS user_stats (
    user_id BIGINT PRIMARY KEY REFERENCES users(id) ON DELETE CASCADE,
    total_distance_meters DOUBLE PRECISION NOT NULL DEFAULT 0.0,
    total_duration_seconds BIGINT NOT NULL DEFAULT 0,
    total_runs INT NOT NULL DEFAULT 0,
    total_game_points INT NOT NULL DEFAULT 0,
    current_held_hexagons INT NOT NULL DEFAULT 0,
    updated_at TIMESTAMP WITH TIME ZONE DEFAULT NOW()
);

CREATE INDEX IF NOT EXISTS idx_user_stats_hexagons ON user_stats(current_held_hexagons DESC);
CREATE INDEX IF NOT EXISTS idx_user_stats_game_points ON user_stats (total_game_points DESC);

CREATE OR REPLACE FUNCTION create_user_stats_row()
RETURNS TRIGGER AS $$
BEGIN
    INSERT INTO user_stats (user_id) VALUES (NEW.id);
    RETURN NEW;
END;
$$ LANGUAGE plpgsql;

CREATE OR REPLACE TRIGGER trg_after_user_insert
AFTER INSERT ON users
FOR EACH ROW
EXECUTE FUNCTION create_user_stats_row();

CREATE OR REPLACE FUNCTION update_updated_at_column()
RETURNS TRIGGER AS $$
BEGIN
    NEW.updated_at = NOW();
    RETURN NEW;
END;
$$ LANGUAGE plpgsql;

CREATE OR REPLACE TRIGGER trg_users_updated_at
BEFORE UPDATE ON users
FOR EACH ROW
EXECUTE FUNCTION update_updated_at_column();

CREATE OR REPLACE TRIGGER trg_user_stats_updated_at
BEFORE UPDATE ON user_stats
FOR EACH ROW
EXECUTE FUNCTION update_updated_at_column();