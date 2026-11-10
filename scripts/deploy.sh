#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

IMAGE_NAME="${RUNNER_IMAGE:-ghcr.io/smayl1ks/otus-cpp/runner:latest}"

if [[ ! -f .env ]]; then
    echo "[deploy] .env not found, creating from .env.example"
    cp .env.example .env
fi

if grep -qE '^JWT_ACCESS_SECRET=(random_hex)?$' .env; then
    JWT_SECRET="$(openssl rand -hex 32)"
    sed -i.bak "s/^JWT_ACCESS_SECRET=.*/JWT_ACCESS_SECRET=${JWT_SECRET}/" .env && rm -f .env.bak
    echo "[deploy] generated new JWT_ACCESS_SECRET into .env"
fi

if [[ -n "${RUNNER_IMAGE:-}" ]]; then
    echo "[deploy] using RUNNER_IMAGE from .env: ${IMAGE_NAME}"
else
    echo "[deploy] trying to pull ${IMAGE_NAME} from GHCR..."
    if docker pull "${IMAGE_NAME}"; then
        echo "[deploy] image pulled"
    else
        echo "[deploy] pull failed (no image or no GHCR access), building locally..."
        docker build -t "${IMAGE_NAME}" .
    fi
fi

echo "[deploy] starting docker compose stack..."
docker compose up -d

echo "[deploy] done. Services:"
docker compose ps
