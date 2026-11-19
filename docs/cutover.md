# Cutover: Python connector → MyRsm native `/metrics`

This replaces `HWiNFO → MyRsm → HWINFOtoPrometheusConnector → Prometheus` with
`HWiNFO → MyRsm (/metrics) → Prometheus`, one machine at a time, with no dashboard edits.
Placeholders in `<ANGLE_BRACKETS>` are looked up per machine at deploy time. None of them are
stored in the repo.

Run through **GATE 1** (manual check of the new build against the live connector) before you start.

## 1. Pre-cutover checklist (per machine)

Read these from the live system and write them down.

| # | What | How | Becomes |
|---|---|---|---|
| 1 | Connector `EXPORTER_HOST` | `compose.yaml` / `docker inspect hwinfo-exporter`, or the `host` label: `curl -s http://<CONNECTOR_HOST>:10445/metrics \| grep -m1 hwinfo_sensor_value` | `--host-label` |
| 2 | Does the live output carry `source=`? | same `grep` output: look for `source="http"` | present → `--compat-source-label` |
| 3 | `job` / `instance` labels Prometheus attaches | `curl -s '<PROMETHEUS_URL>/api/v1/series?match[]=hwinfo_exporter_up'` | `job_name`, pinned `instance` (step 3.2) |
| 4 | Current scrape interval of the hwinfo job | `prometheus.yml` | shadow job `scrape_interval` |
| 5 | Do dashboards filter on `job`/`instance` or use `python_*`/`process_*`? | Grafana → dashboard → Share → Export → JSON, then `grep -nE 'job=|instance=|python_|process_' dashboard.json` | `python_*`/`process_*` panels need replacing (MyRsm doesn't emit them) |
| 6 | Connector filters (`INCLUDE_*`/`EXCLUDE_*`, `METRIC_PREFIX`, `DATA_FRESHNESS_TIMEOUT`) | `compose.yaml` / `docker inspect` | matching `--include-*`/`--exclude-*`, `--metric-prefix`, `--freshness-timeout` |

## 2. Network prerequisites

Until now Prometheus only talked to the connector container on its own host. It will now scrape
the Windows PC directly on TCP 34567.

- **Windows Firewall** on the PC: inbound rule for TCP 34567, scoped to the Prometheus host
  (elevated PowerShell):

  ```powershell
  New-NetFirewallRule -DisplayName "MyRsm HTTP (Prometheus)" -Direction Inbound -Action Allow `
    -Protocol TCP -LocalPort 34567 -RemoteAddress <PROMETHEUS_HOST_IP>
  ```

- **UniFi**: a zone/firewall rule allowing the Prometheus host's network to reach the PC's network
  on TCP 34567.
- **Docker**: if Prometheus runs in a container, check that it can route to the PC's LAN IP. This is
  the bridge-isolation problem that has bitten before:

  ```bash
  docker exec <PROMETHEUS_CONTAINER> wget -qO- http://<WINDOWS_HOST_IP>:34567/healthz
  ```

  A JSON reply (HTTP 200 or 503) means the network path works. A timeout means it doesn't.

## 3. Shadow run (24 h or more)

Run the new MyRsm with a different prefix so its series can't collide with the live dashboard.
The connector keeps working throughout, because the `/` JSON route is unchanged.

1. Put the new build in place of the existing install and add the shadow arguments to the
   service. Use an elevated PowerShell, and add `--compat-source-label` if checklist item 2 said so:

   ```powershell
   Stop-Service MyRsm
   (Get-CimInstance Win32_Service -Filter "Name='MyRsm'").PathName   # shows the current myrsm.exe
   # Copy the CI artifact's contents (bin\, plugins, ...) over the install root, i.e. the folder above bin\, then:
   sc.exe config MyRsm binPath= "\"<PATH_TO>\myrsm.exe\" --metric-prefix hwinfo_shadow --host-label <EXPORTER_HOST>"
   Start-Service MyRsm
   curl.exe -s http://127.0.0.1:34567/healthz
   ```
2. Add a temporary scrape job to `prometheus.yml` and reload Prometheus:

   ```yaml
   - job_name: hwinfo_myrsm_shadow
     scrape_interval: 5s           # match the current hwinfo job's interval (checklist 4)
     static_configs:
       - targets: ['<WINDOWS_HOST_IP>:34567']
   ```

3. Parity checks in Grafana Explore:

   ```promql
   # series count per class must match
   count by (sensor_class) (hwinfo_sensor_value)
   count by (sensor_class) (hwinfo_shadow_sensor_value)

   # anything present on one side only (should return nothing)
   hwinfo_sensor_value unless on(host,sensor_app,sensor_class,sensor_name,sensor_unit,sensor_unit_raw,occurrence) hwinfo_shadow_sensor_value
   hwinfo_shadow_sensor_value unless on(host,sensor_app,sensor_class,sensor_name,sensor_unit,sensor_unit_raw,occurrence) hwinfo_sensor_value

   # value drift (should be near 0 for slow-moving sensors)
   max by (sensor_class, sensor_name) (abs(
     avg_over_time(hwinfo_shadow_sensor_value[1m])
     - on(host,sensor_app,sensor_class,sensor_name,sensor_unit,sensor_unit_raw,occurrence)
     avg_over_time(hwinfo_sensor_value[1m])))
   ```

   The same check works from a shell:

   ```
   python tools/compare_metrics.py --live http://<CONNECTOR_HOST>:10445/metrics http://<WINDOWS_HOST_IP>:34567/metrics --prefix-b hwinfo_shadow --tolerance 0.5
   ```

   Expected differences during the shadow:
   - `hwinfo_exporter_data_age_seconds` is larger on MyRsm (age of the last *new* HWiNFO sample,
     up to HWiNFO's polling period) than on the connector (age of its last HTTP poll).
   - If HWiNFO stops updating, MyRsm drops its sensor series after 20 s. The connector keeps
     exporting the frozen values. This is the intended fix.

## 4. Cutover (per machine)

1. Switch MyRsm to `--metric-prefix hwinfo` and the `--host-label` from the checklist (keep
   `--compat-source-label` if it applied). Restart the service.
2. Repoint the existing hwinfo job's target to `<WINDOWS_HOST_IP>:34567`. **Keep the same
   `job_name`** and pin `instance` to its old value so every dashboard filter still matches:

   ```yaml
   - job_name: <EXISTING_HWINFO_JOB_NAME>      # unchanged
     scrape_interval: <UNCHANGED>
     static_configs:
       - targets: ['<WINDOWS_HOST_IP>:34567']
     relabel_configs:
       - target_label: instance
         replacement: '<OLD_INSTANCE_VALUE>'   # from checklist 3
   ```

   Reload Prometheus (`curl -X POST <PROMETHEUS_URL>/-/reload` if `--web.enable-lifecycle` is on,
   otherwise restart it).
3. Remove the `hwinfo_myrsm_shadow` job, reload, then stop the connector:
   `docker compose down` in the connector's directory.
4. Check every panel on the affected dashboards. A gap of up to one scrape interval at the swap
   point is expected.
5. Repeat for each other monitored PC with its own `host` and `instance` values.

## 5. Rollback

Restore the old target in the hwinfo job, reload Prometheus, and `docker compose up -d` the
connector. MyRsm's `/` JSON never changed, so the connector works against the new build as well.
Nothing in the TSDB needs changing.

## GATE 2

Dashboards green on every migrated machine before starting Phase 3 (installer).
