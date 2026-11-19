# Installer test (GATE 3)

CI already runs a silent install → ini edit + restart → upgrade → uninstall cycle and a service
lifecycle test on a GitHub runner (see the `Service lifecycle test` and `Installer test (silent)`
steps). This checklist covers what CI can't: a **clean** machine (Windows Sandbox), the interactive
wizard, HWiNFO itself, and the upgrade of your real PC.

Get `MyRsm-Setup-<version>.exe` from the `MyRsm-Setup-<run>` artifact of the CI run for the branch.

All commands are for an **elevated** PowerShell.

## 0. Windows Sandbox (once)

```powershell
Enable-WindowsOptionalFeature -FeatureName Containers-DisposableClientVM -Online   # reboot if asked
```

Start **Windows Sandbox**, then copy `MyRsm-Setup-<version>.exe` into it (paste it onto the
Sandbox desktop). Each Sandbox start is a clean Windows: no VC++ runtime, no HWiNFO.

## 1. Fresh install (Sandbox)

1. Run the setup and click through: keep the defaults, tick the firewall rule.
   Expected: no errors. The finish page says MyRsm is running but sees no HWiNFO data. That's
   correct, because the Sandbox has no HWiNFO.
2. Check:

   ```powershell
   & "$env:ProgramFiles\MyRsm\bin\myrsm.exe" service status
   # State: RUNNING, Start type: Automatic (Delayed Start), Account: LocalSystem,
   # Binary: "C:\Program Files\MyRsm\bin\myrsm.exe"   (quoted, no arguments)
   Get-Service MyRsm | Format-List Status, StartType
   sc.exe qfailure MyRsm            # RESET_PERIOD 86400, RESTART -- Delay = 5000 milliseconds x3
   curl.exe -s -i http://127.0.0.1:34567/healthz    # HTTP 503 and JSON (no HWiNFO in the Sandbox)
   Get-NetFirewallRule -DisplayName "MyRsm HTTP" | Get-NetFirewallPortFilter    # LocalPort 34567
   Get-NetFirewallRule -DisplayName "MyRsm HTTP" | Get-NetFirewallAddressFilter # RemoteAddress LocalSubnet
   Get-Content "$env:ProgramData\MyRsm\myrsm.ini"
   Get-ItemProperty "HKLM:\SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64" | Format-List Installed, Version
   ```

   - [ ] Service running, Automatic (Delayed Start), LocalSystem, quoted binPath
   - [ ] `/healthz` answers
   - [ ] Firewall rule present with the right port and scope
   - [ ] VC++ runtime `Installed : 1`. The setup installed it, because the Sandbox doesn't have it.

## 2. Edit the ini, restart (Sandbox)

```powershell
notepad "$env:ProgramData\MyRsm\myrsm.ini"     # set http_port=34600, save
Restart-Service MyRsm
curl.exe -s -o NUL -w "%{http_code}\n" http://127.0.0.1:34600/healthz   # 503 = answering on the new port
```

- [ ] New port answers

## 3. Upgrade / reinstall (Sandbox)

Run the same setup again. The settings page should show **34600**, prefilled from the ini.
Click through.

- [ ] No "file in use" / locked-file errors
- [ ] `Get-Content "$env:ProgramData\MyRsm\myrsm.ini"` still has your values
- [ ] Service running, `/healthz` answers on 34600

## 4. Crash recovery (Sandbox)

```powershell
$before = (Get-CimInstance Win32_Service -Filter "Name='MyRsm'").ProcessId
Stop-Process -Name myrsm -Force
Start-Sleep 8
Get-Service MyRsm; (Get-CimInstance Win32_Service -Filter "Name='MyRsm'").ProcessId   # Running, new PID
```

- [ ] SCM restarted it within about 5 s

## 5. Uninstall (Sandbox)

Settings → Apps → MyRsm → Uninstall. Answer **No** to "Also delete the MyRsm settings and logs".

