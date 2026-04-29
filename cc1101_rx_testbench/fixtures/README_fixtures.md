# wMBus CC1101 RX testbench fixtures

Fixture: T1 max-length fake meter `12345678`.

## Positive case

- input RAW len: 435 bytes
- FIFO threshold: 32
- chunks: 32 x 13 + 19
- tail: 19 bytes
- expected output len: 256 bytes
- expected meter: 12345678

Files:
- `t1_max_435_fake_12345678.json` — original MQTT JSON payload from `wmbus_bridge/raw`
- `t1_max_435_fake_12345678.raw` — RAW hex only
- `t1_max_435_fake_12345678.expected` — expected final telegram hex

## Negative tail-drop case

Simulates CC1101-style tail loss:

- original input: 435 bytes
- delivered after tail drop: 416 bytes
- dropped tail: 19 bytes
- expected result: DROP / `t1_length_check` / `truncated`

Files:
- `t1_max_435_fake_12345678_taildrop_416.raw`
- `t1_max_435_fake_12345678_taildrop_416.drop.log`
