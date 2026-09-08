#!/usr/bin/env bash
set -euo pipefail

label="${1:-cold}"
duration_seconds="${2:-360}"

exec python3 /home/slam/dev/pico_tn160/scripts/capture_thermal_drift.py \
    --label "$label" \
    --duration "$duration_seconds"
