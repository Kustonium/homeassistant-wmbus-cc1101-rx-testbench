#include <mosquitto.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "wmbus_radio/packet.h"

using json = nlohmann::json;
using esphome::wmbus_radio::Packet;
using esphome::wmbus_radio::Frame;
using esphome::wmbus_radio::link_mode_name;

namespace {

std::atomic_bool g_stop{false};
std::atomic_bool g_connected{false};

struct Config {
  std::string mqtt_host{"127.0.0.1"};
  int mqtt_port{1883};
  std::string mqtt_username{};
  std::string mqtt_password{};
  std::string client_id{"wmbus_cc1101_rx_testbench"};

  std::string input_topic{"wmbus_bridge/raw"};
  std::string output_topic{"wmbus_bridge/telegram_cc1101_sim"};
  std::string diag_topic{"wmbus/diag/cc1101_sim"};
  std::string listen_mode_hint{"auto"};

  bool simulate_fifo{true};
  int fifo_size{64};
  int fifo_threshold{32};
  bool drop_tail_below_threshold{false};
  bool publish_output{true};
  bool publish_diag{true};
  bool log_raw{false};
  bool log_output_hex{false};
  std::string log_level{"info"};
  std::string replay_file{};
  int replay_interval_ms{250};
};

Config cfg;
mosquitto *g_mosq = nullptr;

int level_rank(const std::string &level) {
  if (level == "error") return 0;
  if (level == "warn") return 1;
  if (level == "info") return 2;
  if (level == "debug") return 3;
  if (level == "trace") return 4;
  return 2;
}

bool should_log(const std::string &level) {
  return level_rank(level) <= level_rank(cfg.log_level);
}

template <typename... Args>
void logf(const std::string &level, const char *fmt, Args... args) {
  if (!should_log(level)) return;
  char buf[2048];
  std::snprintf(buf, sizeof(buf), fmt, args...);
  const char *prefix = "[I]";
  if (level == "error") prefix = "[E]";
  else if (level == "warn") prefix = "[W]";
  else if (level == "debug") prefix = "[D]";
  else if (level == "trace") prefix = "[T]";
  std::cout << prefix << " " << buf << std::endl;
}

std::string getenv_str(const char *name, const std::string &def = {}) {
  const char *v = std::getenv(name);
  if (v == nullptr || *v == '\0') return def;
  return std::string(v);
}

int getenv_int(const char *name, int def) {
  const char *v = std::getenv(name);
  if (v == nullptr || *v == '\0') return def;
  try { return std::stoi(v); } catch (...) { return def; }
}

bool getenv_bool(const char *name, bool def) {
  std::string v = getenv_str(name);
  std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) { return std::tolower(c); });
  if (v.empty()) return def;
  return v == "1" || v == "true" || v == "yes" || v == "on";
}

