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
