# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

MyRsm is a Windows-only Qt6/C++17 application that reads sensor data from HWiNFO's shared memory
interface and re-exposes it over HTTP (JSON) and WebSocket. It can run either as an interactive
console app or as a native Windows Service (SCM-managed), auto-detected by whether a console window
is attached.

## Build

Requires Qt6 (Core, HttpServer, Network, WebSockets components) and CMake >= 3.16, on Windows.
Also requires ImageMagick's `magick` CLI on PATH (used to generate the multi-resolution `.ico` from
`logo-rsm.png` at configure/build time).

```
cmake -B build -DCMAKE_PREFIX_PATH=<path-to-Qt6-cmake-dir>
cmake --build build
```

Tests:
- `tst_promexporter` (Qt Test, [tests/tst_promexporter.cpp](tests/tst_promexporter.cpp)) — run with
  `ctest --test-dir build -C Release --output-on-failure`. Covers the exporter port and asserts the
  `/` JSON is byte-identical to a verbatim copy of the pre-refactor `toJson()`.
- Golden parity test: `python tools/run_parity.py --renderer build/Release/myrsm_render_fixture.exe`
  (needs `pip install prometheus_client==0.26.0 requests`). Renders every `tests/fixtures/*.json`
  through the verbatim reference connector in [tools/reference/](tools/reference/) and through
  `myrsm_render_fixture` (same code as `/metrics`) with a frozen poll state; any series/value
  difference fails. Don't edit `tools/reference/hwinfo_prom_exporter_2fb02f5.py` — it's the oracle.
- `-DMYRSM_BUILD_APP=OFF` builds only `myrsm_core` + tests (needs just Qt Core/Test, no
  HttpServer/WebSockets/ImageMagick) — handy for a local build when Qt lacks those modules.
- `test.http` is still a manual scratch file for an HTTP-client extension.

Versioning: `VERSION` and `PROJECT_VERSION_NAME` are derived from CI env vars (`CI_PIPELINE_ID`,
`CI_JOB_ID`, `CI_COMMIT_REF_SLUG`); locally they fall back to `1` / `localbuild`. These feed into
`myrsm.rc.in` (Windows version resource) and into the log folder path (see below).

CI: [.github/workflows/build.yml](.github/workflows/build.yml) builds on `windows-latest` (Qt via
`jurplel/install-qt-action`, modules `qthttpserver qtwebsockets`), maps GitHub context onto the
GitLab-style env vars above (`run_number` → `CI_PIPELINE_ID`/`CI_JOB_ID`, slugified `ref_name` →
`CI_COMMIT_REF_SLUG`), runs `ctest` and the parity test, runs the install/deploy script to produce
a portable folder with Qt DLLs, smoke-tests `--version` and the HTTP routes (no HWiNFO on the
runner, so `/metrics` must report stale and `/healthz` 503), and uploads it as the
`myrsm-win64-<run>` artifact. There's no local-Qt
assumption for development — pushing a branch is the expected way to get a build.

## Running

```
myrsm.exe [--console] [--httpport <port>] [--wsport <port>] [--pollms <ms>] [--version]
          [--metric-prefix <p>] [--host-label <name>] [--freshness-timeout <s>]
          [--include-sensors|--exclude-sensors|--include-classes|--exclude-classes|--include-apps|--exclude-apps <csv>]
          [--compat-source-label]
```

- The Prometheus options only affect `/metrics` and `/healthz`/`/readyz` (defaults: prefix
  `hwinfo`, host `%COMPUTERNAME%`, freshness `20` s, no filters, no `source` label). See
  [README.md](README.md) for semantics; they mirror the Python connector's env vars.

- Default ports: HTTP `34567`... but note the *actual* HTTP route is bound via `QHttpServer::bind`
  onto the raw `QTcpServer`, and `test.http` connects on `34569` — check `tcpPortArg`/`wsPortArg`
  defaults in [main.cpp](main.cpp) against whatever you're testing against, they're easy to
  misremember.
- `--pollms` sets the shared-memory poll interval (default `250`, clamped to a `50`ms floor in
  `loadConfigFromCli`). This is the effective freshness bound for both the HTTP endpoint and the
  WebSocket push — a scraper hitting `/` faster than this just re-reads the same cached tick.
- With no console attached and no `--console` flag, the app calls `StartServiceCtrlDispatcher` and
  expects to be launched by the Windows SCM (see `control.qs`, which registers/starts/stops the
  `MyRsm` service via `sc.exe` during install/uninstall).
- `--version` prints `VERSION` / `PROJECT_VERSION_NAME` and exits without starting any server.

## Architecture

- **[main.cpp](main.cpp)** — entry point, CLI parsing, HTTP/WebSocket server wiring, and the Windows
  Service scaffolding (`ServiceMain`, `ServiceCtrlHandler`, SCM status reporting). Both the console
  path (`runAsConsole`) and the service path (`ServiceMain`) independently construct a
  `QCoreApplication` and call the shared `configure()` to stand up the `MyRsm` instance and both
  servers — keep them in sync if you change server setup.
