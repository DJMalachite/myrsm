# MyRsm

MyRsm reads sensor data from [HWiNFO](https://www.hwinfo.com/)'s shared-memory interface on Windows
and serves it over HTTP and WebSocket:

| Route | What |
|---|---|
| `GET /` (HTTP and WebSocket) | JSON array of readings (unchanged; see below) |
| `GET /metrics` | Prometheus text exposition (format 0.0.4) |
| `GET /healthz`, `GET /readyz` | JSON health; `200` when the exporter is up, `503` otherwise |

`/metrics` is a native port of
[HWINFOtoPrometheusConnector](https://github.com/DJMalachite/HWINFOtoPrometheusConnector) @ `2fb02f5`.
It emits the same metric names, label names, label values and value semantics, so Grafana
dashboards built on the connector keep working without query changes. The Python connector is no
longer needed in the pipeline:

```
before: HWiNFO → MyRsm (JSON :34567 /) → Python connector (:10445/metrics) → Prometheus → Grafana
after:  HWiNFO → MyRsm (JSON :34567 /  +  Prometheus :34567/metrics)    → Prometheus → Grafana
```

HWiNFO must have **Shared Memory Support** enabled (Settings). On the free version it switches off
every 12 hours.

## Install

Run `MyRsm-Setup-<version>.exe` (from the CI artifact or a GitHub Release) as an administrator.
It installs to `C:\Program Files\MyRsm` and registers the `MyRsm` service: Automatic (Delayed
Start), LocalSystem, restarted by Windows if it fails. It writes the settings page values to
`%ProgramData%\MyRsm\myrsm.ini`, optionally adds a Windows Firewall rule for the HTTP port, and
checks `/healthz` before it finishes. Upgrades install in place and keep the ini. An existing
`MyRsm` service from an older install, such as the per-user IFW installer or a manual copy, is
stopped and repointed, and its `--host-label`/`--httpport`/... arguments prefill the settings page.

Unattended:

```
MyRsm-Setup-<version>.exe /VERYSILENT /SUPPRESSMSGBOXES /HTTPPORT=34567 /HOSTLABEL=<name> /FWSCOPE=<prometheus-ip>
```

Further parameters are `/WSPORT=`, `/POLLMS=`, `/PREFIX=`, `/COMPATSOURCE=0|1` and `/FIREWALL=0|1`.
Uninstalling removes the service and the firewall rule, and asks before deleting
`%ProgramData%\MyRsm` (config and logs). A silent uninstall keeps it unless you pass `/PURGEDATA`.

MyRsm runs as **LocalSystem** because it needs to open HWiNFO's `Global\` shared memory and mutex
reliably, whoever is logged in.

Installer checklist: [docs/installer-test.md](docs/installer-test.md).

## Configuration

Settings come from, in increasing priority: built-in defaults, then `%ProgramData%\MyRsm\myrsm.ini`
(or `--config <file>`), then the command line. The service is started with no arguments, so you
change its settings by editing the ini and running `Restart-Service MyRsm`.

```ini
[myrsm]
http_port=34567
ws_port=34568
poll_ms=250
bind_address=            ; empty/"any" = all interfaces, or an IP address
metric_prefix=hwinfo
host_label=              ; empty = %COMPUTERNAME%
freshness_timeout=20
compat_source_label=false
include_sensors=         ; comma-separated, case-insensitive substrings
exclude_sensors=
include_classes=
exclude_classes=
include_apps=
exclude_apps=
```

Invalid or unknown keys are logged and ignored. The effective configuration is logged at startup.

| Option | ini key | Default | Notes |
|---|---|---|---|
| `--httpport` | `http_port` | `34567` | HTTP port for `/`, `/metrics`, `/healthz`, `/readyz` |
| `--wsport` | `ws_port` | `34568` | WebSocket port |
| `--pollms` | `poll_ms` | `250` | shared-memory poll interval, minimum 50 ms |
| `--bind-address` | `bind_address` | all interfaces | address both servers listen on |
| `--metric-prefix` | `metric_prefix` | `hwinfo` | passed through the connector's `sanitize_name()` |
| `--host-label` | `host_label` | `%COMPUTERNAME%` | value of the `host` label; set it to the connector's `EXPORTER_HOST` when migrating |
| `--freshness-timeout` | `freshness_timeout` | `20` | seconds without a fresh HWiNFO sample before sensor series are withheld |
| `--include-*` / `--exclude-*` | `include_*` / `exclude_*` | empty | comma-separated, case-insensitive substrings matched against `sensor_name` / `sensor_class` / `sensor_app` (connector `INCLUDE_SENSORS` etc.) |
| `--compat-source-label` | `compat_source_label` | off | adds `source="http"` to every sensor series, matching the post-MQTT connector |
| `--config` | | `%ProgramData%\MyRsm\myrsm.ini` | ini file to read |
| `--console` | | | run in the console instead of as a Windows service |

## Running

```
myrsm.exe [--console] [options above] [--version]
myrsm.exe service <install|uninstall|start|stop|status>
```

With no console attached and no `--console`, MyRsm expects to be started by the Windows Service
Control Manager as the `MyRsm` service. In console mode it also reads the ini.

`myrsm.exe service ...` manages the service through the SCM API:

- `install` creates the service for this `myrsm.exe`. If it already exists, `install` stops it and
  updates it in place. The binPath is quoted with no arguments; the service runs Automatic
  (Delayed Start) as LocalSystem, restarts after 5 s (three times, count reset after a day), and has
  a description.
- `uninstall` stops and removes the service.
- `start` and `stop` wait for RUNNING / STOPPED, for up to 30 s.
- `status` shows the state, start type, account, binary and last exit code.

`install`, `uninstall`, `start` and `stop` need an elevated prompt. Exit codes: 0 ok, 1 failed,
2 usage, 3 not elevated, 4 not installed. Stopping or uninstalling something that isn't running or
installed counts as success.

Service logs are in `%ProgramData%\MyRsm\logs\` (`logs-<branch>\` for non-release builds), 7 files
kept. When the service fails to start, it reports a service-specific exit code: `1` HTTP port in use,
`2` HTTP bind failed, `3` WebSocket port in use.

`%COMPUTERNAME%` is the upper-case NetBIOS name. The connector defaulted to Python's
`socket.gethostname()`, whose case can differ, so set `--host-label` explicitly when migrating.

### `/` JSON (unchanged)

An array of `{SensorApp, SensorClass, SensorName, SensorUnit, SensorValue, SensorUpdateTime}` objects.
Query parameters: `classRegex`, `nameRegex` and `getMinMaxAvg=1`. The WebSocket pushes the same
payload on every poll tick, filtered by the query parameters of the connection URL.

### `/metrics`

All families are gauges:

- `<prefix>_exporter_up`, `_source_up`, `_stale`, `_data_age_seconds`, `_freshness_timeout_seconds`,
  `_last_success_timestamp_seconds`, `_last_attempt_timestamp_seconds`, `_last_poll_duration_seconds`,
  `_last_http_status`, `_successful_polls_total`, `_failed_polls_total`, `_raw_items_last`,
  `_exported_items_last`
- `<prefix>_sensor_value`, `<prefix>_sensor_update_time_seconds`, `<prefix>_sensor_present`, with
  labels `host, sensor_app, sensor_class, sensor_name, sensor_unit, sensor_unit_raw, occurrence`
  (+ `source` with `--compat-source-label`)

Label values go through the connector's `clean_text()` (Latin-1 → UTF-8 mojibake repair, `Â°C` →
`°C`, NULs removed, whitespace stripped). `sensor_unit` is the connector's normalised unit
(`°C` → `celsius`, `MB/s` → `megabytes_per_second`, …). `occurrence` numbers rows that share
the same label set, in shared-memory order. Values are parsed from the same strings the `/` route
serves, so they match what the connector exported.

### Freshness: the one intentional difference

The connector treated data as fresh when its HTTP poll of MyRsm succeeded. MyRsm keeps serving
the last known readings when HWiNFO goes away (`DEAD` signature, unclean exit, or the free
version's 12-hour shared-memory limit). As a result, **the old pipeline could export frozen
values as live.** MyRsm now checks HWiNFO itself:

- A poll tick **succeeds** when the shared memory is attached, the HWiNFO mutex is acquired, the
  signature is not `DEAD`, and parsing finds at least one reading.
- `last_success_ts` moves forward only when a tick succeeds **and** HWiNFO's own `poll_time` has
  changed since the last tick (or on the very first read). `data_age_seconds` is therefore the age
  of the last *new* HWiNFO sample. It ranges from 0 up to HWiNFO's polling period (2 s by default).
- `stale = never succeeded OR now - last_success_ts > freshness_timeout`. While stale, no
  `*_sensor_*` families are written, only the `*_exporter_*` ones.
- A mutex wait timeout is counted as a failed poll, but on its own it doesn't change `source_up`
  or the cached data. Staleness only kicks in after the freshness window.
- While stale, and as soon as HWiNFO marks the memory `DEAD`, MyRsm detaches from the shared
  memory and re-attaches on the next tick. HWiNFO's mutex is only opened for the duration of each
  read, never held in between. Holding it would keep the mutex alive after HWiNFO exits and stop a
  restarted HWiNFO from publishing. A restarted HWiNFO is therefore picked up without restarting
  MyRsm.
- `last_http_status` is kept for compatibility: `200` when the last read succeeded, else `0`.

The freshness timeout must be longer than HWiNFO's sensor polling period. If you raise HWiNFO's
period above 20 s, raise `--freshness-timeout` to match.

Other small differences from the connector, none of which affect dashboard queries:

- No `python_*` / `process_*` metrics (they described the Python process, not the hardware).
- A read where every reading is removed by `--include-*/--exclude-*` still counts as a successful
  poll. The connector counted an empty filtered payload as a failure.
- `last_poll_duration_seconds` is the duration of the shared-memory read, not an HTTP round trip.
- Labels appear in the order listed above. `prometheus_client` sorts them alphabetically. Prometheus
  ignores label order.
- `/healthz` has the connector's keys and types. Key order and indentation differ.

## Build

Qt 6 (Core, HttpServer, Network, WebSockets, Test) and CMake ≥ 3.16 on Windows, plus ImageMagick's
`magick` for the icon.

```
cmake -B build -DCMAKE_PREFIX_PATH=<path-to-Qt6>
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

`-DMYRSM_BUILD_APP=OFF` builds only the exporter library and tests. That configuration doesn't need
HttpServer, WebSockets or ImageMagick. `-DMYRSM_BUILD_TESTS=OFF` skips the tests.

CI ([.github/workflows/build.yml](.github/workflows/build.yml)) builds on `windows-latest`, runs the
unit tests and the parity test, smoke-tests the HTTP routes, tests the service lifecycle against the
real SCM, builds the Inno Setup installer ([installer/myrsm.iss](installer/myrsm.iss), bundling the
matching `vc_redist.x64.exe`), and runs a silent install, upgrade and uninstall. It uploads the
portable folder (`myrsm-win64-<run>`) and the installer (`MyRsm-Setup-<run>`), and on `v*` tags
attaches the installer to the GitHub Release.

## Tests

- `tst_config` (Qt Test): ini parsing, validation and precedence (CLI > ini > defaults).
- `tst_promexporter` (Qt Test): unit normalisation, `sanitize_name`, mojibake repair, `safe_float`,
  occurrence numbering, stale/never-succeeded output, label escaping, and a check that the `/` JSON
  is byte-identical to the pre-refactor implementation.
- Parity test: `tools/run_parity.py` renders every `tests/fixtures/*.json` through the **verbatim
  reference connector** ([tools/reference/](tools/reference/)) and through `myrsm_render_fixture`
  (the same code as `/metrics`) with a frozen poll state. The series sets and values must be
  identical. See [tests/fixtures/README.md](tests/fixtures/README.md).

  ```
  pip install prometheus_client==0.26.0 requests
  python tools/run_parity.py --renderer build/Release/myrsm_render_fixture.exe
  ```

- Live comparison of two running exporters:

  ```
  python tools/compare_metrics.py --live http://<connector>:10445/metrics http://127.0.0.1:34567/metrics --tolerance 0.5
  ```

  Label sets must match exactly. Values may differ by `tolerance × max(1, |a|, |b|)`, because the
  two scrapes are not simultaneous. Larger drift is reported, and fails the run only with
  `--fail-on-drift`. `--prefix-a/--prefix-b` compare differently prefixed outputs (shadow runs).

Migrating from the connector: [docs/cutover.md](docs/cutover.md).

## License

GPLv3, see [LICENSE](LICENSE). `tools/reference/hwinfo_prom_exporter_2fb02f5.py` is a verbatim copy of
the MIT-licensed HWINFOtoPrometheusConnector and is used only as a test oracle.