```powershell
Get-Service MyRsm -ErrorAction SilentlyContinue                 # nothing
Get-NetFirewallRule -DisplayName "MyRsm HTTP" -ErrorAction SilentlyContinue   # nothing
Test-Path "$env:ProgramFiles\MyRsm"                             # False
Test-Path "$env:ProgramData\MyRsm\myrsm.ini"                    # True (you said No)
```

- [ ] Service, firewall rule and Program Files folder gone; ProgramData kept
- [ ] Reinstall + uninstall answering **Yes** → `$env:ProgramData\MyRsm` gone

## 6. Silent install with parameters (a second, fresh Sandbox)

```powershell
.\MyRsm-Setup-<version>.exe /VERYSILENT /SUPPRESSMSGBOXES /HTTPPORT=34567 /HOSTLABEL=SandboxPC /FWSCOPE=192.168.1.50 /LOG=$env:TEMP\myrsm-setup.log
Get-Content "$env:ProgramData\MyRsm\myrsm.ini"          # host_label=SandboxPC
Get-NetFirewallRule -DisplayName "MyRsm HTTP" | Get-NetFirewallAddressFilter   # 192.168.1.50
Select-String "$env:TEMP\myrsm-setup.log" -Pattern "Health check|PROBLEM"
```

- [ ] Installed silently with those values, no `PROBLEM` lines in the log

Silent uninstall, which keeps ProgramData unless you pass `/PURGEDATA`:

```powershell
& "$env:ProgramFiles\MyRsm\unins000.exe" /VERYSILENT /SUPPRESSMSGBOXES
```

## 7. Upgrade your real PC

Your current `MyRsm` service runs `C:\PROGRAMS\myrsm-win64-3\bin\myrsm.exe --host-label DJMalachite`
(a manual copy, not the IFW install). The setup will:

- prefill **Host label = DJMalachite** from that binPath (and any `--httpport`/`--metric-prefix`
  in it);
- stop that service and repoint it to `C:\Program Files\MyRsm\bin\myrsm.exe` with no arguments
  (the settings move into `myrsm.ini`);
- show a note that the service used to run from the old folder.

Steps:

1. Run the setup. Check the settings page: host label `DJMalachite`, port 34567, prefix `hwinfo`,
   compat `source` label unticked. For the firewall scope, use the Prometheus host's IP, or keep
   `LocalSubnet` if that covers it. Finish.
2. Expected finish page: "MyRsm is running and exporting HWiNFO data" (HWiNFO running with
   Shared Memory Support on).
3. Check:

   ```powershell
   & "$env:ProgramFiles\MyRsm\bin\myrsm.exe" service status
   curl.exe -s http://127.0.0.1:34567/metrics | Select-String "hwinfo_exporter_up|host=" | Select-Object -First 2
   ```

   - [ ] `hwinfo_exporter_up 1.0`, `host="DJMalachite"`
4. If there is an old per-user IFW "MyRsm" entry in Settings → Apps, uninstall it. Then re-run
   `service status`. If the service is gone (the old uninstaller deletes it), run the new setup
   again to repair.
5. Once everything is green, delete `C:\PROGRAMS\myrsm-win64-3`.
6. Grafana: repeat the Phase 2 check. Every panel updates, and `hwinfo_exporter_up` stays 1
   apart from a gap of up to one scrape interval around the upgrade.

   - [ ] Dashboards green after the upgrade

## Troubleshooting

- Setup log: `%TEMP%\Setup Log <date> #NNN.txt`, or wherever you passed `/LOG=`. Lines starting with
  `myrsm.exe service ...` include the command's output; `PROBLEM:` lines are what the setup
  reported as failed.
- Service log: `%ProgramData%\MyRsm\logs\` (test builds: `logs-<branch>\`).
- `myrsm.exe service status` shows the last exit code when the service is stopped. Service-specific
  exit codes: `1` HTTP port in use or not bindable, `2` HTTP server bind failed, `3` WebSocket port in
  use.
