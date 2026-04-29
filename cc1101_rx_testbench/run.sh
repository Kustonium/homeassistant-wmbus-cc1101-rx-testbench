#!/usr/bin/with-contenv bashio
set -euo pipefail

bashio::log.info "wMBus CC1101 RX Testbench run.sh 0.1.6"

cfg() {
  local key="$1"
  local def="${2:-}"
  local val=""

  if bashio::config.exists "$key"; then
    val="$(bashio::config "$key" || true)"
  else
    val="$def"
  fi

  if [[ "$val" == "null" ]]; then
    val=""
  fi

  printf '%s' "$val"
}

cfg_bool() {
  local key="$1"
  local def="${2:-false}"
  cfg "$key" "$def"
}

MQTT_MODE="$(cfg mqtt_mode auto)"

# Keep the simple old option names visible in HA. Also accept external_* names
# for compatibility with previous test packages.
MQTT_HOST="$(cfg mqtt_host '')"
[[ -z "$MQTT_HOST" ]] && MQTT_HOST="$(cfg external_mqtt_host '')"

MQTT_PORT="$(cfg mqtt_port '')"
[[ -z "$MQTT_PORT" ]] && MQTT_PORT="$(cfg external_mqtt_port 1883)"
[[ -z "$MQTT_PORT" ]] && MQTT_PORT="1883"

MQTT_USERNAME="$(cfg mqtt_username '')"
[[ -z "$MQTT_USERNAME" ]] && MQTT_USERNAME="$(cfg external_mqtt_username '')"

MQTT_PASSWORD="$(cfg mqtt_password '')"
[[ -z "$MQTT_PASSWORD" ]] && MQTT_PASSWORD="$(cfg external_mqtt_password '')"

use_ha_mqtt_service() {
  if ! bashio::services.available "mqtt"; then
    bashio::log.warning "Home Assistant MQTT service is not available to this add-on."
    return 1
  fi

  bashio::log.info "MQTT service found, fetching credentials ..."

  MQTT_HOST="$(bashio::services mqtt "host" || true)"
  MQTT_PORT="$(bashio::services mqtt "port" || true)"
  MQTT_USERNAME="$(bashio::services mqtt "username" || true)"
  MQTT_PASSWORD="$(bashio::services mqtt "password" || true)"

  [[ "$MQTT_HOST" == "null" ]] && MQTT_HOST=""
  [[ "$MQTT_PORT" == "null" ]] && MQTT_PORT="1883"
  [[ "$MQTT_USERNAME" == "null" ]] && MQTT_USERNAME=""
  [[ "$MQTT_PASSWORD" == "null" ]] && MQTT_PASSWORD=""
  [[ -z "$MQTT_PORT" ]] && MQTT_PORT="1883"

  if [[ -z "$MQTT_HOST" ]]; then
    bashio::log.warning "MQTT service exists, but returned an empty host."
    return 1
  fi

  return 0
}

use_core_mosquitto_fallback() {
  if getent hosts core-mosquitto >/dev/null 2>&1; then
    MQTT_HOST="core-mosquitto"
    [[ -z "$MQTT_PORT" ]] && MQTT_PORT="1883"
    bashio::log.warning "Using DNS fallback core-mosquitto:${MQTT_PORT}."
    bashio::log.warning "If auth is required, set mqtt_username and mqtt_password."
    return 0
  fi
  return 1
}

case "$MQTT_MODE" in
  ha)
    if ! use_ha_mqtt_service; then
      bashio::log.error "mqtt_mode=ha, but Home Assistant MQTT service is unavailable or empty."
      exit 1
    fi
    ;;

  external)
    if [[ -z "$MQTT_HOST" ]]; then
      bashio::log.error "mqtt_mode=external, but mqtt_host is empty."
      bashio::log.error "Set mqtt_host, mqtt_port, mqtt_username and mqtt_password in add-on configuration."
      exit 1
    fi
    ;;

  auto)
    if use_ha_mqtt_service; then
      :
    elif [[ -n "$MQTT_HOST" ]]; then
      bashio::log.info "Using MQTT broker from add-on configuration."
    elif use_core_mosquitto_fallback; then
      :
    else
      bashio::log.error "MQTT host is empty."
      bashio::log.error "Set mqtt_mode=external and mqtt_host, or enable the HA MQTT service."
      exit 1
    fi
    ;;

  *)
    bashio::log.error "Invalid mqtt_mode: ${MQTT_MODE}"
    exit 1
    ;;
esac

export MQTT_HOST MQTT_PORT MQTT_USERNAME MQTT_PASSWORD
export MQTT_CLIENT_ID="wmbus_cc1101_rx_testbench_$(hostname)"
export INPUT_TOPIC="$(cfg input_topic 'wmbus_bridge/raw')"
export OUTPUT_TOPIC="$(cfg output_topic 'wmbus_bridge/telegram_cc1101_sim')"
export DIAG_TOPIC="$(cfg diag_topic 'wmbus/diag/cc1101_sim')"
export LISTEN_MODE_HINT="$(cfg listen_mode_hint auto)"
export SIMULATE_FIFO="$(cfg_bool simulate_fifo true)"
export FIFO_SIZE="$(cfg fifo_size 64)"
export FIFO_THRESHOLD="$(cfg fifo_threshold 32)"
export DROP_TAIL_BELOW_THRESHOLD="$(cfg_bool drop_tail_below_threshold false)"
export PUBLISH_OUTPUT="$(cfg_bool publish_output true)"
export PUBLISH_DIAG="$(cfg_bool publish_diag true)"
export LOG_RAW="$(cfg_bool log_raw false)"
export LOG_OUTPUT_HEX="$(cfg_bool log_output_hex false)"
export LOG_LEVEL="$(cfg log_level info)"
export FILTER_INPUT_LEN_MIN="$(cfg filter_input_len_min 0)"
export FILTER_INPUT_LEN_MAX="$(cfg filter_input_len_max 0)"
export FILTER_INPUT_LENGTHS="$(cfg filter_input_lengths '')"
export FILTER_METER_IDS="$(cfg filter_meter_ids '12345678')"
export PREFILTER_METER_IDS="$(cfg_bool prefilter_meter_ids true)"
export LOG_IGNORED="$(cfg_bool log_ignored false)"
export STATS_EVERY_N="$(cfg stats_every_n 50)"
export STATS_INTERVAL_S="$(cfg stats_interval_s 60)"
export REPLAY_FILE="$(cfg replay_file '')"
export REPLAY_INTERVAL_MS="$(cfg replay_interval_ms 250)"

bashio::log.info "MQTT: ${MQTT_HOST}:${MQTT_PORT} user=$([[ -n "${MQTT_USERNAME}" ]] && echo yes || echo no)"
bashio::log.info "input=${INPUT_TOPIC} output=${OUTPUT_TOPIC} diag=${DIAG_TOPIC}"

exec /usr/bin/cc1101_rx_testbench
