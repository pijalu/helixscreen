#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""
HelixScreen Crash Debugger

Resolves ASLR-randomized backtrace addresses from crash telemetry events
into human-readable function names, groups crashes by stack signature,
and provides summary/detail views.

Usage:
    telemetry-crashes.py --since 2026-02-09           # Summary of recent crashes
    telemetry-crashes.py --since 2026-02-09 --detail   # Full resolved backtraces
    telemetry-crashes.py --version 0.9.12 --detail     # Filter by version
    telemetry-crashes.py --sig a3f82b1c                # Zoom into one signature
    telemetry-crashes.py --json                        # Machine-readable output
"""

import argparse
import hashlib
import json
import os
import sys
import urllib.request
import urllib.error
from bisect import bisect_right
from pathlib import Path
from typing import Optional


# ---------------------------------------------------------------------------
# Symbol table
# ---------------------------------------------------------------------------

class SymbolTable:
    """Parsed nm -nC output with binary-search lookup."""

    # Linker/runtime boundary symbols that aren't real functions.
    # Resolving to these means the unwind landed in a gap.
    GARBAGE_SYMBOLS = frozenset({
        "data_start", "_edata", "_end", "__bss_start", "__bss_start__",
        "__bss_end__", "__data_start", "__dso_handle", "__libc_csu_init",
        "__libc_csu_fini", "_fini", "_init", "_fp_hw", "_IO_stdin_used",
        "__init_array_start", "__init_array_end", "__fini_array_start",
        "__fini_array_end", "__FRAME_END__", "__GNU_EH_FRAME_HDR",
        "__TMC_END__", "__ehdr_start", "__exidx_start", "__exidx_end",
        "_GLOBAL_OFFSET_TABLE_", "_DYNAMIC", "_PROCEDURE_LINKAGE_TABLE_",
        "completed.0",
    })

    def __init__(self, entries: list[tuple[int, str]]):
        # entries: sorted list of (address, demangled_name)
        self.addrs = [a for a, _ in entries]
        self.names = [n for _, n in entries]
        self.crash_handler_offset: Optional[int] = None
        self._find_crash_handler()

    @classmethod
    def from_file(cls, path: Path) -> "SymbolTable":
        import re
        lto_pattern = re.compile(r'\.\w+\.[0-9a-f]{6,}$')
        entries: list[tuple[int, str]] = []
        with open(path, "r") as f:
            for line in f:
                line = line.rstrip("\n")
                if not line:
                    continue
                parts = line.split(None, 2)
                if len(parts) < 3:
                    continue
                addr_str, sym_type, name = parts[0], parts[1], parts[2]
                if sym_type not in ("T", "t", "W", "w"):
                    continue
                try:
                    addr = int(addr_str, 16)
                except ValueError:
                    continue
                if addr == 0:
                    continue
                # Filter LTO compilation-unit section markers (e.g., "foo.c.35cb4f60")
                if lto_pattern.search(name):
                    continue
                entries.append((addr, name))
        entries.sort(key=lambda x: x[0])
        return cls(entries)

    def _find_crash_handler(self) -> None:
        for addr, name in zip(self.addrs, self.names):
            if "crash_signal_handler" in name:
                self.crash_handler_offset = addr
                return

    def lookup(self, file_offset: int) -> str:
        """Resolve a file offset to 'func_name+0xNN'."""
        if not self.addrs:
            return f"0x{file_offset:x}"
        idx = bisect_right(self.addrs, file_offset) - 1
        if idx < 0:
            return f"0x{file_offset:x}"
        base = self.addrs[idx]
        name = self.names[idx]
        # Filter garbage linker boundary symbols (data_start, _edata, etc.)
        if name in self.GARBAGE_SYMBOLS:
            return f"(unknown @ 0x{file_offset:x})"
        offset = file_offset - base
        if offset == 0:
            return name
        return f"{name}+0x{offset:x}"


# ---------------------------------------------------------------------------
# Symbol cache
# ---------------------------------------------------------------------------

CACHE_DIR = Path(os.environ.get("XDG_CACHE_HOME", Path.home() / ".cache")) / "helixscreen" / "symbols"
R2_BASE_URL = os.environ.get("HELIX_R2_URL", "https://releases.helixscreen.org") + "/symbols"


class SymbolCache:
    """Downloads and caches symbol files from R2."""

    def __init__(self):
        self._tables: dict[str, Optional[SymbolTable]] = {}
        self._warnings: list[str] = []

    @property
    def warnings(self) -> list[str]:
        return self._warnings

    def get(self, version: str, platform: str) -> Optional[SymbolTable]:
        key = f"{version}/{platform}"
        if key in self._tables:
            return self._tables[key]

        sym_path = CACHE_DIR / f"v{version}" / f"{platform}.sym"

        # Download if not cached
        if not sym_path.exists():
            sym_path.parent.mkdir(parents=True, exist_ok=True)
            url = f"{R2_BASE_URL}/v{version}/{platform}.sym"
            try:
                print(f"  Downloading symbols for v{version}/{platform}...", file=sys.stderr)
                req = urllib.request.Request(url, headers={"User-Agent": "helixscreen-crashes/1.0"})
                with urllib.request.urlopen(req) as resp, open(sym_path, "wb") as out:
                    out.write(resp.read())
            except urllib.error.HTTPError as e:
                self._warnings.append(f"v{version}/{platform}: symbols not available (HTTP {e.code})")
                self._tables[key] = None
                return None
            except urllib.error.URLError as e:
                self._warnings.append(f"v{version}/{platform}: download failed ({e.reason})")
                self._tables[key] = None
                return None

        # Validate non-empty
        if sym_path.stat().st_size == 0:
            self._warnings.append(f"v{version}/{platform}: symbol file is empty (broken upload?)")
            self._tables[key] = None
            return None

        table = SymbolTable.from_file(sym_path)
        if not table.addrs:
            self._warnings.append(f"v{version}/{platform}: no text symbols found in .sym file")
            self._tables[key] = None
            return None

        if table.crash_handler_offset is None:
            self._warnings.append(f"v{version}/{platform}: crash_signal_handler not found in symbols")

        self._tables[key] = table
        return table


# ---------------------------------------------------------------------------
# ASLR resolution
# ---------------------------------------------------------------------------

def is_shared_lib_addr(addr: int, platform: str) -> bool:
    """Detect shared library addresses (not from our binary).

    aarch64 (pi): binary at 0x0000aaaa..., shared libs at 0x0000ffff...
    armhf (pi32): binary at low addresses, shared libs at 0xf0000000+
    """
    if platform in ("pi", "rpi4_64bit"):
        # aarch64 PIE: our binary is loaded at 0x0000aaaa_XXXXXXXX
        # Shared libs live at 0x0000ffff_XXXXXXXX
        top_word = (addr >> 32) & 0xFFFF
        return top_word >= 0xFFFF
    elif platform == "pi32":
        # armhf: shared libs mapped at 0xf0000000+
        return addr >= 0xF0000000
    return False


def detect_platform_from_addrs(backtrace: list[int]) -> str:
    """Heuristic: 64-bit pi addresses have 0xaaaa or 0xffff in upper bits."""
    for addr in backtrace:
        if addr > 0xFFFFFFFF:
            return "pi"
    return "pi32"


def resolve_backtrace(
    backtrace: list[str],
    platform: str,
    symbols: Optional[SymbolTable],
    load_base: Optional[str] = None,
) -> list[dict]:
    """Resolve a crash backtrace to named frames.

    Returns list of {addr, resolved, is_shared_lib} dicts.
    """
    if not backtrace:
        return []

    addrs = []
    for addr_str in backtrace:
        try:
            addrs.append(int(addr_str, 16))
        except ValueError:
            addrs.append(0)

    frames: list[dict] = []

    if symbols is None or symbols.crash_handler_offset is None:
        # Can't resolve — return raw addresses
        for i, addr in enumerate(addrs):
            is_lib = is_shared_lib_addr(addr, platform)
            frames.append({
                "addr": f"0x{addr:x}",
                "resolved": "<shared lib>" if is_lib else f"0x{addr:x}",
                "is_shared_lib": is_lib,
            })
        return frames

    # Determine ASLR base address
    if load_base is not None:
        # Explicit load_base from crash event (preferred — no heuristic needed)
        try:
            base_address = int(load_base, 16)
        except ValueError:
            base_address = 0
    elif addrs and addrs[0] > symbols.crash_handler_offset:
        # Legacy heuristic: frame 0 is a return address inside crash_signal_handler.
        # Check if addresses are already file-relative by testing frame 0 against symbols.
        naive_file_offset = addrs[0]
        resolved_test = symbols.lookup(naive_file_offset)
        if "crash_signal_handler" in resolved_test:
            # Addresses are already file-relative (base=0)
            base_address = 0
        else:
            # Addresses are ASLR'd. Frame 0 = base + crash_handler_offset + intra_offset.
            # Best estimate: base = frame0 - crash_handler_offset (off by intra_offset).
            base_address = addrs[0] - symbols.crash_handler_offset
    else:
        base_address = 0

    for i, addr in enumerate(addrs):
        is_lib = is_shared_lib_addr(addr, platform)
        if is_lib:
            frames.append({
                "addr": f"0x{addr:x}",
                "resolved": "<shared lib>",
                "is_shared_lib": True,
            })
        else:
            file_offset = addr - base_address
            resolved = symbols.lookup(file_offset)
            frames.append({
                "addr": f"0x{addr:x}",
                "resolved": resolved,
                "is_shared_lib": False,
            })

    return frames


# ---------------------------------------------------------------------------
# Stack signature
# ---------------------------------------------------------------------------

def compute_signature(frames: list[dict]) -> str:
    """Hash resolved function names (no offsets) to group identical crashes.

    Skips frame 0 (crash_signal_handler) and shared lib frames.
    When frames are unresolved (raw hex), uses relative offsets from frame 0
    so that ASLR-randomized addresses still group correctly.
    """
    sig_parts = []
    # Check if we have resolved symbols (any frame has a non-hex name)
    has_symbols = any(
        not f["is_shared_lib"] and not f["resolved"].startswith("0x")
        for f in frames[1:]  # skip frame 0
    )

    if has_symbols:
        for i, frame in enumerate(frames):
            if i == 0:
                continue
            if frame["is_shared_lib"]:
                continue
            name = frame["resolved"]
            plus_idx = name.rfind("+0x")
            if plus_idx > 0:
                name = name[:plus_idx]
            sig_parts.append(name)
    else:
        # No symbols: compute relative offsets from frame 0 for grouping
        # This makes ASLR-randomized addresses produce the same signature
        base_addr = None
        for frame in frames:
            if not frame["is_shared_lib"]:
                try:
                    base_addr = int(frame["addr"], 16)
                except ValueError:
                    pass
                break
        if base_addr is not None:
            for i, frame in enumerate(frames):
                if i == 0:
                    continue
                if frame["is_shared_lib"]:
                    continue
                try:
                    addr = int(frame["addr"], 16)
                    rel = addr - base_addr
                    sig_parts.append(f"rel+{rel:#x}")
                except ValueError:
                    sig_parts.append(frame["resolved"])

    if not sig_parts:
        return "unknown"

    sig_str = "\n".join(sig_parts)
    return hashlib.sha256(sig_str.encode()).hexdigest()[:8]


# ---------------------------------------------------------------------------
# Event loading (reuses patterns from telemetry-analyze.py)
# ---------------------------------------------------------------------------

def find_project_root() -> Path:
    path = Path(__file__).resolve().parent
    for _ in range(10):
        if (path / ".git").exists() or (path / "Makefile").exists():
            return path
        path = path.parent
    return Path.cwd()


def load_events(
    data_dir: str,
    since: Optional[str] = None,
    until: Optional[str] = None,
) -> tuple[list[dict], list[dict]]:
    """Load crash and session events. Returns (crashes, sessions)."""
    from datetime import datetime, timezone

    data_path = Path(data_dir)
    if not data_path.exists():
        print(f"Data directory not found: {data_path}", file=sys.stderr)
        return [], []

    crashes: list[dict] = []
    sessions: list[dict] = []
    file_count = 0

    # Parse date filters
    since_dt = None
    until_dt = None
    if since:
        since_dt = datetime.strptime(since, "%Y-%m-%d").replace(tzinfo=timezone.utc)
    if until:
        until_dt = datetime.strptime(until, "%Y-%m-%d").replace(tzinfo=timezone.utc)
        # Include the entire "until" day
        until_dt = until_dt.replace(hour=23, minute=59, second=59)

    for fpath in sorted(data_path.rglob("*.json")):
        file_count += 1
        try:
            with open(fpath, "r") as f:
                data = json.load(f)
        except (json.JSONDecodeError, OSError):
            continue

        events = data if isinstance(data, list) else [data]
        for ev in events:
            # Date filter
            ts_str = ev.get("timestamp")
            if ts_str and (since_dt or until_dt):
                try:
                    ts = datetime.fromisoformat(ts_str.replace("Z", "+00:00"))
                    if since_dt and ts < since_dt:
                        continue
                    if until_dt and ts > until_dt:
                        continue
                except ValueError:
                    pass

            if ev.get("event") == "crash":
                crashes.append(ev)
            elif ev.get("event") == "session":
                sessions.append(ev)

    print(f"Loaded {len(crashes)} crashes, {len(sessions)} sessions from {file_count} files", file=sys.stderr)
    return crashes, sessions


# ---------------------------------------------------------------------------
# Platform resolution
# ---------------------------------------------------------------------------

def build_device_platform_map(sessions: list[dict]) -> dict[str, str]:
    """Map device_id → platform from session events."""
    device_map: dict[str, str] = {}
    for s in sessions:
        did = s.get("device_id")
        plat = (s.get("app") or {}).get("platform")
        if did and plat:
            # Normalize rpi4_64bit → pi
            if plat == "rpi4_64bit":
                plat = "pi"
            device_map[did] = plat
    return device_map


def get_platform(
    crash: dict,
    device_map: dict[str, str],
    override: Optional[str] = None,
) -> str:
    """Determine platform for a crash event."""
    if override:
        return override
    did = crash.get("device_id", "")
    if did in device_map:
        return device_map[did]
    # Fallback: heuristic from addresses
    bt = crash.get("backtrace", [])
    addrs = []
    for a in bt:
        try:
            addrs.append(int(a, 16))
        except ValueError:
            pass
    return detect_platform_from_addrs(addrs)


# ---------------------------------------------------------------------------
# Analysis
# ---------------------------------------------------------------------------

def analyze_crashes(
    crashes: list[dict],
    sessions: list[dict],
    symbol_cache: SymbolCache,
    platform_override: Optional[str] = None,
    version_filter: Optional[str] = None,
    sig_filter: Optional[str] = None,
) -> dict:
    """Analyze crashes: resolve symbols, group by signature."""
    device_map = build_device_platform_map(sessions)

    # Filter by version if requested
    if version_filter:
        crashes = [c for c in crashes if c.get("app_version") == version_filter]

    signatures: dict[str, dict] = {}  # sig_hash → group info

    for crash in crashes:
        version = crash.get("app_version", "unknown")
        platform = get_platform(crash, device_map, platform_override)
        backtrace = crash.get("backtrace", [])
        device_id = crash.get("device_id", "")
        uptime = crash.get("uptime_sec", 0)
        signal_name = crash.get("signal_name", "?")
        timestamp = crash.get("timestamp", "")

        # Get symbols
        symbols = symbol_cache.get(version, platform)

        # Resolve backtrace
        load_base = crash.get("load_base")
        frames = resolve_backtrace(backtrace, platform, symbols, load_base=load_base)

        # If backtrace is shallow (only crash_handler + signal_restorer),
        # inject reg_pc and reg_lr as additional frames for resolution.
        # This recovers the actual crash location from ucontext registers
        # on platforms where backtrace() can't unwind past signal frames.
        non_lib = [f for f in frames if not f["is_shared_lib"]]
        if len(non_lib) <= 2:
            reg_addrs = []
            for reg_key in ("reg_pc", "reg_lr"):
                reg_val = crash.get(reg_key)
                if reg_val:
                    reg_addrs.append(reg_val)
            if reg_addrs:
                reg_frames = resolve_backtrace(reg_addrs, platform, symbols, load_base=load_base)
                useful_reg = [f for f in reg_frames if not f["is_shared_lib"]
                              and "crash_signal_handler" not in f["resolved"]]
                if useful_reg:
                    frames = useful_reg + frames

        # Compute signature
        sig = compute_signature(frames)

        # Check if we're filtering to a specific signature
        if sig_filter and not sig.startswith(sig_filter):
            continue

        # Warn about pi32 shallow backtraces
        non_lib_frames = [f for f in frames if not f["is_shared_lib"]]
        shallow = len(non_lib_frames) <= 2

        if sig not in signatures:
            signatures[sig] = {
                "sig": sig,
                "count": 0,
                "signal": signal_name,
                "versions": set(),
                "devices": set(),
                "platforms": set(),
                "uptimes": [],
                "timestamps": [],
                "frames": frames,  # representative backtrace
                "shallow": shallow,
                "instances": [],
            }

        group = signatures[sig]
        group["count"] += 1
        group["versions"].add(version)
        group["devices"].add(device_id[:8])
        group["platforms"].add(platform)
        group["uptimes"].append(uptime)
        group["timestamps"].append(timestamp)
        group["instances"].append({
            "version": version,
            "platform": platform,
            "device": device_id[:8],
            "uptime": uptime,
            "signal": signal_name,
            "timestamp": timestamp,
            "frames": frames,
        })

    # Sort by count descending
    sorted_sigs = sorted(signatures.values(), key=lambda g: -g["count"])

    return {
        "total_crashes": len(crashes),
        "total_signatures": len(sorted_sigs),
        "signatures": sorted_sigs,
        "warnings": symbol_cache.warnings,
    }


# ---------------------------------------------------------------------------
# Output formatting
# ---------------------------------------------------------------------------

def format_frame(frame: dict, index: int) -> str:
    """Format a single backtrace frame."""
    marker = "→" if index == 0 else " "
    return f"  {marker} #{index:<2} {frame['addr']:>20s}  {frame['resolved']}"


def format_terminal(result: dict, detail: bool = False) -> str:
    lines: list[str] = []
    sep = "=" * 70

    lines.append(sep)
    lines.append("  HELIXSCREEN CRASH DEBUGGER")
    lines.append(sep)
    lines.append(f"  Total crashes: {result['total_crashes']}")
    lines.append(f"  Unique signatures: {result['total_signatures']}")

    if result["warnings"]:
        lines.append("")
        lines.append("  Warnings:")
        for w in result["warnings"]:
            lines.append(f"    ⚠ {w}")

    lines.append("")

    for group in result["signatures"]:
        sig = group["sig"]
        count = group["count"]
        signal = group["signal"]
        versions = sorted(group["versions"])
        devices = sorted(group["devices"])
        platforms = sorted(group["platforms"])
        uptimes = group["uptimes"]

        # Top-of-stack preview (first non-handler, non-shared-lib frame)
        top_func = "?"
        for i, frame in enumerate(group["frames"]):
            if i == 0:
                continue
            if not frame["is_shared_lib"] and not frame["resolved"].startswith("0x"):
                top_func = frame["resolved"]
                # Strip offset for preview
                plus_idx = top_func.rfind("+0x")
                if plus_idx > 0:
                    top_func = top_func[:plus_idx]
                break

        lines.append(f"  [{sig}] {count}x {signal} — {top_func}")
        lines.append(f"    versions: {', '.join(f'v{v}' for v in versions)}  |  "
                      f"platforms: {', '.join(platforms)}  |  "
                      f"devices: {len(devices)}")

        if uptimes:
            min_up = min(uptimes)
            max_up = max(uptimes)
            if min_up == max_up:
                lines.append(f"    uptime: {_fmt_duration(min_up)}")
            else:
                lines.append(f"    uptime: {_fmt_duration(min_up)} — {_fmt_duration(max_up)}")

        if group["shallow"]:
            lines.append("    ⚠ shallow backtrace (pi32?) — grouping may be unreliable")

        if detail:
            for inst in group["instances"]:
                lines.append(f"    ── v{inst['version']} {inst['platform']} "
                              f"dev={inst['device']} uptime={inst['uptime']}s "
                              f"{inst['timestamp']}")
                for idx, frame in enumerate(inst["frames"]):
                    lines.append(format_frame(frame, idx))
            lines.append("")
        else:
            # Show representative backtrace (condensed)
            frames = group["frames"]
            if frames:
                for idx, frame in enumerate(frames):
                    if idx > 8:
                        lines.append(f"       ... +{len(frames) - 8} more frames")
                        break
                    lines.append(format_frame(frame, idx))
            lines.append("")

    if not result["signatures"]:
        lines.append("  No crashes found matching filters.")

    lines.append(sep)
    return "\n".join(lines)


def format_json_output(result: dict) -> str:
    """JSON output with sets converted to lists."""
    def serialize(obj):
        if isinstance(obj, set):
            return sorted(obj)
        return str(obj)
    return json.dumps(result, indent=2, default=serialize)


def _fmt_duration(seconds) -> str:
    seconds = float(seconds)
    if seconds < 60:
        return f"{seconds:.0f}s"
    if seconds < 3600:
        return f"{seconds / 60:.1f}min"
    return f"{seconds / 3600:.1f}hr"


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        description="HelixScreen Crash Debugger — resolve ASLR backtraces from telemetry",
    )
    parser.add_argument("--since", metavar="YYYY-MM-DD", help="Include crashes on or after this date")
    parser.add_argument("--until", metavar="YYYY-MM-DD", help="Include crashes on or before this date")
    parser.add_argument("--version", metavar="VER", help="Filter to specific app version (e.g. 0.9.12)")
    parser.add_argument("--platform", metavar="PLAT", help="Override platform detection (pi, pi32)")
    parser.add_argument("--sig", metavar="HASH", help="Show only crashes matching this signature prefix")
    parser.add_argument("--detail", action="store_true", help="Show full resolved backtraces per instance")
    parser.add_argument("--json", action="store_true", help="Machine-readable JSON output")
    parser.add_argument("--data-dir", metavar="PATH", help="Override telemetry data directory")
    args = parser.parse_args()

    # Resolve data dir
    if args.data_dir:
        data_dir = args.data_dir
    else:
        root = find_project_root()
        data_dir = str(root / ".telemetry-data" / "events")

    # Load events
    crashes, sessions = load_events(data_dir, since=args.since, until=args.until)
    if not crashes:
        print("No crashes found.", file=sys.stderr)
        sys.exit(0)

    # Analyze
    cache = SymbolCache()
    result = analyze_crashes(
        crashes,
        sessions,
        cache,
        platform_override=args.platform,
        version_filter=args.version,
        sig_filter=args.sig,
    )

    # Output
    if args.json:
        print(format_json_output(result))
    else:
        print(format_terminal(result, detail=args.detail))


if __name__ == "__main__":
    main()
