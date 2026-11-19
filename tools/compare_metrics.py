#!/usr/bin/env python3
"""Compare two Prometheus /metrics expositions for HWiNFO exporter parity.

Two modes:

  File mode (exact, used by CI via run_parity.py):
      python compare_metrics.py EXPECTED.txt ACTUAL.txt

  Live mode (two running exporters, e.g. the Python connector vs MyRSM):
      python compare_metrics.py --live http://CONNECTOR:10445/metrics http://127.0.0.1:34567/metrics --tolerance 0.5

Checks (both modes):
  * every <prefix>_exporter_* family is present on both sides, with the same
    HELP text and type;
  * the *_sensor_* families present are the same, with the same HELP and type;
  * the multiset of (metric name, label set) for *_sensor_* series is
    identical; any series on one side only is printed;
  * the right-hand side has no unexpected <prefix>_* families.

Values:
  * file mode: every value (exporter and sensor) must be exactly equal;
  * live mode: sensor values may differ by up to
        tolerance * max(1, |a|, |b|)
    (relative, with an absolute floor), because the two scrapes are not
    simultaneous. Drift beyond that is reported; it only fails the run with
    --fail-on-drift. Exporter values are not compared in live mode.

--prefix-a / --prefix-b let you compare differently-prefixed outputs, e.g.
the live dashboard (hwinfo) against a shadow run (hwinfo_shadow).
Exit status: 0 = parity, 1 = mismatch, 2 = usage / fetch error.
"""

import argparse
import math
import sys
import urllib.request
from collections import Counter

from prometheus_client.parser import text_string_to_metric_families

EXPORTER_SUFFIXES = (
    "_exporter_up",
    "_exporter_source_up",
    "_exporter_stale",
    "_exporter_data_age_seconds",
    "_exporter_freshness_timeout_seconds",
    "_exporter_last_success_timestamp_seconds",
    "_exporter_last_attempt_timestamp_seconds",
    "_exporter_last_poll_duration_seconds",
    "_exporter_last_http_status",
    "_exporter_successful_polls_total",
    "_exporter_failed_polls_total",
    "_exporter_raw_items_last",
    "_exporter_exported_items_last",
)
SENSOR_SUFFIXES = (
    "_sensor_value",
    "_sensor_update_time_seconds",
    "_sensor_present",
)
MAX_LISTED = 25


def parse(text: str) -> dict:
    families = {}
    for fam in text_string_to_metric_families(text):
        if fam.name in families:
            raise ValueError(f"family {fam.name} appears twice")
        families[fam.name] = fam
    return families


def _label_key(labels: dict, extra: dict = None) -> tuple:
    merged = dict(labels)
    if extra:
        merged.update(extra)
    return tuple(sorted(merged.items()))


def _values_equal(a: float, b: float) -> bool:
    return a == b or (math.isnan(a) and math.isnan(b))


def _fmt_labels(key: tuple) -> str:
    return "{" + ",".join(f'{k}="{v}"' for k, v in key) + "}"


