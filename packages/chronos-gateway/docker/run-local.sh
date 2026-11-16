#!/usr/bin/env bash
# Run a SQL-only chronos-gateway on this machine with Docker, backed by
# PostgreSQL 18 in Docker. Works on macOS and Linux; needs only Docker.
#
#   packages/chronos-gateway/docker/run-local.sh          # build if needed, start
#   packages/chronos-gateway/docker/run-local.sh rebuild  # rebuild after code changes, start
#   packages/chronos-gateway/docker/run-local.sh stop     # stop and remove containers
#
# Ports on 127.0.0.1: control API ${CHRONOS_GATEWAY_CONTROL_PORT:-7400},
# PostgreSQL wire ${CHRONOS_GATEWAY_POSTGRES_PORT:-6543}. Workspace "local" has one
# SQL store, "app". Data persists in the Docker volume until "stop --clean".
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/../../.." && pwd)"
image=chronos-gateway-dev
network=chronos-gateway-local
build_volume=chronos-gateway-local-build
pg_container=chronos-gateway-local-pg
gw_container=chronos-gateway-local
control_port="${CHRONOS_GATEWAY_CONTROL_PORT:-7400}"
postgres_port="${CHRONOS_GATEWAY_POSTGRES_PORT:-6543}"
token="${CHRONOS_CONTROLLER_TOKEN:-local-dev-token}"

stop() {
    docker rm -f "$gw_container" "$pg_container" >/dev/null 2>&1 || true
    if [[ "${1:-}" == "--clean" ]]; then
        docker volume rm "$build_volume" >/dev/null 2>&1 || true
        docker network rm "$network" >/dev/null 2>&1 || true
    fi
    echo "chronos-gateway local containers stopped"
}

build() {
    docker build -q -t "$image" "$here" >/dev/null
    docker volume create "$build_volume" >/dev/null
    docker run --rm -v "$repo":/src -v "$build_volume":/build "$image" bash -c \
        "cmake -S packages/chronos-gateway -B /build -DCMAKE_BUILD_TYPE=Release \
               -DCHRONOS_GATEWAY_BUILD_TESTS=OFF >/dev/null && \
         cmake --build /build -j\$(nproc) --target chronos-gateway"
}

case "${1:-start}" in
    stop) stop "${2:-}"; exit 0 ;;
    rebuild) build ;;
    start)
        if ! docker run --rm -v "$build_volume":/build "$image" test -x /build/chronos-gateway 2>/dev/null; then
            build
        fi ;;
    *) echo "usage: $0 [start|rebuild|stop [--clean]]" >&2; exit 2 ;;
esac

docker network create "$network" >/dev/null 2>&1 || true
if ! docker ps --format '{{.Names}}' | grep -qx "$pg_container"; then
    docker rm -f "$pg_container" >/dev/null 2>&1 || true
    docker run -d --name "$pg_container" --network "$network" \
        -e POSTGRES_PASSWORD=postgres postgres:18 >/dev/null
fi
for _ in $(seq 60); do
    docker exec "$pg_container" pg_isready -U postgres >/dev/null 2>&1 && break
    sleep 1
done
for db in chronos_data chronos_metadata; do
    docker exec "$pg_container" psql -U postgres -tAc "SELECT 1 FROM pg_database WHERE datname = '$db'" \
        | grep -q 1 || docker exec "$pg_container" createdb -U postgres "$db"
done

docker rm -f "$gw_container" >/dev/null 2>&1 || true
docker run -d --name "$gw_container" --network "$network" \
    -p "127.0.0.1:${control_port}:7400" -p "127.0.0.1:${postgres_port}:6543" \
    -v "$build_volume":/build \
    -e CHRONOS_CONTROLLER_TOKEN="$token" \
    -e CHRONOS_METADATA_URL="postgresql://postgres:postgres@${pg_container}:5432/chronos_metadata" \
    -e CHRONOS_APP_DATA_URL="postgresql://postgres:postgres@${pg_container}:5432/chronos_data" \
    "$image" bash -c "
        ip=\$(hostname -i | awk '{print \$1}')
        cat > /tmp/gateway.json <<JSON
{
  \"listeners\": {
    \"control\": \"\$ip:7400\",
    \"postgres\": \"\$ip:6543\",
    \"postgres_advertise\": \"localhost:${postgres_port}\"
  },
  \"controller_token_env\": \"CHRONOS_CONTROLLER_TOKEN\",
  \"workspaces\": {
    \"local\": {
      \"metadata_url_env\": \"CHRONOS_METADATA_URL\",
      \"postgres\": {\"app\": {\"data_url_env\": \"CHRONOS_APP_DATA_URL\"}}
    }
  }
}
JSON
        exec /build/chronos-gateway --config /tmp/gateway.json" >/dev/null

for _ in $(seq 30); do
    curl -fsS "http://127.0.0.1:${control_port}/healthz" >/dev/null 2>&1 && break
    sleep 1
done
curl -fsS "http://127.0.0.1:${control_port}/healthz" >/dev/null || {
    docker logs "$gw_container" >&2; exit 1; }

cat <<INFO
chronos-gateway is running (workspace "local", SQL store "app").
  control API: http://127.0.0.1:${control_port}   token: ${token}
  PostgreSQL:  localhost:${postgres_port}   (credentials come from the control API)

Create a long-lived branch and get its database URL:
  curl -s -X POST http://127.0.0.1:${control_port}/v1/workspaces/local/branches \\
    -H "Authorization: Bearer ${token}" -H 'Content-Type: application/json' \\
    -d '{"branch_id": "agent-1", "sandbox": false, "databases": ["app"]}'

Logs: docker logs -f ${gw_container}      Stop: $0 stop
INFO
