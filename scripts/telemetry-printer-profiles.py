#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""
HelixScreen Printer Profile Analyzer

Analyzes hardware_profile telemetry events to:
  1. Show detection rates and model distribution
  2. Find discriminating hardware names per detected model
  3. Cluster undetected printers by hardware similarity
  4. Generate candidate heuristics for printer_database.json
  5. Validate existing database entries against telemetry

Usage:
    telemetry-printer-profiles.py                    # Terminal summary
    telemetry-printer-profiles.py --json             # JSON output
    telemetry-printer-profiles.py --candidates       # Print candidate heuristics
    telemetry-printer-profiles.py --validate         # Validate existing DB entries
    telemetry-printer-profiles.py --data-dir /path   # Custom data directory
    telemetry-printer-profiles.py --since 2026-01-01
"""

import argparse
import json
import math
import sys
from collections import Counter, defaultdict
from datetime import datetime
from pathlib import Path
from typing import Any, Optional


def load_hardware_profiles(
    data_dir: str,
    since: Optional[str] = None,
    until: Optional[str] = None,
) -> list[dict]:
    """Load hardware_profile events from telemetry data directory."""
    data_path = Path(data_dir)
    if not data_path.exists():
        print(f"Data directory not found: {data_path}", file=sys.stderr)
        return []

    since_dt = datetime.fromisoformat(since) if since else None
    until_dt = datetime.fromisoformat(until) if until else None

    profiles: list[dict] = []
    json_files = sorted(data_path.rglob("*.json"))
    if not json_files:
        print(f"No JSON files found in {data_path}", file=sys.stderr)
        return []

    for fpath in json_files:
        try:
            with open(fpath, "r") as f:
                data = json.load(f)
            events = data if isinstance(data, list) else [data]
            for ev in events:
                if ev.get("event") != "hardware_profile":
                    continue
                ts = ev.get("timestamp", "")
                if since_dt and ts < since:
                    continue
                if until_dt and ts > until:
                    continue
                profiles.append(ev)
        except (json.JSONDecodeError, OSError) as e:
            print(f"Warning: skipping {fpath}: {e}", file=sys.stderr)

    print(
        f"Loaded {len(profiles)} hardware_profile events from {len(json_files)} files",
        file=sys.stderr,
    )
    return profiles


def extract_names(profile: dict, section: str, field: str = "names") -> list[str]:
    """Extract a names list from a profile section."""
    sub = profile.get(section, {})
    if isinstance(sub, dict):
        val = sub.get(field, [])
        if isinstance(val, list):
            return [str(v) for v in val]
    return []


def extract_printer_objects(profile: dict) -> list[str]:
    """Extract printer_objects list from a profile."""
    objs = profile.get("printer_objects", [])
    if isinstance(objs, list):
        return [str(o) for o in objs]
    return []


def get_detected_model(profile: dict) -> str:
    """Get detected model name, or '' if undetected."""
    printer = profile.get("printer", {})
    model = printer.get("detected_model", "")
    return model if isinstance(model, str) else ""


def get_kinematics(profile: dict) -> str:
    printer = profile.get("printer", {})
    kin = printer.get("kinematics", "")
    return kin if isinstance(kin, str) else ""


def get_build_volume(profile: dict) -> tuple[int, int, int]:
    bv = profile.get("build_volume", {})
    if isinstance(bv, dict):
        return (
            int(bv.get("x_mm", 0)),
            int(bv.get("y_mm", 0)),
            int(bv.get("z_mm", 0)),
        )
    return (0, 0, 0)


def get_mcu(profile: dict) -> str:
    mcus = profile.get("mcus", {})
    return str(mcus.get("primary", "")) if isinstance(mcus, dict) else ""


# ---------------------------------------------------------------------------
# Grouping & analysis
# ---------------------------------------------------------------------------


def group_by_model(
    profiles: list[dict],
) -> dict[str, list[dict]]:
    """Group profiles by detected_model. Empty string key = undetected."""
    groups: dict[str, list[dict]] = defaultdict(list)
    for p in profiles:
        groups[get_detected_model(p)].append(p)
    return dict(groups)


NameSource = tuple[str, str]  # (section, field)

ALL_NAME_SOURCES: list[NameSource] = [
    ("fans", "names"),
    ("sensors", "temperature_names"),
    ("sensors", "filament_names"),
    ("leds", "names"),
    ("steppers", "names"),
    ("macros", "names"),
]


def compute_name_frequencies(
    profiles: list[dict],
) -> dict[NameSource, Counter]:
    """For each name source, count how many profiles contain each name."""
    freqs: dict[NameSource, Counter] = {}
    for source in ALL_NAME_SOURCES:
        counter: Counter = Counter()
        for p in profiles:
            names = extract_names(p, source[0], source[1])
            for name in set(names):  # deduplicate within profile
                counter[name] += 1
        freqs[source] = counter
    # Also printer_objects
    obj_counter: Counter = Counter()
    for p in profiles:
        for obj in set(extract_printer_objects(p)):
            obj_counter[obj] += 1
    freqs[("printer_objects", "")] = obj_counter
    return freqs


def compute_object_frequencies(
    profiles: list[dict],
) -> Counter:
    """Count printer_objects across all profiles."""
    counter: Counter = Counter()
    for p in profiles:
        for obj in set(extract_printer_objects(p)):
            counter[obj] += 1
    return counter


def find_common_names(
    freqs: dict[NameSource, Counter],
    total: int,
    threshold: float = 0.5,
) -> dict[str, list[tuple[str, float]]]:
    """Names appearing in >threshold fraction of profiles, by source."""
    result: dict[str, list[tuple[str, float]]] = {}
    for source, counter in freqs.items():
        key = f"{source[0]}.{source[1]}" if source[1] else source[0]
        common = []
        for name, count in counter.most_common():
            pct = count / total
            if pct >= threshold:
                common.append((name, round(pct * 100, 1)))
            else:
                break
        if common:
            result[key] = common
    return result


def find_discriminating_names(
    group_freqs: dict[str, dict[NameSource, Counter]],
    group_sizes: dict[str, int],
    min_prevalence: float = 0.4,
    max_other_prevalence: float = 0.1,
) -> dict[str, list[dict]]:
    """
    Find names that are common in one group but rare in others.

    Returns {model: [{name, source, prevalence, max_other_prevalence, confidence_suggestion}]}
    """
    all_models = [m for m in group_sizes if m]  # skip undetected
    result: dict[str, list[dict]] = {}

    for model in all_models:
        discriminators = []
        model_freqs = group_freqs[model]
        model_size = group_sizes[model]
        if model_size < 3:
            continue

        for source, counter in model_freqs.items():
            key = f"{source[0]}.{source[1]}" if source[1] else source[0]
            for name, count in counter.items():
                prevalence = count / model_size
                if prevalence < min_prevalence:
                    continue

                # Check prevalence in other groups
                max_other = 0.0
                for other_model in all_models:
                    if other_model == model:
                        continue
                    other_counter = group_freqs[other_model].get(source, Counter())
                    other_size = group_sizes[other_model]
                    if other_size > 0:
                        other_pct = other_counter.get(name, 0) / other_size
                        max_other = max(max_other, other_pct)

                if max_other <= max_other_prevalence:
                    # Suggest confidence based on prevalence and exclusivity
                    conf = int(min(99, prevalence * 100 * (1 - max_other)))
                    discriminators.append(
                        {
                            "name": name,
                            "source": key,
                            "prevalence_pct": round(prevalence * 100, 1),
                            "max_other_pct": round(max_other * 100, 1),
                            "confidence_suggestion": conf,
                        }
                    )

        if discriminators:
            discriminators.sort(key=lambda d: d["confidence_suggestion"], reverse=True)
            result[model] = discriminators

    return result


# ---------------------------------------------------------------------------
# Clustering undetected printers
# ---------------------------------------------------------------------------


def cluster_undetected(
    profiles: list[dict],
    min_cluster_size: int = 3,
) -> list[dict]:
    """
    Cluster undetected printers by hardware similarity.

    Simple approach: group by (kinematics, build_volume_bucket, mcu).
    """
    buckets: dict[tuple, list[dict]] = defaultdict(list)

    for p in profiles:
        kin = get_kinematics(p)
        bv = get_build_volume(p)
        # Bucket volume to nearest 50mm
        bv_bucket = (
            round(bv[0] / 50) * 50 if bv[0] > 0 else 0,
            round(bv[1] / 50) * 50 if bv[1] > 0 else 0,
            round(bv[2] / 50) * 50 if bv[2] > 0 else 0,
        )
        mcu = get_mcu(p)

        key = (kin, bv_bucket, mcu)
        buckets[key].append(p)

    clusters = []
    for (kin, bv_bucket, mcu), members in sorted(
        buckets.items(), key=lambda x: -len(x[1])
    ):
        if len(members) < min_cluster_size:
            continue

        # Find common names within cluster
        freqs = compute_name_frequencies(members)
        common = find_common_names(freqs, len(members), threshold=0.5)

        # Find unique objects that could be heuristics
        obj_freqs = compute_object_frequencies(members)
        top_objects = [
            (name, count)
            for name, count in obj_freqs.most_common(20)
            if count >= len(members) * 0.5
        ]

        clusters.append(
            {
                "size": len(members),
                "kinematics": kin,
                "build_volume_bucket": {
                    "x": bv_bucket[0],
                    "y": bv_bucket[1],
                    "z": bv_bucket[2],
                },
                "mcu": mcu,
                "common_names": common,
                "top_printer_objects": [
                    {"name": n, "count": c} for n, c in top_objects
                ],
                "device_ids": list(
                    set(p.get("device_id", "?")[:8] for p in members)
                ),
            }
        )

    return clusters


# ---------------------------------------------------------------------------
# Candidate heuristic generation
# ---------------------------------------------------------------------------

HEURISTIC_TYPE_MAP: dict[str, str] = {
    "fans.names": "fan_match",
    "sensors.temperature_names": "sensor_match",
    "sensors.filament_names": "sensor_match",
    "leds.names": "led_match",
    "macros.names": "macro_match",
    "printer_objects": "object_exists",
}

HEURISTIC_FIELD_MAP: dict[str, str] = {
    "fans.names": "fans",
    "sensors.temperature_names": "sensors",
    "sensors.filament_names": "sensors",
    "leds.names": "leds",
    "macros.names": "macros",
    "printer_objects": "printer_objects",
}


def generate_candidate_heuristics(
    discriminators: dict[str, list[dict]],
) -> dict[str, list[dict]]:
    """
    Convert discriminating names into candidate heuristic entries
    compatible with printer_database.json format.
    """
    result: dict[str, list[dict]] = {}

    for model, disc_list in discriminators.items():
        heuristics = []
        for d in disc_list:
            source = d["source"]
            h_type = HEURISTIC_TYPE_MAP.get(source)
            h_field = HEURISTIC_FIELD_MAP.get(source)
            if not h_type or not h_field:
                continue

            heuristics.append(
                {
                    "type": h_type,
                    "field": h_field,
                    "pattern": d["name"],
                    "confidence": d["confidence_suggestion"],
                    "reason": (
                        f"Found in {d['prevalence_pct']}% of {model} devices, "
                        f"<={d['max_other_pct']}% of others "
                        f"(from telemetry analysis)"
                    ),
                }
            )

        if heuristics:
            result[model] = heuristics

    return result


def generate_cluster_heuristics(
    clusters: list[dict],
) -> list[dict]:
    """Generate candidate heuristic entries for undetected printer clusters."""
    result = []
    for i, cluster in enumerate(clusters):
        heuristics = []

        # From common names
        for source_key, name_list in cluster.get("common_names", {}).items():
            h_type = HEURISTIC_TYPE_MAP.get(source_key)
            h_field = HEURISTIC_FIELD_MAP.get(source_key)
            if not h_type or not h_field:
                continue
            for name, pct in name_list[:5]:  # top 5 per source
                heuristics.append(
                    {
                        "type": h_type,
                        "field": h_field,
                        "pattern": name,
                        "confidence": min(95, int(pct * 0.9)),
                        "reason": (
                            f"Found in {pct}% of cluster "
                            f"({cluster['kinematics']} "
                            f"{cluster['build_volume_bucket']['x']}x"
                            f"{cluster['build_volume_bucket']['y']}mm, "
                            f"n={cluster['size']})"
                        ),
                    }
                )

        # From printer objects
        for obj_info in cluster.get("top_printer_objects", [])[:5]:
            obj_name = obj_info["name"]
            # Skip very common Klipper builtins
            if obj_name in (
                "gcode_macro CANCEL_PRINT",
                "gcode_macro PAUSE",
                "gcode_macro RESUME",
                "heaters",
                "configfile",
                "gcode_move",
                "toolhead",
                "print_stats",
                "display_status",
            ):
                continue
            heuristics.append(
                {
                    "type": "object_exists",
                    "field": "printer_objects",
                    "pattern": obj_name,
                    "confidence": min(
                        90, int(obj_info["count"] / cluster["size"] * 90)
                    ),
                    "reason": (
                        f"Object in {obj_info['count']}/{cluster['size']} "
                        f"devices in cluster"
                    ),
                }
            )

        if heuristics:
            result.append(
                {
                    "cluster_id": i + 1,
                    "size": cluster["size"],
                    "kinematics": cluster["kinematics"],
                    "build_volume": cluster["build_volume_bucket"],
                    "mcu": cluster["mcu"],
                    "candidate_heuristics": heuristics,
                }
            )

    return result


# ---------------------------------------------------------------------------
# Validation
# ---------------------------------------------------------------------------


def validate_database(
    db_path: str,
    profiles: list[dict],
    group_freqs: dict[str, dict[NameSource, Counter]],
    group_sizes: dict[str, int],
) -> list[dict]:
    """
    Validate printer_database.json entries against telemetry data.

    Flags:
    - Entries with no telemetry matches
    - Heuristics that never fire in telemetry
    - Models detected in telemetry but not in database
    """
    issues: list[dict] = []

    try:
        with open(db_path) as f:
            db = json.load(f)
    except (json.JSONDecodeError, OSError) as e:
        return [{"severity": "error", "message": f"Cannot load database: {e}"}]

    db_models = set()
    for entry in db.get("printers", []):
        db_id = entry.get("id", "?")
        db_name = entry.get("name", "?")
        db_models.add(db_name)

        # Check if we have any telemetry for this model
        if db_name not in group_sizes or group_sizes[db_name] == 0:
            issues.append(
                {
                    "severity": "info",
                    "entry": db_id,
                    "message": f"No telemetry data for '{db_name}'",
                }
            )

    # Check for detected models not in database
    for model in group_sizes:
        if model and model not in db_models:
            issues.append(
                {
                    "severity": "warning",
                    "message": (
                        f"Model '{model}' detected {group_sizes[model]} times "
                        f"in telemetry but not found in database"
                    ),
                }
            )

    return issues


# ---------------------------------------------------------------------------
# Output formatting
# ---------------------------------------------------------------------------


def print_summary(
    profiles: list[dict],
    groups: dict[str, list[dict]],
    discriminators: dict[str, list[dict]],
    clusters: list[dict],
    validation_issues: list[dict],
) -> None:
    """Print a human-readable terminal summary."""
    total = len(profiles)
    detected = sum(len(v) for k, v in groups.items() if k)
    undetected = len(groups.get("", []))

    print(f"\n{'=' * 60}")
    print(f"  Printer Profile Analysis")
    print(f"{'=' * 60}")
    print(f"  Total hardware profiles: {total}")
    print(f"  Detected:    {detected} ({detected/total*100:.1f}%)" if total else "")
    print(
        f"  Undetected:  {undetected} ({undetected/total*100:.1f}%)" if total else ""
    )

    # Model distribution
    print(f"\n--- Detected Models ---")
    for model, members in sorted(groups.items(), key=lambda x: -len(x[1])):
        if not model:
            continue
        print(f"  {model:40s}  {len(members):4d}  ({len(members)/total*100:.1f}%)")

    # Discriminating names
    if discriminators:
        print(f"\n--- Discriminating Hardware Names ---")
        for model, disc_list in sorted(discriminators.items()):
            print(f"\n  {model}:")
            for d in disc_list[:5]:
                print(
                    f"    {d['source']:30s} {d['name']:30s}  "
                    f"{d['prevalence_pct']:5.1f}%  "
                    f"(conf={d['confidence_suggestion']})"
                )

    # Undetected clusters
    if clusters:
        print(f"\n--- Undetected Printer Clusters ---")
        for c in clusters:
            bv = c["build_volume_bucket"]
            print(
                f"\n  Cluster (n={c['size']}): {c['kinematics']} "
                f"{bv['x']}x{bv['y']}x{bv['z']}mm, MCU={c['mcu']}"
            )
            for source_key, name_list in c.get("common_names", {}).items():
                for name, pct in name_list[:3]:
                    print(f"    {source_key:30s} {name:30s}  {pct:.1f}%")

    # Validation
    if validation_issues:
        print(f"\n--- Database Validation ---")
        for issue in validation_issues:
            sev = issue["severity"].upper()
            entry = issue.get("entry", "")
            prefix = f"[{sev}] {entry}: " if entry else f"[{sev}] "
            print(f"  {prefix}{issue['message']}")

    print()


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------


def main():
    parser = argparse.ArgumentParser(
        description="Analyze hardware_profile telemetry for printer detection"
    )
    parser.add_argument(
        "--data-dir",
        default=".telemetry-data/events",
        help="Path to telemetry event data (default: .telemetry-data/events)",
    )
    parser.add_argument(
        "--db",
        default="config/printer_database.json",
        help="Path to printer_database.json (default: config/printer_database.json)",
    )
    parser.add_argument("--since", help="Filter events after YYYY-MM-DD")
    parser.add_argument("--until", help="Filter events before YYYY-MM-DD")
    parser.add_argument(
        "--json", action="store_true", help="Output as JSON"
    )
    parser.add_argument(
        "--candidates",
        action="store_true",
        help="Output candidate heuristics for printer_database.json",
    )
    parser.add_argument(
        "--validate",
        action="store_true",
        help="Validate database entries against telemetry",
    )
    parser.add_argument(
        "--min-cluster",
        type=int,
        default=3,
        help="Minimum cluster size for undetected grouping (default: 3)",
    )
    args = parser.parse_args()

    profiles = load_hardware_profiles(args.data_dir, args.since, args.until)
    if not profiles:
        print("No hardware_profile events found.", file=sys.stderr)
        sys.exit(1)

    # Group by detected model
    groups = group_by_model(profiles)
    group_sizes = {model: len(members) for model, members in groups.items()}

    # Compute name frequencies per group
    group_freqs: dict[str, dict[NameSource, Counter]] = {}
    for model, members in groups.items():
        group_freqs[model] = compute_name_frequencies(members)

    # Find discriminating names
    discriminators = find_discriminating_names(group_freqs, group_sizes)

    # Cluster undetected printers
    undetected = groups.get("", [])
    clusters = cluster_undetected(undetected, min_cluster_size=args.min_cluster)

    # Validate database
    validation_issues = validate_database(
        args.db, profiles, group_freqs, group_sizes
    )

    if args.json:
        output = {
            "total_profiles": len(profiles),
            "detected_count": sum(len(v) for k, v in groups.items() if k),
            "undetected_count": len(undetected),
            "model_distribution": {
                model: len(members)
                for model, members in sorted(groups.items(), key=lambda x: -len(x[1]))
                if model
            },
            "discriminating_names": discriminators,
            "undetected_clusters": clusters,
            "validation_issues": validation_issues,
        }
        if args.candidates:
            output["candidate_heuristics_detected"] = generate_candidate_heuristics(
                discriminators
            )
            output["candidate_heuristics_clusters"] = generate_cluster_heuristics(
                clusters
            )
        print(json.dumps(output, indent=2))

    elif args.candidates:
        detected_candidates = generate_candidate_heuristics(discriminators)
        cluster_candidates = generate_cluster_heuristics(clusters)

        if detected_candidates:
            print("\n--- Candidate Heuristics for Known Models ---")
            print(json.dumps(detected_candidates, indent=2))

        if cluster_candidates:
            print("\n--- Candidate Heuristics for Undetected Clusters ---")
            print(json.dumps(cluster_candidates, indent=2))

    else:
        print_summary(profiles, groups, discriminators, clusters, validation_issues)


if __name__ == "__main__":
    main()