void load_config_from_env() {
  cfg.mqtt_host = getenv_str("MQTT_HOST", cfg.mqtt_host);
  cfg.mqtt_port = getenv_int("MQTT_PORT", cfg.mqtt_port);
  cfg.mqtt_username = getenv_str("MQTT_USERNAME", cfg.mqtt_username);
  cfg.mqtt_password = getenv_str("MQTT_PASSWORD", cfg.mqtt_password);
  cfg.client_id = getenv_str("MQTT_CLIENT_ID", cfg.client_id);

  cfg.input_topic = getenv_str("INPUT_TOPIC", cfg.input_topic);
  cfg.output_topic = getenv_str("OUTPUT_TOPIC", cfg.output_topic);
  cfg.diag_topic = getenv_str("DIAG_TOPIC", cfg.diag_topic);
  cfg.listen_mode_hint = getenv_str("LISTEN_MODE_HINT", cfg.listen_mode_hint);

  cfg.simulate_fifo = getenv_bool("SIMULATE_FIFO", cfg.simulate_fifo);
  cfg.fifo_size = getenv_int("FIFO_SIZE", cfg.fifo_size);
  cfg.fifo_threshold = getenv_int("FIFO_THRESHOLD", cfg.fifo_threshold);
  cfg.drop_tail_below_threshold = getenv_bool("DROP_TAIL_BELOW_THRESHOLD", cfg.drop_tail_below_threshold);
  cfg.publish_output = getenv_bool("PUBLISH_OUTPUT", cfg.publish_output);
  cfg.publish_diag = getenv_bool("PUBLISH_DIAG", cfg.publish_diag);
  cfg.log_raw = getenv_bool("LOG_RAW", cfg.log_raw);
  cfg.log_output_hex = getenv_bool("LOG_OUTPUT_HEX", cfg.log_output_hex);
  cfg.log_level = getenv_str("LOG_LEVEL", cfg.log_level);
  cfg.replay_file = getenv_str("REPLAY_FILE", cfg.replay_file);
  cfg.replay_interval_ms = getenv_int("REPLAY_INTERVAL_MS", cfg.replay_interval_ms);

  if (cfg.fifo_size < 1) cfg.fifo_size = 64;
  if (cfg.fifo_threshold < 1) cfg.fifo_threshold = 32;
  if (cfg.fifo_threshold > cfg.fifo_size) cfg.fifo_threshold = cfg.fifo_size;
}

std::string trim(const std::string &s) {
  size_t a = 0;
  while (a < s.size() && std::isspace(static_cast<unsigned char>(s[a]))) a++;
  size_t b = s.size();
  while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) b--;
  return s.substr(a, b - a);
}

std::string normalize_hex(std::string s) {
  s = trim(s);

  // rtl_wmbus style line: ...;;;0xAABBCC
  auto p = s.rfind("0x");
  auto p2 = s.rfind("0X");
  if (p != std::string::npos) s = s.substr(p + 2);
  else if (p2 != std::string::npos) s = s.substr(p2 + 2);

  std::string out;
  out.reserve(s.size());
  for (unsigned char c : s) {
    if (std::isspace(c) || c == ':' || c == '-' || c == '_') continue;
    out.push_back(static_cast<char>(std::toupper(c)));
  }
  return out;
}

