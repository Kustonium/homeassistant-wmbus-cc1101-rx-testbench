#!/usr/bin/env bash
set -euo pipefail

OPTIONS=/data/options.json

json_get() {
  local key="$1"
  local def="${2:-}"
  jq -r --arg key "$key" --arg def "$def" '.[$key] // $def' "$OPTIONS"
}

json_bool() {
  local key="$1"
  local def="${2:-false}"
  jq -r --arg key "$key" --argjson def "$def" '.[$key] // $def' "$OPTIONS"
}

MQTT_MODE="$(json_get mqtt_mode auto)"
MQTT_HOST="$(json_get mqtt_host '')"
MQTT_PORT="$(json_get mqtt_port 1883)"
MQTT_USERNAME="$(json_get mqtt_username '')"
MQTT_PASSWORD="$(json_get mqtt_password '')"

if [[ "$MQTT_MODE" == "auto" || "$MQTT_MODE" == "ha" ]]; then
  echo "[wmbus-cc1101-testbench] Trying Home Assistant MQTT service discovery..."
  if [[ -n "${SUPERVISOR_TOKEN:-}" ]]; then
    MQTT_JSON="$(curl -fsS -H "Authorization: Bearer ${SUPERVISOR_TOKEN}" http://supervisor/services/mqtt || true)"
    if [[ -n "$MQTT_JSON" && "$MQTT_JSON" != "null" ]]; then
      MQTT_HOST_DISC="$(echo "$MQTT_JSON" | jq -r '.data.host // empty')"
      MQTT_PORT_DISC="$(echo "$MQTT_JSON" | jq -r '.data.port // empty')"
      MQTT_USERNAME_DISC="$(echo "$MQTT_JSON" | jq -r '.data.username // empty')"
      MQTT_PASSWORD_DISC="$(echo "$MQTT_JSON" | jq -r '.data.password // empty')"
      [[ -n "$MQTT_HOST_DISC" ]] && MQTT_HOST="$MQTT_HOST_DISC"
      [[ -n "$MQTT_PORT_DISC" ]] && MQTT_PORT="$MQTT_PORT_DISC"
      [[ -n "$MQTT_USERNAME_DISC" ]] && MQTT_USERNAME="$MQTT_USERNAME_DISC"
      [[ -n "$MQTT_PASSWORD_DISC" ]] && MQTT_PASSWORD="$MQTT_PASSWORD_DISC"
    fi
  fi
fi

if [[ -z "$MQTT_HOST" ]]; then
  echo "[wmbus-cc1101-testbench] ERROR: MQTT host is empty. Set mqtt_mode=external and mqtt_host, or enable HA MQTT service." >&2
  exit 1
fi

export MQTT_HOST MQTT_PORT MQTT_USERNAME MQTT_PASSWORD
export MQTT_CLIENT_ID="wmbus_cc1101_rx_testbench_$(hostname)"
export INPUT_TOPIC="$(json_get input_topic 'wmbus_bridge/raw')"
export OUTPUT_TOPIC="$(json_get output_topic 'wmbus_bridge/telegram_cc1101_sim')"
export DIAG_TOPIC="$(json_get diag_topic 'wmbus/diag/cc1101_sim')"
export LISTEN_MODE_HINT="$(json_get listen_mode_hint auto)"
export SIMULATE_FIFO="$(json_bool simulate_fifo true)"
export FIFO_SIZE="$(json_get fifo_size 64)"
export FIFO_THRESHOLD="$(json_get fifo_threshold 32)"
export DROP_TAIL_BELOW_THRESHOLD="$(json_bool drop_tail_below_threshold false)"
export PUBLISH_OUTPUT="$(json_bool publish_output true)"
export PUBLISH_DIAG="$(json_bool publish_diag true)"
export LOG_RAW="$(json_bool log_raw false)"
export LOG_OUTPUT_HEX="$(json_bool log_output_hex false)"
export LOG_LEVEL="$(json_get log_level info)"
export REPLAY_FILE="$(json_get replay_file '')"
export REPLAY_INTERVAL_MS="$(json_get replay_interval_ms 250)"

echo "[wmbus-cc1101-testbench] MQTT: ${MQTT_HOST}:${MQTT_PORT} user=$([[ -n "$MQTT_USERNAME" ]] && echo yes || echo no)"
echo "[wmbus-cc1101-testbench] input=${INPUT_TOPIC} output=${OUTPUT_TOPIC} diag=${DIAG_TOPIC}"

exec /usr/bin/cc1101_rx_testbench
