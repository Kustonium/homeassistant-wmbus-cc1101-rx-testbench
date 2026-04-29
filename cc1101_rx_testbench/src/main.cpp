#include <mosquitto.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iomanip>
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
  int filter_input_len_min{0};
  int filter_input_len_max{0};
  std::string filter_input_lengths{};
  std::string filter_meter_ids{};
  bool prefilter_meter_ids{true};
  bool log_ignored{false};
  int stats_every_n{50};
  int stats_interval_s{60};
  std::string replay_file{};
  int replay_interval_ms{250};
};

Config cfg;
mosquitto *g_mosq = nullptr;

struct Stats {
  uint64_t rx{0};
  uint64_t ok{0};
  uint64_t drop{0};
  uint64_t parse_fail{0};
  uint64_t hex_fail{0};
  uint64_t decode_fail{0};
  uint64_t crc_fail{0};
  uint64_t truncated{0};
  uint64_t tail_dropped{0};
  uint64_t ignored{0};
  uint64_t invalid_symbols{0};
  uint64_t input_bytes{0};
  uint64_t delivered_bytes{0};
  uint64_t output_bytes{0};
  std::chrono::steady_clock::time_point last_report{std::chrono::steady_clock::now()};
};

Stats stats;

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
  cfg.filter_input_len_min = getenv_int("FILTER_INPUT_LEN_MIN", cfg.filter_input_len_min);
  cfg.filter_input_len_max = getenv_int("FILTER_INPUT_LEN_MAX", cfg.filter_input_len_max);
  cfg.filter_input_lengths = getenv_str("FILTER_INPUT_LENGTHS", cfg.filter_input_lengths);
  cfg.filter_meter_ids = getenv_str("FILTER_METER_IDS", cfg.filter_meter_ids);
  cfg.prefilter_meter_ids = getenv_bool("PREFILTER_METER_IDS", cfg.prefilter_meter_ids);
  cfg.log_ignored = getenv_bool("LOG_IGNORED", cfg.log_ignored);
  cfg.stats_every_n = getenv_int("STATS_EVERY_N", cfg.stats_every_n);
  cfg.stats_interval_s = getenv_int("STATS_INTERVAL_S", cfg.stats_interval_s);
  cfg.replay_file = getenv_str("REPLAY_FILE", cfg.replay_file);
  cfg.replay_interval_ms = getenv_int("REPLAY_INTERVAL_MS", cfg.replay_interval_ms);

  if (cfg.fifo_size < 1) cfg.fifo_size = 64;
  if (cfg.fifo_threshold < 1) cfg.fifo_threshold = 32;
  if (cfg.fifo_threshold > cfg.fifo_size) cfg.fifo_threshold = cfg.fifo_size;
  if (cfg.filter_input_len_min < 0) cfg.filter_input_len_min = 0;
  if (cfg.filter_input_len_max < 0) cfg.filter_input_len_max = 0;
  if (cfg.filter_input_len_max > 0 && cfg.filter_input_len_min > cfg.filter_input_len_max) cfg.filter_input_len_max = cfg.filter_input_len_min;
  if (cfg.stats_every_n < 0) cfg.stats_every_n = 0;
  if (cfg.stats_interval_s < 0) cfg.stats_interval_s = 0;
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

std::string meter_id_8(uint32_t meter_id) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%08u", static_cast<unsigned>(meter_id));
  return std::string(buf);
}

size_t fifo_tail_bytes(size_t len) {
  if (!cfg.simulate_fifo || cfg.fifo_threshold <= 0) return 0;
  const size_t threshold = static_cast<size_t>(cfg.fifo_threshold);
  return len % threshold;
}

std::vector<std::string> split_csv(std::string s) {
  for (char &c : s) {
    if (c == ';' || c == ' ' || c == '\t' || c == '\n' || c == '\r') c = ',';
  }
  std::vector<std::string> out;
  std::stringstream ss(s);
  std::string item;
  while (std::getline(ss, item, ',')) {
    item = trim(item);
    if (!item.empty()) out.push_back(item);
  }
  return out;
}

bool csv_contains_int(const std::string &csv, int value) {
  if (csv.empty()) return false;
  for (const auto &item : split_csv(csv)) {
    try {
      if (std::stoi(item) == value) return true;
    } catch (...) {}
  }
  return false;
}

bool input_length_allowed(size_t len, std::string &reason) {
  if (cfg.filter_input_len_min > 0 && len < static_cast<size_t>(cfg.filter_input_len_min)) {
    reason = "input_len_below_min";
    return false;
  }
  if (cfg.filter_input_len_max > 0 && len > static_cast<size_t>(cfg.filter_input_len_max)) {
    reason = "input_len_above_max";
    return false;
  }
  if (!cfg.filter_input_lengths.empty() && !csv_contains_int(cfg.filter_input_lengths, static_cast<int>(len))) {
    reason = "input_len_not_allowed";
    return false;
  }
  return true;
}