int hex_val(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

bool hex_to_bytes(const std::string &hex, std::vector<uint8_t> &out, std::string &err) {
  if (hex.empty()) {
    err = "empty_hex";
    return false;
  }
  if (hex.size() % 2 != 0) {
    err = "odd_hex_length";
    return false;
  }
  out.clear();
  out.reserve(hex.size() / 2);
  for (size_t i = 0; i < hex.size(); i += 2) {
    int hi = hex_val(hex[i]);
    int lo = hex_val(hex[i + 1]);
    if (hi < 0 || lo < 0) {
      err = "non_hex_char_at_" + std::to_string(i);
      return false;
    }
    out.push_back(static_cast<uint8_t>((hi << 4) | lo));
  }
  return true;
}

std::string bytes_to_hex_upper(const std::vector<uint8_t> &data) {
  static const char *hex = "0123456789ABCDEF";
  std::string out;
  out.reserve(data.size() * 2);
  for (uint8_t b : data) {
    out.push_back(hex[b >> 4]);
    out.push_back(hex[b & 0x0F]);
  }
  return out;
}

std::string join_chunks(const std::vector<size_t> &chunks) {
  std::ostringstream oss;
  for (size_t i = 0; i < chunks.size(); i++) {
    if (i) oss << "+";
    oss << chunks[i];
  }
  return oss.str();
}

struct InputFrame {
  std::string raw_hex;
  std::string source{"unknown"};
  std::string chip{"unknown"};
  std::string mode{"auto"};
  int rssi{0};
  bool rssi_set{false};
  json original{};
};

bool parse_input_payload(const std::string &payload, InputFrame &in, std::string &err) {
  std::string p = trim(payload);
  if (p.empty()) {
    err = "empty_payload";
    return false;
  }

  if (!p.empty() && p.front() == '{') {
    try {
      in.original = json::parse(p);
      if (in.original.contains("raw")) in.raw_hex = in.original.value("raw", "");
      else if (in.original.contains("hex")) in.raw_hex = in.original.value("hex", "");
      else if (in.original.contains("packet")) in.raw_hex = in.original.value("packet", "");
      else if (in.original.contains("payload")) in.raw_hex = in.original.value("payload", "");
      else {
        err = "json_without_raw_hex_field";
        return false;
      }
      in.source = in.original.value("source", in.source);
      in.chip = in.original.value("chip", in.chip);
      in.mode = in.original.value("mode", in.original.value("listen_mode", in.mode));
      if (in.original.contains("rssi") && in.original["rssi"].is_number_integer()) {
        in.rssi = in.original["rssi"].get<int>();
        in.rssi_set = true;
      }
    } catch (const std::exception &e) {
      err = std::string("json_parse_failed: ") + e.what();
      return false;
    }
  } else {
    in.raw_hex = p;
    in.source = "plain_hex";
  }

  in.raw_hex = normalize_hex(in.raw_hex);
  return true;
}

std::vector<uint8_t> apply_fifo_sim(const std::vector<uint8_t> &bytes, std::vector<size_t> &chunks, bool &tail_dropped) {
  tail_dropped = false;
  chunks.clear();

  if (!cfg.simulate_fifo) {
    chunks.push_back(bytes.size());
    return bytes;
  }

  std::vector<uint8_t> out;
  out.reserve(bytes.size());

  const size_t threshold = static_cast<size_t>(std::max(1, cfg.fifo_threshold));
  size_t pos = 0;
  while (pos < bytes.size()) {
    const size_t rem = bytes.size() - pos;
    if (cfg.drop_tail_below_threshold && rem < threshold) {
      tail_dropped = true;
      break;
    }
    const size_t take = std::min(rem, threshold);
    chunks.push_back(take);
    out.insert(out.end(), bytes.begin() + pos, bytes.begin() + pos + take);
    pos += take;
  }

  return out;
}

void mqtt_publish_str(const std::string &topic, const std::string &payload, bool retain = false) {
  if (g_mosq == nullptr || topic.empty()) return;
  mosquitto_publish(g_mosq, nullptr, topic.c_str(), static_cast<int>(payload.size()), payload.data(), 0, retain);
}

void publish_diag_json(const json &diag) {
  if (!cfg.publish_diag) return;
  mqtt_publish_str(cfg.diag_topic, diag.dump());
}

void process_payload(const std::string &topic, const std::string &payload, const char *origin = "mqtt") {
  InputFrame input;
  std::string err;
  json diag;
  diag["event"] = "cc1101_rx_testbench";
  diag["origin"] = origin;
  diag["topic_in"] = topic;

  if (!parse_input_payload(payload, input, err)) {
    logf("warn", "[DROP] parse_failed topic=%s reason=%s", topic.c_str(), err.c_str());
    diag["ok"] = false;
    diag["drop_stage"] = "input_parse";
    diag["drop_reason"] = err;
    publish_diag_json(diag);
    return;
  }

  std::vector<uint8_t> bytes;
  if (!hex_to_bytes(input.raw_hex, bytes, err)) {
    logf("warn", "[DROP] invalid_hex topic=%s reason=%s", topic.c_str(), err.c_str());
    diag["ok"] = false;
    diag["drop_stage"] = "hex_parse";
    diag["drop_reason"] = err;
    diag["raw_hex_len"] = input.raw_hex.size();
    publish_diag_json(diag);
    return;
  }

  std::vector<size_t> chunks;
  bool tail_dropped = false;
  std::vector<uint8_t> delivered = apply_fifo_sim(bytes, chunks, tail_dropped);

  Packet packet;
  if (input.rssi_set) packet.set_rssi(static_cast<int8_t>(std::clamp(input.rssi, -128, 127)));
  auto *dst = packet.append_space(delivered.size());
  if (!delivered.empty()) std::memcpy(dst, delivered.data(), delivered.size());

  auto frame = packet.convert_to_frame();

  diag["source"] = input.source;
  diag["chip"] = input.chip;
  diag["mode_hint"] = input.mode;
  diag["rssi"] = input.rssi_set ? json(input.rssi) : json(nullptr);
  diag["input_bytes"] = bytes.size();
  diag["delivered_bytes"] = delivered.size();
  diag["simulate_fifo"] = cfg.simulate_fifo;
  diag["fifo_size"] = cfg.fifo_size;
  diag["fifo_threshold"] = cfg.fifo_threshold;
  diag["chunks"] = join_chunks(chunks);
  diag["tail_dropped"] = tail_dropped;
  diag["raw_got_len"] = packet.raw_got_len();
  diag["decoded_len"] = packet.decoded_len();
  diag["final_len"] = packet.final_len();
  diag["want_len"] = packet.want_len();
  diag["got_len"] = packet.got_len();
  diag["dll_crc_removed"] = packet.dll_crc_removed();
  diag["suffix_ignored"] = packet.suffix_ignored();
  diag["t1_symbols_total"] = packet.t1_symbols_total();
  diag["t1_symbols_invalid"] = packet.t1_symbols_invalid();

  if (cfg.log_raw) {
    diag["raw"] = input.raw_hex;
  }

  logf("info", "[IN ] origin=%s topic=%s chip=%s mode=%s rssi=%s input_len=%zu delivered_len=%zu",
       origin, topic.c_str(), input.chip.c_str(), input.mode.c_str(),
       input.rssi_set ? std::to_string(input.rssi).c_str() : "n/a", bytes.size(), delivered.size());
  logf("debug", "[SIM] fifo=%s size=%d threshold=%d chunks=%s tail_dropped=%s",
       cfg.simulate_fifo ? "on" : "off", cfg.fifo_size, cfg.fifo_threshold,
       join_chunks(chunks).c_str(), tail_dropped ? "true" : "false");

  if (!frame) {
    diag["ok"] = false;
    diag["drop_stage"] = packet.drop_stage();
    diag["drop_reason"] = packet.drop_reason();
    diag["drop_detail"] = packet.drop_detail();
    diag["truncated"] = packet.is_truncated();
    logf("warn", "[DROP] stage=%s reason=%s detail=%s truncated=%s",
         packet.drop_stage().c_str(), packet.drop_reason().c_str(), packet.drop_detail().c_str(),
         packet.is_truncated() ? "true" : "false");
    publish_diag_json(diag);
    return;
  }

  std::string out_hex = frame->as_hex();
  uint32_t meter_id = 0;
  const bool meter_ok = frame->try_get_meter_id(meter_id);

  diag["ok"] = true;
  diag["link_mode"] = link_mode_name(frame->link_mode());
  diag["format"] = frame->format();
  diag["output_bytes"] = frame->data().size();
  diag["output_topic"] = cfg.output_topic;
  if (meter_ok) diag["meter_id"] = meter_id;
  if (cfg.log_output_hex) diag["output_hex"] = out_hex;

  if (cfg.publish_output) {
    mqtt_publish_str(cfg.output_topic, out_hex, false);
    logf("info", "[OUT] topic=%s mode=%s format=%s len=%zu%s",
         cfg.output_topic.c_str(), link_mode_name(frame->link_mode()), frame->format().c_str(),
         frame->data().size(), meter_ok ? (std::string(" meter=") + std::to_string(meter_id)).c_str() : "");
    if (cfg.log_output_hex) logf("debug", "[HEX] %s", out_hex.c_str());
  } else {
    logf("info", "[OK ] publish_output=false mode=%s format=%s len=%zu", link_mode_name(frame->link_mode()),
         frame->format().c_str(), frame->data().size());
  }

  publish_diag_json(diag);
}

void on_connect(struct mosquitto *, void *, int rc) {
  if (rc == 0) {
    g_connected = true;
    logf("info", "Connected to MQTT %s:%d", cfg.mqtt_host.c_str(), cfg.mqtt_port);
    mosquitto_subscribe(g_mosq, nullptr, cfg.input_topic.c_str(), 0);
    logf("info", "Subscribed input_topic=%s", cfg.input_topic.c_str());
  } else {
    logf("error", "MQTT connect failed rc=%d", rc);
  }
}

void on_disconnect(struct mosquitto *, void *, int rc) {
  g_connected = false;
  if (!g_stop) logf("warn", "MQTT disconnected rc=%d", rc);
}

void on_message(struct mosquitto *, void *, const struct mosquitto_message *msg) {
  if (msg == nullptr || msg->payload == nullptr || msg->payloadlen <= 0) return;
  std::string topic = msg->topic ? msg->topic : "";
  std::string payload(static_cast<const char *>(msg->payload), static_cast<size_t>(msg->payloadlen));
  process_payload(topic, payload, "mqtt");
}

void handle_signal(int) {
  g_stop = true;
  if (g_mosq != nullptr) mosquitto_disconnect(g_mosq);
}

void replay_file_if_configured() {
  if (cfg.replay_file.empty()) return;
  std::ifstream f(cfg.replay_file);
  if (!f) {
    logf("warn", "Replay file cannot be opened: %s", cfg.replay_file.c_str());
    return;
  }

  logf("info", "Replay file enabled: %s", cfg.replay_file.c_str());
  std::string line;
  size_t n = 0;
  while (!g_stop && std::getline(f, line)) {
    line = trim(line);
    if (line.empty() || line[0] == '#') continue;
    n++;
    process_payload("replay_file", line, "file");
    if (cfg.replay_interval_ms > 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(cfg.replay_interval_ms));
    }
  }
  logf("info", "Replay finished frames=%zu", n);
}

}  // namespace

