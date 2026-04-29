# Home Assistant add-on repository: wMBus CC1101 RX Testbench

Add this repository in Home Assistant add-on store, then install **wMBus CC1101 RX Testbench**.

This is a development/test add-on, not a normal end-user integration.


Current add-on version: 0.1.4 adds visible FIFO chunk logging, padded 8-digit meter IDs, and periodic RX statistics.

## 0.1.6

Adds crowded-air filters for synthetic TX testing:

- `filter_input_len_min`
- `filter_input_len_max`
- `filter_input_lengths`
- `filter_meter_ids`
- `log_ignored`

Example for long fake T1 raw packets:

```yaml
filter_input_len_min: 400
filter_input_len_max: 500
log_ignored: false
```


## 0.1.6 fake-meter prefilter

Default dev filter: `filter_meter_ids: "12345678"` and `prefilter_meter_ids: true`.
The add-on first decodes the full raw input only to identify the meter ID, before FIFO/tail simulation. This keeps normal RF traffic out of the buffer test while still allowing `drop_tail_below_threshold: true` to intentionally truncate the selected fake meter afterwards.

Set `filter_meter_ids: ""` to process all received meters again.
