-- PostGIS
CREATE EXTENSION IF NOT EXISTS postgis;

CREATE TABLE IF NOT EXISTS hexagons (
    h3_index          BIGINT PRIMARY KEY, -- H3 res=9 (uint64_t)
    owner_user_id     BIGINT REFERENCES users(id) ON DELETE SET NULL,
    owner_team_id     BIGINT REFERENCES teams(id) ON DELETE SET NULL,

    top_score         INT NOT NULL DEFAULT 0, -- Game Points
    total_captures    INT NOT NULL DEFAULT 0,

    boundary          GEOMETRY(Polygon, 4326) DEFAULT NULL,

    captured_at       TIMESTAMP WITH TIME ZONE DEFAULT NOW(),
    updated_at        TIMESTAMP WITH TIME ZONE DEFAULT NOW()
);

CREATE INDEX IF NOT EXISTS idx_hexagons_owner_user ON hexagons(owner_user_id);
CREATE INDEX IF NOT EXISTS idx_hexagons_owner_team ON hexagons(owner_team_id);
CREATE INDEX IF NOT EXISTS idx_hexagons_boundary ON hexagons USING GIST(boundary);

CREATE TABLE IF NOT EXISTS hexagon_user_stats (
    h3_index               BIGINT NOT NULL,
    user_id                BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,

    game_points            INT NOT NULL DEFAULT 0,
    total_distance_meters  DOUBLE PRECISION NOT NULL DEFAULT 0.0,
    visits_count           INT NOT NULL DEFAULT 0,
    
    last_visited_at        TIMESTAMP WITH TIME ZONE DEFAULT NOW(),
    
    PRIMARY KEY (h3_index, user_id)
);

CREATE INDEX IF NOT EXISTS idx_hex_user_leaderboard 
    ON hexagon_user_stats (h3_index, game_points DESC);

CREATE INDEX IF NOT EXISTS idx_hex_user_stats_user 
    ON hexagon_user_stats (user_id);

CREATE TABLE IF NOT EXISTS hexagon_history (
    id                 BIGSERIAL PRIMARY KEY,
    h3_index           BIGINT NOT NULL,
    
    previous_owner_id  BIGINT REFERENCES users(id) ON DELETE SET NULL,
    previous_team_id   BIGINT REFERENCES teams(id) ON DELETE SET NULL,
    
    new_owner_id       BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    new_team_id        BIGINT REFERENCES teams(id) ON DELETE SET NULL,

    score_at_capture   INT NOT NULL CHECK (score_at_capture >= 0),
    
    captured_at        TIMESTAMP WITH TIME ZONE DEFAULT NOW()
);

CREATE INDEX IF NOT EXISTS idx_hex_history_h3 ON hexagon_history(h3_index, captured_at DESC);
CREATE INDEX IF NOT EXISTS idx_hex_history_new_owner ON hexagon_history(new_owner_id);
CREATE INDEX IF NOT EXISTS idx_hex_history_new_team ON hexagon_history(new_team_id);

CREATE OR REPLACE TRIGGER trg_hexagons_updated_at
BEFORE UPDATE ON hexagons
FOR EACH ROW
EXECUTE FUNCTION update_updated_at_column();