int main() {
  std::signal(SIGINT, handle_signal);
  std::signal(SIGTERM, handle_signal);

  load_config_from_env();

  logf("info", "wMBus CC1101 RX Testbench starting");
  logf("info", "input=%s output=%s diag=%s", cfg.input_topic.c_str(), cfg.output_topic.c_str(), cfg.diag_topic.c_str());
  logf("info", "fifo_sim=%s fifo_size=%d threshold=%d drop_tail_below_threshold=%s",
       cfg.simulate_fifo ? "true" : "false", cfg.fifo_size, cfg.fifo_threshold,
       cfg.drop_tail_below_threshold ? "true" : "false");

  mosquitto_lib_init();
  g_mosq = mosquitto_new(cfg.client_id.c_str(), true, nullptr);
  if (g_mosq == nullptr) {
    logf("error", "mosquitto_new failed");
    return 1;
  }

  if (!cfg.mqtt_username.empty()) {
    mosquitto_username_pw_set(g_mosq, cfg.mqtt_username.c_str(), cfg.mqtt_password.c_str());
  }

  mosquitto_connect_callback_set(g_mosq, on_connect);
  mosquitto_disconnect_callback_set(g_mosq, on_disconnect);
  mosquitto_message_callback_set(g_mosq, on_message);
  mosquitto_reconnect_delay_set(g_mosq, 1, 30, true);

  int rc = mosquitto_connect_async(g_mosq, cfg.mqtt_host.c_str(), cfg.mqtt_port, 30);
  if (rc != MOSQ_ERR_SUCCESS) {
    logf("error", "MQTT connect_async failed: %s", mosquitto_strerror(rc));
    mosquitto_destroy(g_mosq);
    mosquitto_lib_cleanup();
    return 2;
  }

  mosquitto_loop_start(g_mosq);

  // Optional one-shot replay, useful for regression tests from /share.
  if (!cfg.replay_file.empty()) {
    for (int i = 0; i < 50 && !g_stop && !g_connected; i++) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    replay_file_if_configured();
  }

  while (!g_stop) {
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
  }

  logf("info", "Stopping");
  mosquitto_loop_stop(g_mosq, true);
  mosquitto_destroy(g_mosq);
  mosquitto_lib_cleanup();
  return 0;
}
