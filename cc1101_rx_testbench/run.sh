#!/usr/bin/with-contenv bashio
set -euo pipefail

cfg() {
  local key="$1"
  local def="${2:-}"
  if bashio::config.exists "$key"; then
    bashio::config "$key"
  else
    printf '%s' "$def"
  fi
}

cfg_bool() {
  local key="$1"
  local def="${2:-false}"
  if bashio::config.exists "$key"; then
    bashio::config "$key"
  else
    printf '%s' "$def"
  fi
}

# New names follow the existing wMBus MQTT Bridge add-on convention:
# external_mqtt_host / external_mqtt_port / external_mqtt_username / external_mqtt_password
# Legacy mqtt_* names are still accepted for compatibility with 0.1.0/0.1.1.
MQTT_MODE="$(cfg mqtt_mode auto)"

MQTT_HOST="$(cfg external_mqtt_host '')"
[[ -z "$MQTT_HOST" || "$MQTT_HOST" == "null" ]] && MQTT_HOST="$(cfg mqtt_host '')"

MQTT_PORT="$(cfg external_mqtt_port '')"
[[ -z "$MQTT_PORT" || "$MQTT_PORT" == "null" ]] && MQTT_PORT="$(cfg mqtt_port 1883)"

MQTT_USERNAME="$(cfg external_mqtt_username '')"
[[ -z "$MQTT_USERNAME" || "$MQTT_USERNAME" == "null" ]] && MQTT_USERNAME="$(cfg mqtt_username '')"

MQTT_PASSWORD="$(cfg external_mqtt_password '')"
[[ -z "$MQTT_PASSWORD" || "$MQTT_PASSWORD" == "null" ]] && MQTT_PASSWORD="$(cfg mqtt_password '')"

use_ha_mqtt_service() {
  if ! bashio::services.available "mqtt"; then
    return 1
  fi

  bashio::log.info "MQTT service found, fetching credentials ..."

  MQTT_HOST="$(bashio::services mqtt "host")"
  MQTT_PORT="$(bashio::services mqtt "port")"
  MQTT_USERNAME="$(bashio::services mqtt "username")"
  MQTT_PASSWORD="$(bashio::services mqtt "password")"

  if [[ -z "$MQTT_HOST" || "$MQTT_HOST" == "null" ]]; then
    return 1
  fi

  return 0
}

use_core_mosquitto_fallback() {
  if getent hosts core-mosquitto >/dev/null 2>&1; then
    MQTT_HOST="core-mosquitto"
    MQTT_PORT="${MQTT_PORT:-1883}"
    bashio::log.warning "MQTT service discovery unavailable, but core-mosquitto resolves. Using core-mosquitto:${MQTT_PORT}."
    bashio::log.warning "If the broker requires auth, set external_mqtt_username and external_mqtt_password."
    return 0
  fi

  return 1
}

case "$MQTT_MODE" in
  ha)
    if ! use_ha_mqtt_service; then
      bashio::log.error "mqtt_mode=ha, but Home Assistant MQTT service is not available."
      bashio::log.error "Install/enable Mosquitto add-on or set mqtt_mode=external with external_mqtt_host."
      exit 1
    fi
    ;;

  external)
    if [[ -z "$MQTT_HOST" || "$MQTT_HOST" == "null" ]]; then
      bashio::log.error "mqtt_mode=external, but external_mqtt_host is empty."
      exit 1
    fi
    ;;

  auto)
    if use_ha_mqtt_service; then
      :
    elif [[ -n "$MQTT_HOST" && "$MQTT_HOST" != "null" ]]; then
      bashio::log.info "No HA MQTT service found, using configured external MQTT broker."
    elif use_core_mosquitto_fallback; then
      :
    else
      bashio::log.error "MQTT host is empty."
      bashio::log.error "Set mqtt_mode=external and external_mqtt_host, or enable the HA MQTT service."
      exit 1
    fi
    ;;

  *)
    bashio::log.error "Invalid mqtt_mode: $MQTT_MODE"
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
export REPLAY_FILE="$(cfg replay_file '')"
export REPLAY_INTERVAL_MS="$(cfg replay_interval_ms 250)"

bashio::log.info "MQTT: ${MQTT_HOST}:${MQTT_PORT} user=$([[ -n "$MQTT_USERNAME" ]] && echo yes || echo no)"
bashio::log.info "input=${INPUT_TOPIC} output=${OUTPUT_TOPIC} diag=${DIAG_TOPIC}"

exec /usr/bin/cc1101_rx_testbench