- **[MyRsm.hpp](MyRsm.hpp) / [MyRsm.cpp](MyRsm.cpp)** — the core: attaches to HWiNFO's named shared
  memory segment (`HWiNFO_SENSORS_MAP_FILE_NAME2`) and stays attached (no per-tick attach/detach —
  that was measurable overhead once polling moved to sub-second intervals). Polls on a `QTimer` at
  the interval passed into the constructor (`--pollms`, default 250ms) and parses the two
  variable-length array sections (sensors, then readings) using offsets/strides read from the shared
  memory header itself (`dwOffsetOfSensorSection`, `dwSizeOfSensorElement`, etc.) rather than fixed
  struct layouts. Each poll acquires the `HWiNFO_SENSORS_SM2_MUTEX` named mutex with a short
  (50ms) timeout before touching the mapped memory — HWiNFO documents this mutex as required to
  avoid torn reads while it's writing; if the wait times out, that tick is skipped rather than
  risking corrupt data. The mutex is opened with `OpenMutexA` for each read and closed right after
  (`HwinfoMutexLock`); never create it and never hold a handle between ticks — a lingering handle
  keeps HWiNFO's mutex alive after HWiNFO exits, and a restarted HWiNFO then won't publish shared
  memory until MyRsm is stopped (observed in practice). It also checks `dwSignature` for HWiNFO's
  documented `DEAD` marker, keeps the last-known-good readings for `/`, and detaches from the
  mapping immediately (re-attaching on the next tick) instead of parsing garbage. `poll()` re-reads the sensor list only when
  its count changes, but always rebuilds the reading list every tick. `toJson()` builds the JSON
  response and accepts `classRegex`/`nameRegex` query-param filters plus a `getMinMaxAvg=1` flag to
  include min/max/avg fields — the filter regexes are compiled once per request, not once per
  reading, which matters once requests are arriving every 250ms across a few hundred readings.
  `toJson()` serialises `snapshot()` (a list of `SnapshotRow`, see [RsmTypes.hpp](RsmTypes.hpp)),
  and `/metrics` is rendered from the same `snapshot()`, so the two can't drift apart. `poll()` also
  maintains `PollHealth` (attempt/success timestamps, counters, last error): a tick only counts as
  *fresh* when HWiNFO's `poll_time` changed, and once nothing fresh has arrived for the freshness
  timeout it detaches the shared memory every tick so a restarted HWiNFO is re-attached (see
  README "Freshness"). A mutex timeout counts as a failed poll but changes nothing else.
- **[PromExporter.hpp](PromExporter.hpp) / [PromExporter.cpp](PromExporter.cpp)** — Windows-free
  port of the HWINFOtoPrometheusConnector @ `2fb02f5` output contract: `cleanText` (mojibake
  repair), `sanitizeName`, `normalizeUnit`, `safeFloat` (Python `float()` semantics), `buildRows`
  (filters + `occurrence`), `render` (text format 0.0.4, connector's families/HELP, no sensor
  families when stale), `renderHealthJson`. Python string semantics (`str.strip()` whitespace set,
  `repr(float)`) are reproduced deliberately — label values must match the connector byte-for-byte
  or dashboards break. Don't "improve" them without a shadow comparison.
- **[hwisenssm2.h](hwisenssm2.h)** — vendored copy of HWiNFO's public shared-memory ABI (structs are
  `#pragma pack(1)` — don't reformat/reorder these; they must binary-match HWiNFO's layout). Treat
  as a third-party interface header, not application code.
- **[logger.hpp](logger.hpp) / [logger.cpp](logger.cpp)** — singleton (`logger::init()` /
  `logger::uninit()`) installed as a Qt message handler (`qInstallMessageHandler`). Writes timestamped
  log files to `C:/ProgramData/myrsm/` (or `C:/ProgramData/myrsm_<BRANCH>/` for non-`main` branches),
  rotating to keep only the last 7 files, rolling over daily. Only wired up in service mode
  (`runAsService()` calls `logger::init()`); console mode relies on Qt's default stderr logging via
  the `qSetMessagePattern` set in `main()`. Set env var `STDERR_LOG=1` to also mirror log output to
  stderr when the file logger is active.
- **HTTP/WebSocket surface** — `/` on both transports, plus HTTP-only `/metrics`, `/healthz`
  and `/readyz` (200 when exporter_up, else 503; query params ignored). On `/`, query params act as filters
  and are forwarded straight into `MyRsm::toJson()`; the WebSocket path additionally pushes updates
  to connected clients on every `MyRsm::newData` signal (i.e. every poll tick), replaying the same
  filtered payload each time using the query params captured at connection time.
- **[myrsm.rc.in](myrsm.rc.in) / CMake icon generation** — Windows version-info resource, templated
  by CMake (`@VERSION@`, `@VERSION_JOB@`, `@COMMENT@`, etc.) and paired with an `.ico` generated from
  `logo-rsm.png` via ImageMagick at build time.
- **[control.qs](control.qs)** — Qt Installer Framework control script run during
  install/uninstall of the packaged installer. Registers/deregisters the `MyRsm` Windows service via
  `sc.exe` and copies/removes the per-user uninstall registry key to `HKEY_LOCAL_MACHINE` when
  installed for all users. This only runs in the context of the IFW-built installer, not during
  normal development.

## Licensing

GPLv3 (see [LICENSE](LICENSE)). `myrsm.rc.in` marks the binary itself "Private use only" in its
version resource — be aware of this apparent inconsistency if touching packaging/licensing metadata.
