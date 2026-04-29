# wMBus CC1101 RX Testbench add-on

Development add-on for testing the wMBus RX conversion path without a physical CC1101 radio.

It subscribes to raw packets published by the ESP/SX1262 debug tap, runs the copied RX packet logic from `esphome-wmbus-bridge-rawonly-dev`, and publishes the final wMBus telegram to a test MQTT topic.

Default flow:

```text
ESP SX1262 hidden RAW tap
  -> wmbus_bridge/raw
  -> this add-on
  -> Packet::convert_to_frame()
  -> wmbus_bridge/telegram_cc1101_sim
  -> your wmbusmeters bridge/add-on
```

## Default topics

Input:

```text
wmbus_bridge/raw
```

Output:

```text
wmbus_bridge/telegram_cc1101_sim
```

Diagnostics:

```text
wmbus/diag/cc1101_sim
```

## Accepted input payloads

Preferred JSON payload from ESP:

```json
{
  "event": "radio_raw",
  "chip": "SX1262",
  "listen_mode": "T1 only",
  "mode": "T1",
  "rssi": -118,
  "raw_len": 116,
  "hex_len": 232,
  "raw": "AABBCCDDEEFF..."
}
```

Plain HEX is also accepted for manual tests:

```text
AABBCCDDEEFF00112233
```

rtl_wmbus-style `...0xAABBCC` lines are accepted too.

## Important limitation

This is not a full CC1101 chip emulator. It does not test SPI, GDO IRQ timing, real FIFO behavior, RF sensitivity, register setup, or power issues.

It tests the software conversion path from a captured radio packet to the final wMBus telegram.


## 0.1.4 buffer visibility

This build prints the simulated CC1101 FIFO path for every packet:

```text
[SIM] fifo=on size=64 threshold=32 chunks=32+32+32+21 tail=21 delivered_len=245 tail_dropped=false
```

Meter IDs are now always padded to 8 digits in logs, for example `00089907` and `03534275`.

It also prints periodic statistics:

```text
[STAT] rx=100 ok=92 drop=8 parse_fail=0 hex_fail=0 decode_fail=8 crc_fail=0 truncated=0 tail_dropped=0 invalid_symbols=42 in_bytes=14500 delivered_bytes=14500 out_bytes=7084 ok_pct=92.0%
```

Configuration:

```yaml
stats_every_n: 50       # 0 disables count-based reports
stats_interval_s: 60    # 0 disables time-based reports
```

## 0.1.6 crowded-air filters

When the air is full of real meters and you only want to process synthetic TX frames, enable input filtering.

For the long synthetic raw packet visible as `input_len=435`:

```yaml
filter_input_len_min: 400
filter_input_len_max: 500
# or exact-length mode:
# filter_input_lengths: "435"
log_ignored: false
```

With this setting the add-on silently ignores normal 134/245 byte packets and only runs the RX/FIFO test on long frames.

Optional exact meter filter, after successful decoding:

```yaml
filter_meter_ids: "00089907,03534157"
```

Ignored packets are counted in `[STAT] ignored=...`. Set `log_ignored: true` only when debugging the filter itself; otherwise leave it disabled to keep the log clean.


## 0.1.6 fake-meter prefilter

Default: no meter filter. To isolate the fake TX meter, set `filter_meter_ids: "12345678"` and `prefilter_meter_ids: true`.
The add-on first decodes the full raw input only to identify the meter ID, before FIFO/tail simulation. This keeps normal RF traffic out of the buffer test while still allowing `drop_tail_below_threshold: true` to intentionally truncate the selected fake meter afterwards.

To process all received meters, keep `filter_meter_ids: ""` or set `prefilter_meter_ids: false`.
