#!/usr/bin/env python3
"""Golden parity test: MyRSM's prom::render vs the reference Python connector.

For every tests/fixtures/<name>.json (MyRSM "/" JSON payloads) and every run
listed in the optional tests/fixtures/<name>.options.json, render:

  * expected: the verbatim connector @ 2fb02f5 (tools/reference/render_reference.py)
  * actual:   myrsm_render_fixture (same buildRows/render code as /metrics)

with the same frozen poll state, then require exact parity with
compare_metrics.compare(). Any difference fails the run.

Usage:
    python tools/run_parity.py --renderer build/Release/myrsm_render_fixture.exe [--keep out_dir]
"""

import argparse
import json
import pathlib
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE / "reference"))

import compare_metrics  # noqa: E402
import render_reference  # noqa: E402

RUN_KEYS = {
    "metric_prefix": "--metric-prefix",
    "host_label": "--host-label",
    "freshness_timeout": "--freshness-timeout",
    "data_age": "--data-age",
    "include_sensors": "--include-sensors",
    "exclude_sensors": "--exclude-sensors",
    "include_classes": "--include-classes",
    "exclude_classes": "--exclude-classes",
    "include_apps": "--include-apps",
    "exclude_apps": "--exclude-apps",
}
KNOWN_KEYS = set(RUN_KEYS) | {"name", "compat_source_label", "expect_sensor_series"}


def renderer_args(run: dict) -> list:
    args = []
    for key, flag in RUN_KEYS.items():
        if key in run:
            args += [flag, str(run[key])]
    if run.get("compat_source_label"):
        args.append("--compat-source-label")
    return args


def main() -> int:
    # Label values can hold any Unicode; never crash on a legacy console code page.
    sys.stdout.reconfigure(errors="backslashreplace")
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--renderer", required=True, help="path to myrsm_render_fixture(.exe)")
    ap.add_argument("--fixtures", default=str(HERE.parent / "tests" / "fixtures"))
    ap.add_argument("--keep", help="write expected/actual outputs to this directory")
    args = ap.parse_args()

    fixtures = sorted(p for p in pathlib.Path(args.fixtures).glob("*.json") if not p.name.endswith(".options.json"))
    if not fixtures:
        print(f"no fixtures found in {args.fixtures}", file=sys.stderr)
        return 2

    out_dir = pathlib.Path(args.keep) if args.keep else pathlib.Path(tempfile.mkdtemp(prefix="myrsm-parity-"))
    out_dir.mkdir(parents=True, exist_ok=True)

    failures = 0
    total = 0
    for fixture in fixtures:
        options_file = fixture.with_name(fixture.stem + ".options.json")
        runs = json.loads(options_file.read_text(encoding="utf-8")) if options_file.exists() else [{"name": "default"}]
        data = fixture.read_bytes()
        for run in runs:
            total += 1
            unknown = set(run) - KNOWN_KEYS
            label = f"{fixture.stem}[{run.get('name', 'default')}]"
            if unknown:
                print(f"FAIL {label}: unknown run keys {sorted(unknown)}")
                failures += 1
                continue

            expected = render_reference.render(data, run)
            expected_path = out_dir / f"{fixture.stem}.{run.get('name', 'default')}.expected_metrics.txt"
            actual_path = out_dir / f"{fixture.stem}.{run.get('name', 'default')}.actual_metrics.txt"
            expected_path.write_bytes(expected)

            cmd = [args.renderer, str(fixture), "--out", str(actual_path)] + renderer_args(run)
            proc = subprocess.run(cmd, capture_output=True, text=True)
            if proc.returncode != 0:
                print(f"FAIL {label}: renderer exited {proc.returncode}: {proc.stderr.strip()}")
                failures += 1
                continue
            actual = actual_path.read_bytes()

            prefix = render_reference.load_reference().sanitize_name(run.get("metric_prefix", "hwinfo"))
            extra = {"source": "http"} if run.get("compat_source_label") else None
            errors, _ = compare_metrics.compare(expected.decode("utf-8"), actual.decode("utf-8"),
                                                prefix_a=prefix, prefix_b=prefix, extra_labels_a=extra)

            # Guard against a fixture silently exporting nothing.
            series = sum(1 for f in compare_metrics.text_string_to_metric_families(actual.decode("utf-8"))
                         if f.name == prefix + "_sensor_value" for _ in f.samples)
            want = run.get("expect_sensor_series")
            if want is not None and series != want:
                errors.append(f"expected {want} {prefix}_sensor_value series, got {series}")

            if errors:
                failures += 1
                print(f"FAIL {label} ({series} sensor series)")
                for e in errors:
                    print(f"     {e}")
            else:
                print(f"ok   {label} ({series} sensor series)")

    print(f"{total - failures}/{total} parity runs passed; outputs in {out_dir}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
