# Parity fixtures

Each `<name>.json` is a synthetic MyRSM `/` payload. It is a JSON array of
`{SensorApp, SensorClass, SensorName, SensorUnit, SensorValue, SensorUpdateTime}`, with
`SensorValue` as a **string**, exactly as MyRSM serves it. The fixtures cover the output contract,
not any particular PC.

| File | Covers |
|---|---|
| `basic.json` | a typical CPU/GPU/drive spread using every unit in the connector's normalisation map |
| `duplicates.json` | identical `(app, class, name, unit)` rows, duplicates that only collide after cleaning, groups split by filters and by non-numeric values |
| `encoding.json` | `°C`, `Â°C`, `Ã©`, Latin-1 that isn't UTF-8, invalid/overlong/surrogate sequences, BOMs, NULs, Unicode whitespace, quotes, backslashes, newlines, emoji, case mapping |
| `edge.json` | empty/missing names and units, unknown units, `Yes/No`-style values, NaN/Inf/overflow/underflow, Python `float()` edge syntax, missing `SensorUpdateTime` |
| `empty.json` | `[]` (never-succeeded / stale output) |

An optional `<name>.options.json` lists the runs to do for that fixture (default: one run with
default options):

```json
[{"name": "default"},
 {"name": "shadow", "metric_prefix": "hwinfo_shadow", "host_label": "pc-2",
  "freshness_timeout": 20, "data_age": 1,
  "include_sensors": "", "exclude_sensors": "", "include_classes": "", "exclude_classes": "",
  "include_apps": "", "exclude_apps": "",
  "compat_source_label": false, "expect_sensor_series": 36}]
```

`data_age` is the age of the frozen last successful poll; `> freshness_timeout` gives a stale run.
`expect_sensor_series` is an optional guard on the number of `*_sensor_value` series.

The loader is strict: string fields must be strings and `SensorUpdateTime` an integer, so a fixture
can't be interpreted one way here and another way by the Python reference.

To add a real-world fixture from any machine running MyRSM:

```powershell
powershell -ExecutionPolicy Bypass -File tools/capture-fixture.ps1 -Name mypc
```

It saves `tests/fixtures/captured-mypc.json`, and the parity test picks it up automatically. The file
holds that machine's hardware names, so review it before committing.
