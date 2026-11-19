#!/usr/bin/env python3
"""Render the *reference* connector's /metrics output for a MyRSM fixture.

Loads the verbatim connector source (hwinfo_prom_exporter_2fb02f5.py, from
DJMalachite/HWINFOtoPrometheusConnector @ 2fb02f5), feeds a MyRSM "/" JSON
fixture through its own parse_hwinfo_payload() and HwinfoCollector with a
frozen poll state, and returns generate_latest() bytes.

The frozen state MUST stay in sync with tests/render_fixture.cpp.

Usage:
    python render_reference.py <fixture.json> --out expected_metrics.txt [--run '{"metric_prefix": "x"}']
"""

import argparse
import importlib.util
import json
import pathlib
import sys
import types

from prometheus_client import CollectorRegistry, generate_latest

HERE = pathlib.Path(__file__).resolve().parent
REFERENCE = HERE / "hwinfo_prom_exporter_2fb02f5.py"

NOW = 1700000000.0     # frozen time.time()
POLL_DURATION = 0.005  # frozen last poll duration

FILTER_KEYS = (
    "include_sensors",
    "exclude_sensors",
    "include_classes",
    "exclude_classes",
    "include_apps",
    "exclude_apps",
)


def load_reference():
    """Import a fresh copy of the reference module (fresh global state)."""
    spec = importlib.util.spec_from_file_location("hwinfo_prom_exporter_2fb02f5", REFERENCE)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def parse_list(value):
    # Same parsing the connector applies to its INCLUDE_*/EXCLUDE_* env vars.
    return [x.strip().lower() for x in (value or "").split(",") if x.strip()]


def render(fixture_bytes: bytes, run: dict) -> bytes:
    mod = load_reference()

    # Configuration (the connector reads these from env vars at import time).
    mod.METRIC_PREFIX = run.get("metric_prefix", "hwinfo")
    mod.EXPORTER_HOST = run.get("host_label", "fixture-host")
    mod.DATA_FRESHNESS_TIMEOUT = float(run.get("freshness_timeout", 20))
    for key in FILTER_KEYS:
        setattr(mod, key.upper(), parse_list(run.get(key)))

    # Frozen clock for HwinfoCollector.collect().
    mod.time = types.SimpleNamespace(time=lambda: NOW)

    # The connector decodes MyRSM's response as UTF-8 and json.loads() it.
    payload = json.loads(fixture_bytes.decode("utf-8"))
    rows, raw_count = mod.parse_hwinfo_payload(payload)

    # Frozen poll state, following poll_once(): zero parsed rows is a failure.
    st = mod.state
    st.last_attempt_ts = NOW - 1.0
    st.last_poll_duration_seconds = POLL_DURATION
    st.last_http_status = 200
    st.raw_items_last = raw_count
    if rows:
        st.rows = rows
        st.source_up = 1
        st.last_success_ts = NOW - float(run.get("data_age", 1))
        st.successful_polls = 1
        st.failed_polls = 0
        st.exported_items_last = len(rows)
    else:
        st.rows = []
        st.source_up = 0
        st.last_success_ts = 0.0
        st.successful_polls = 0
        st.failed_polls = 1
        st.exported_items_last = 0

    registry = CollectorRegistry(auto_describe=False)
    registry.register(mod.HwinfoCollector())
    return generate_latest(registry)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("fixture")
    ap.add_argument("--out", required=True)
    ap.add_argument("--run", default="{}", help="JSON object of run options (see tests/fixtures/README.md)")
    args = ap.parse_args()
    data = pathlib.Path(args.fixture).read_bytes()
    pathlib.Path(args.out).write_bytes(render(data, json.loads(args.run)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
