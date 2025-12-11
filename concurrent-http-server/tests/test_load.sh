#!/bin/bash
set -euo pipefail

URL="http://localhost:8080/index.html"

if ! command -v ab >/dev/null 2>&1; then
    echo "[WARN] ab não encontrado, a saltar teste de carga."
    exit 0
fi

tmp=$(mktemp)
ab -q -n 1000 -c 50 "$URL" > "$tmp" 2>&1 || true
grep -E "Failed requests:|Requests per second:|Time per request:" "$tmp" || cat "$tmp"
rm -f "$tmp"
