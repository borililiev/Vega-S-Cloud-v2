#!/usr/bin/env bash
set -euo pipefail
PORT="${1:-38443}"
if command -v ss >/dev/null && ss -lnt | awk '{print $4}' | grep -Eq "(^|:)$PORT$"; then
  echo "ERROR: TCP $PORT appears in use. Choose another port first." >&2
  exit 2
fi
command -v docker >/dev/null || { echo 'Docker not installed' >&2; exit 2; }
docker compose version
[ -f .env ] || { echo 'Create .env with python3 scripts/generate_env.py' >&2; exit 2; }
docker compose config >/dev/null
echo 'Preflight OK. Check router forwarding separately.'
