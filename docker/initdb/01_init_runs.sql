-- PostGIS
CREATE EXTENSION IF NOT EXISTS postgis;

CREATE TABLE IF NOT EXISTS runs (
    id                     BIGSERIAL PRIMARY KEY,
    user_id                BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    status                 VARCHAR(20) NOT NULL DEFAULT 'active' 
                           CHECK (status IN ('active', 'finished', 'cancelled')),

    total_distance_meters  DOUBLE PRECISION NOT NULL DEFAULT 0.0,
    total_duration_seconds BIGINT NOT NULL DEFAULT 0,
    avg_pace_sec_per_km    INT NOT NULL DEFAULT 0,
    total_steps            INT NOT NULL DEFAULT 0,
    hexagons_captured      INT NOT NULL DEFAULT 0,
    game_points_earned     INT NOT NULL DEFAULT 0,
    route_line             GEOMETRY(LineString, 4326) DEFAULT NULL,
    
    summary_polyline       TEXT DEFAULT NULL,

    is_valid               BOOLEAN NOT NULL DEFAULT TRUE,
    validation_reason      VARCHAR(100) DEFAULT NULL,

    started_at             TIMESTAMP WITH TIME ZONE NOT NULL DEFAULT NOW(),
    finished_at            TIMESTAMP WITH TIME ZONE DEFAULT NULL,
    created_at             TIMESTAMP WITH TIME ZONE NOT NULL DEFAULT NOW()
);

CREATE INDEX IF NOT EXISTS idx_runs_user_started ON runs (user_id, started_at DESC);
CREATE INDEX IF NOT EXISTS idx_runs_status ON runs (status) WHERE status = 'active';
CREATE INDEX IF NOT EXISTS idx_runs_route_line ON runs USING GIST (route_line);

CREATE TABLE IF NOT EXISTS run_points (
    run_id       BIGINT NOT NULL REFERENCES runs(id) ON DELETE CASCADE,
    point_order  INT NOT NULL,

    latitude     DOUBLE PRECISION NOT NULL,
    longitude    DOUBLE PRECISION NOT NULL,
    altitude     REAL DEFAULT NULL,
    speed        REAL DEFAULT NULL,
    accuracy     REAL NOT NULL DEFAULT 0.0,

    h3_index     BIGINT NOT NULL, -- H3 Index (res=9) uint64_t
    step_count   INT DEFAULT NULL,

    recorded_at  TIMESTAMP WITH TIME ZONE NOT NULL,

    PRIMARY KEY (run_id, point_order)
);

CREATE INDEX IF NOT EXISTS idx_run_points_h3 ON run_points (h3_index);
CREATE INDEX IF NOT EXISTS idx_run_points_recorded ON run_points (run_id, recorded_at);