bool meter_allowed(const std::string &meter_id) {
  if (cfg.filter_meter_ids.empty()) return true;
  for (auto item : split_csv(cfg.filter_meter_ids)) {
    item = normalize_hex(item);
    if (item.size() < 8 && std::all_of(item.begin(), item.end(), [](unsigned char c){ return std::isdigit(c); })) {
      item = std::string(8 - item.size(), '0') + item;
    }
    if (item == meter_id) return true;
  }
  return false;
}

struct PrefilterResult {
  bool attempted{false};
  bool frame_ok{false};
  bool meter_ok{false};
  std::string meter_id{};
  std::string drop_stage{};
  std::string drop_reason{};
  std::string drop_detail{};
};

void mqtt_publish_str(const std::string &topic, const std::string &payload, bool retain = false);

void report_stats(bool force = false) {
  if (stats.rx == 0) return;

  const auto now = std::chrono::steady_clock::now();
  const auto elapsed_s = std::chrono::duration_cast<std::chrono::seconds>(now - stats.last_report).count();
  const bool by_count = cfg.stats_every_n > 0 && (stats.rx % static_cast<uint64_t>(cfg.stats_every_n) == 0);
  const bool by_time = cfg.stats_interval_s > 0 && elapsed_s >= cfg.stats_interval_s;
  if (!force && !by_count && !by_time) return;

  const double ok_pct = stats.rx ? (100.0 * static_cast<double>(stats.ok) / static_cast<double>(stats.rx)) : 0.0;
  logf("info", "[STAT] rx=%llu ok=%llu drop=%llu ignored=%llu parse_fail=%llu hex_fail=%llu decode_fail=%llu crc_fail=%llu truncated=%llu tail_dropped=%llu invalid_symbols=%llu in_bytes=%llu delivered_bytes=%llu out_bytes=%llu ok_pct=%.1f%%",
       static_cast<unsigned long long>(stats.rx),
       static_cast<unsigned long long>(stats.ok),
       static_cast<unsigned long long>(stats.drop),
       static_cast<unsigned long long>(stats.ignored),
       static_cast<unsigned long long>(stats.parse_fail),
       static_cast<unsigned long long>(stats.hex_fail),
       static_cast<unsigned long long>(stats.decode_fail),
       static_cast<unsigned long long>(stats.crc_fail),
       static_cast<unsigned long long>(stats.truncated),
       static_cast<unsigned long long>(stats.tail_dropped),
       static_cast<unsigned long long>(stats.invalid_symbols),
       static_cast<unsigned long long>(stats.input_bytes),
       static_cast<unsigned long long>(stats.delivered_bytes),
       static_cast<unsigned long long>(stats.output_bytes),
       ok_pct);

  if (cfg.publish_diag) {
    json j;
    j["event"] = "cc1101_rx_testbench_stats";
    j["rx"] = stats.rx;
    j["ok"] = stats.ok;
    j["drop"] = stats.drop;
    j["ignored"] = stats.ignored;
    j["parse_fail"] = stats.parse_fail;
    j["hex_fail"] = stats.hex_fail;
    j["decode_fail"] = stats.decode_fail;
    j["crc_fail"] = stats.crc_fail;
    j["truncated"] = stats.truncated;
    j["tail_dropped"] = stats.tail_dropped;
    j["invalid_symbols"] = stats.invalid_symbols;
    j["input_bytes"] = stats.input_bytes;
    j["delivered_bytes"] = stats.delivered_bytes;
    j["output_bytes"] = stats.output_bytes;
    j["ok_pct"] = ok_pct;
    mqtt_publish_str(cfg.diag_topic, j.dump(), false);
  }

  stats.last_report = now;
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

PrefilterResult decode_meter_from_full_input(const std::vector<uint8_t> &bytes, const InputFrame &input) {
  PrefilterResult r;
  r.attempted = true;

  Packet packet;
  if (input.rssi_set) packet.set_rssi(static_cast<int8_t>(std::clamp(input.rssi, -128, 127)));
  auto *dst = packet.append_space(bytes.size());
  if (!bytes.empty()) std::memcpy(dst, bytes.data(), bytes.size());

  auto frame = packet.convert_to_frame();
  if (!frame) {
    r.drop_stage = packet.drop_stage();
    r.drop_reason = packet.drop_reason();
    r.drop_detail = packet.drop_detail();
    return r;
  }

  r.frame_ok = true;
  uint32_t meter_id = 0;
  if (frame->try_get_meter_id(meter_id)) {
    r.meter_ok = true;
    r.meter_id = meter_id_8(meter_id);
  }
  return r;
}

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

void mqtt_publish_str(const std::string &topic, const std::string &payload, bool retain) {
  if (g_mosq == nullptr || topic.empty()) return;
  mosquitto_publish(g_mosq, nullptr, topic.c_str(), static_cast<int>(payload.size()), payload.data(), 0, retain);
}

void publish_diag_json(const json &diag) {
  if (!cfg.publish_diag) return;
  mqtt_publish_str(cfg.diag_topic, diag.dump());
}

void process_payload(const std::string &topic, const std::string &payload, const char *origin = "mqtt") {
  stats.rx++;

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
    stats.drop++;
    stats.parse_fail++;
    publish_diag_json(diag);
    report_stats();
    return;
  }

  std::vector<uint8_t> bytes;
  if (!hex_to_bytes(input.raw_hex, bytes, err)) {
    logf("warn", "[DROP] invalid_hex topic=%s reason=%s", topic.c_str(), err.c_str());
    diag["ok"] = false;
    diag["drop_stage"] = "hex_parse";
    diag["drop_reason"] = err;
    diag["raw_hex_len"] = input.raw_hex.size();
    stats.drop++;
    stats.hex_fail++;
    publish_diag_json(diag);
    report_stats();
    return;
  }

  std::string filter_reason;
  if (!input_length_allowed(bytes.size(), filter_reason)) {
    stats.ignored++;
    stats.input_bytes += bytes.size();
    diag["ok"] = false;
    diag["ignored"] = true;
    diag["ignore_stage"] = "input_length_filter";
    diag["ignore_reason"] = filter_reason;
    diag["input_bytes"] = bytes.size();
    diag["filter_input_len_min"] = cfg.filter_input_len_min;
    diag["filter_input_len_max"] = cfg.filter_input_len_max;
    diag["filter_input_lengths"] = cfg.filter_input_lengths;
    if (cfg.log_ignored) {
      logf("info", "[IGN] stage=input_length_filter reason=%s topic=%s chip=%s mode=%s rssi=%s input_len=%zu",
           filter_reason.c_str(), topic.c_str(), input.chip.c_str(), input.mode.c_str(),
           input.rssi_set ? std::to_string(input.rssi).c_str() : "n/a", bytes.size());
      publish_diag_json(diag);
    }
    report_stats();
    return;
  }

  PrefilterResult prefilter;
  if (cfg.prefilter_meter_ids && !cfg.filter_meter_ids.empty()) {
    prefilter = decode_meter_from_full_input(bytes, input);
    diag["prefilter_meter_ids"] = cfg.filter_meter_ids;
    diag["prefilter_attempted"] = true;
    diag["prefilter_frame_ok"] = prefilter.frame_ok;
    diag["prefilter_meter_ok"] = prefilter.meter_ok;
    if (prefilter.meter_ok) diag["prefilter_meter_id"] = prefilter.meter_id;
    if (!prefilter.frame_ok) {
      diag["prefilter_drop_stage"] = prefilter.drop_stage;
      diag["prefilter_drop_reason"] = prefilter.drop_reason;
      diag["prefilter_drop_detail"] = prefilter.drop_detail;
    }

    if (!prefilter.meter_ok || !meter_allowed(prefilter.meter_id)) {
      stats.ignored++;
      stats.input_bytes += bytes.size();
      diag["ok"] = false;
      diag["ignored"] = true;
      diag["ignore_stage"] = "meter_prefilter";
      diag["ignore_reason"] = prefilter.meter_ok ? "meter_id_not_allowed" : "meter_id_not_decodable_before_fifo_sim";
      diag["input_bytes"] = bytes.size();
      if (cfg.log_ignored) {
        logf("info", "[IGN] stage=meter_prefilter reason=%s meter=%s input_len=%zu filter_meter_ids=%s",
             prefilter.meter_ok ? "meter_id_not_allowed" : "meter_id_not_decodable_before_fifo_sim",
             prefilter.meter_ok ? prefilter.meter_id.c_str() : "n/a", bytes.size(), cfg.filter_meter_ids.c_str());
        publish_diag_json(diag);
      }
      report_stats();
      return;
    }

    logf("info", "[FLT] meter_prefilter=pass meter=%s input_len=%zu", prefilter.meter_id.c_str(), bytes.size());
  }

  std::vector<size_t> chunks;
  bool tail_dropped = false;
  std::vector<uint8_t> delivered = apply_fifo_sim(bytes, chunks, tail_dropped);
  const size_t tail_bytes = fifo_tail_bytes(bytes.size());

  stats.input_bytes += bytes.size();
  stats.delivered_bytes += delivered.size();
  if (tail_dropped) stats.tail_dropped++;

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
  diag["tail_bytes"] = tail_bytes;
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
  logf("info", "[SIM] fifo=%s size=%d threshold=%d chunks=%s tail=%zu delivered_len=%zu tail_dropped=%s",
       cfg.simulate_fifo ? "on" : "off", cfg.fifo_size, cfg.fifo_threshold,
       join_chunks(chunks).c_str(), tail_bytes, delivered.size(), tail_dropped ? "true" : "false");

  if (!frame) {
    diag["ok"] = false;
    diag["drop_stage"] = packet.drop_stage();
    diag["drop_reason"] = packet.drop_reason();
    diag["drop_detail"] = packet.drop_detail();
    diag["truncated"] = packet.is_truncated();
    stats.drop++;
    if (packet.drop_reason() == "dll_crc_failed" || packet.drop_stage().rfind("dll_crc", 0) == 0) stats.crc_fail++;
    else stats.decode_fail++;
    if (packet.is_truncated()) stats.truncated++;
    stats.invalid_symbols += packet.t1_symbols_invalid();
    logf("warn", "[DROP] stage=%s reason=%s input_len=%zu delivered_len=%zu tail_dropped=%s detail=%s truncated=%s",
         packet.drop_stage().c_str(), packet.drop_reason().c_str(), bytes.size(), delivered.size(),
         tail_dropped ? "true" : "false", packet.drop_detail().c_str(),
         packet.is_truncated() ? "true" : "false");
    publish_diag_json(diag);
    report_stats();
    return;
  }

  std::string out_hex = frame->as_hex();
  uint32_t meter_id = 0;
  const bool meter_ok = frame->try_get_meter_id(meter_id);
  const std::string meter_id_str = meter_ok ? meter_id_8(meter_id) : std::string();

  diag["ok"] = true;
  diag["link_mode"] = link_mode_name(frame->link_mode());
  diag["format"] = frame->format();
  diag["output_bytes"] = frame->data().size();
  diag["output_topic"] = cfg.output_topic;
  if (meter_ok) {
    diag["meter_id"] = meter_id_str;
    diag["meter_id_numeric"] = meter_id;
  }
  if (cfg.log_output_hex) diag["output_hex"] = out_hex;

  if (!meter_allowed(meter_id_str)) {
    stats.ignored++;
    diag["ok"] = false;
    diag["ignored"] = true;
    diag["ignore_stage"] = "meter_filter";
    diag["ignore_reason"] = "meter_id_not_allowed";
    diag["filter_meter_ids"] = cfg.filter_meter_ids;
    if (cfg.log_ignored) {
      logf("info", "[IGN] stage=meter_filter reason=meter_id_not_allowed meter=%s input_len=%zu delivered_len=%zu",
           meter_ok ? meter_id_str.c_str() : "n/a", bytes.size(), delivered.size());
      publish_diag_json(diag);
    }
    report_stats();
    return;
  }

  stats.ok++;
  stats.output_bytes += frame->data().size();
  stats.invalid_symbols += packet.t1_symbols_invalid();

  if (cfg.publish_output) {
    mqtt_publish_str(cfg.output_topic, out_hex, false);
    logf("info", "[OUT] topic=%s mode=%s format=%s len=%zu%s%s",
         cfg.output_topic.c_str(), link_mode_name(frame->link_mode()), frame->format().c_str(),
         frame->data().size(), meter_ok ? " meter=" : "", meter_ok ? meter_id_str.c_str() : "");
    if (cfg.log_output_hex) logf("debug", "[HEX] %s", out_hex.c_str());
  } else {
    logf("info", "[OK ] publish_output=false mode=%s format=%s len=%zu", link_mode_name(frame->link_mode()),
         frame->format().c_str(), frame->data().size());
  }

  publish_diag_json(diag);
  report_stats();
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
  logf("info", "stats_every_n=%d stats_interval_s=%d", cfg.stats_every_n, cfg.stats_interval_s);
  logf("info", "filters: input_len_min=%d input_len_max=%d input_lengths=%s meter_ids=%s prefilter_meter_ids=%s log_ignored=%s",
       cfg.filter_input_len_min, cfg.filter_input_len_max,
       cfg.filter_input_lengths.empty() ? "none" : cfg.filter_input_lengths.c_str(),
       cfg.filter_meter_ids.empty() ? "none" : cfg.filter_meter_ids.c_str(),
       cfg.prefilter_meter_ids ? "true" : "false",
       cfg.log_ignored ? "true" : "false");

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

  report_stats(true);
  logf("info", "Stopping");
  mosquitto_loop_stop(g_mosq, true);
  mosquitto_destroy(g_mosq);
  mosquitto_lib_cleanup();
  return 0;
}