def compare(expected_text: str, actual_text: str, *, prefix_a: str = "hwinfo", prefix_b: str = "hwinfo",
            live: bool = False, tolerance: float = 0.0, extra_labels_a: dict = None):
    """Return (errors, warnings) lists of human-readable strings.

    extra_labels_a: labels added to every left-hand sensor series before
    comparing (used to emulate the post-MQTT connector's source="http").
    """
    errors, warnings = [], []
    a = parse(expected_text)
    b = parse(actual_text)

    # Exporter families.
    for suffix in EXPORTER_SUFFIXES:
        na, nb = prefix_a + suffix, prefix_b + suffix
        fa, fb = a.get(na), b.get(nb)
        if fa is None:
            errors.append(f"left side is missing {na}")
        if fb is None:
            errors.append(f"right side is missing {nb}")
        if fa is None or fb is None:
            continue
        if fa.documentation != fb.documentation:
            errors.append(f"{nb}: HELP differs: {fa.documentation!r} vs {fb.documentation!r}")
        if fa.type != fb.type:
            errors.append(f"{nb}: TYPE differs: {fa.type} vs {fb.type}")
        if not live:
            va = [(s.name[len(prefix_a):], _label_key(s.labels), s.value) for s in fa.samples]
            vb = [(s.name[len(prefix_b):], _label_key(s.labels), s.value) for s in fb.samples]
            if len(va) != len(vb) or any(x[:2] != y[:2] or not _values_equal(x[2], y[2]) for x, y in zip(va, vb)):
                errors.append(f"{nb}: value differs: {va} vs {vb}")

    # Sensor families: presence.
    present_a = {s for s in SENSOR_SUFFIXES if prefix_a + s in a}
    present_b = {s for s in SENSOR_SUFFIXES if prefix_b + s in b}
    for s in sorted(present_a - present_b):
        errors.append(f"right side has no {prefix_b + s} family (left side has it)")
    for s in sorted(present_b - present_a):
        errors.append(f"left side has no {prefix_a + s} family (right side has it)")

    # Unexpected families on the right-hand side.
    known_b = {prefix_b + s for s in EXPORTER_SUFFIXES + SENSOR_SUFFIXES}
    for name in sorted(b):
        if name.startswith(prefix_b + "_") and name not in known_b:
            errors.append(f"right side has unexpected family {name}")

    # Sensor series: labels and values.
    for suffix in SENSOR_SUFFIXES:
        if suffix not in present_a or suffix not in present_b:
            continue
        fa, fb = a[prefix_a + suffix], b[prefix_b + suffix]
        if fa.documentation != fb.documentation:
            errors.append(f"{prefix_b + suffix}: HELP differs: {fa.documentation!r} vs {fb.documentation!r}")
        if fa.type != fb.type:
            errors.append(f"{prefix_b + suffix}: TYPE differs: {fa.type} vs {fb.type}")

        sa = [(_label_key(s.labels, extra_labels_a), s.value) for s in fa.samples]
        sb = [(_label_key(s.labels), s.value) for s in fb.samples]
        ca = Counter(k for k, _ in sa)
        cb = Counter(k for k, _ in sb)
        only_a = list((ca - cb).elements())
        only_b = list((cb - ca).elements())
        for k in only_a[:MAX_LISTED]:
            errors.append(f"only on left:  {prefix_a + suffix}{_fmt_labels(k)}")
        for k in only_b[:MAX_LISTED]:
            errors.append(f"only on right: {prefix_b + suffix}{_fmt_labels(k)}")
        if len(only_a) > MAX_LISTED or len(only_b) > MAX_LISTED:
            errors.append(f"{suffix}: {len(only_a)} series only on left, {len(only_b)} only on right (list truncated)")
        for k, n in ca.items():
            if n > 1:
                errors.append(f"left side has {n} series with identical labels {prefix_a + suffix}{_fmt_labels(k)}")
        for k, n in cb.items():
            if n > 1:
                errors.append(f"right side has {n} series with identical labels {prefix_b + suffix}{_fmt_labels(k)}")

        va = dict(sa)
        vb = dict(sb)
        drift = 0
        for k in sorted(set(va) & set(vb)):
            x, y = va[k], vb[k]
            if live:
                if not _values_equal(x, y) and abs(x - y) > tolerance * max(1.0, abs(x), abs(y)):
                    drift += 1
                    if drift <= MAX_LISTED:
                        warnings.append(f"value drift {prefix_b + suffix}{_fmt_labels(k)}: {x} vs {y}")
            elif not _values_equal(x, y):
                errors.append(f"value differs {prefix_b + suffix}{_fmt_labels(k)}: {x!r} vs {y!r}")
        if drift > MAX_LISTED:
            warnings.append(f"{suffix}: {drift} series drifted beyond tolerance (list truncated)")

    return errors, warnings


def fetch(url: str) -> str:
    with urllib.request.urlopen(url, timeout=10) as resp:
        return resp.read().decode("utf-8")


def main() -> int:
    # Label values can hold any Unicode; never crash on a legacy console code page.
    sys.stdout.reconfigure(errors="backslashreplace")
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("left", help="expected file, or left URL with --live")
    ap.add_argument("right", help="actual file, or right URL with --live")
    ap.add_argument("--live", action="store_true", help="LEFT and RIGHT are /metrics URLs to scrape")
    ap.add_argument("--tolerance", type=float, default=0.0,
                    help="live mode: allowed relative drift, |a-b| <= tol*max(1,|a|,|b|)")
    ap.add_argument("--fail-on-drift", action="store_true", help="live mode: value drift fails the run")
    ap.add_argument("--prefix-a", default="hwinfo", help="metric prefix on the left side")
    ap.add_argument("--prefix-b", default="hwinfo", help="metric prefix on the right side")
    args = ap.parse_args()

    try:
        if args.live:
            left, right = fetch(args.left), fetch(args.right)
        else:
            with open(args.left, encoding="utf-8", newline="") as f:
                left = f.read()
            with open(args.right, encoding="utf-8", newline="") as f:
                right = f.read()
    except Exception as exc:  # noqa: BLE001 - report and exit
        print(f"error: {exc}", file=sys.stderr)
        return 2

    errors, warnings = compare(left, right, prefix_a=args.prefix_a, prefix_b=args.prefix_b,
                               live=args.live, tolerance=args.tolerance)
    for w in warnings:
        print("WARN ", w)
    for e in errors:
        print("FAIL ", e)

    sensor_a = sum(1 for f in text_string_to_metric_families(left) if f.name == args.prefix_a + "_sensor_value"
                   for _ in f.samples)
    sensor_b = sum(1 for f in text_string_to_metric_families(right) if f.name == args.prefix_b + "_sensor_value"
                   for _ in f.samples)
    print(f"{args.prefix_a}_sensor_value series: left={sensor_a} right={sensor_b}")

    if errors or (args.fail_on_drift and warnings):
        print("RESULT: MISMATCH")
        return 1
    print("RESULT: PARITY" + (" (with value drift)" if warnings else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
