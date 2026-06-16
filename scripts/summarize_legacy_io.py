#!/usr/bin/env python3
"""Parse nvme_perf_model legacy IO logs and write summary CSV/TXT/JSON."""

from __future__ import annotations

import csv
import json
import re
import sys
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Optional

CASE_META: dict[str, dict[str, object]] = {
    "rand_read_4k": {
        "category": "random_read",
        "label": "Random Read",
        "block": "4K",
        "read_ratio": 100,
        "write_ratio": 0,
    },
    "rand_write_4k": {
        "category": "random_write",
        "label": "Random Write",
        "block": "4K",
        "read_ratio": 0,
        "write_ratio": 100,
    },
    "mixed_rw_70_30": {
        "category": "mixed",
        "label": "Mixed R/W 70/30",
        "block": "4K",
        "read_ratio": 70,
        "write_ratio": 30,
    },
    "mixed_rw_50_50": {
        "category": "mixed",
        "label": "Mixed R/W 50/50",
        "block": "4K",
        "read_ratio": 50,
        "write_ratio": 50,
    },
    "rand_read_128k": {
        "category": "random_read",
        "label": "Random Read",
        "block": "128K",
        "read_ratio": 100,
        "write_ratio": 0,
    },
    "rand_write_128k": {
        "category": "random_write",
        "label": "Random Write",
        "block": "128K",
        "read_ratio": 0,
        "write_ratio": 100,
    },
    "mixed_rw_70_30_128k": {
        "category": "mixed",
        "label": "Mixed R/W 70/30",
        "block": "128K",
        "read_ratio": 70,
        "write_ratio": 30,
    },
}

CASE_ORDER = list(CASE_META.keys())

PATTERNS = {
    "total_iops": re.compile(r"Performance = ([\d.]+) IOPS"),
    "read_iops": re.compile(r"Read IOPS = ([\d.]+)"),
    "write_iops": re.compile(r"Write IOPS = ([\d.]+)"),
    "erase_iops": re.compile(r"Erase IOPS = ([\d.]+)"),
    "read_bw_mbps": re.compile(r"Read Bandwidth \(sim\) = ([\d.]+) MB/s"),
    "write_bw_mbps": re.compile(r"Write Bandwidth \(sim\) = ([\d.]+) MB/s"),
    "total_bw_mbps": re.compile(r"Total Bandwidth \(sim\) = ([\d.]+) MB/s"),
    "read_bw_raw_mbps": re.compile(
        r"Read Bandwidth \(sim\).*?\n\s*sim\(raw\) = ([\d.]+) MB/s", re.S
    ),
    "write_bw_raw_mbps": re.compile(
        r"Write Bandwidth \(sim\).*?\n\s*sim\(raw\) = ([\d.]+) MB/s", re.S
    ),
    "pool_rejects": re.compile(r"Pool rejects = (\d+)"),
    "bus_xfers": re.compile(r"Bus xfers = (\d+)"),
}


@dataclass
class CaseResult:
    case: str
    category: str
    label: str
    block: str
    read_ratio: int
    write_ratio: int
    status: str
    total_iops: Optional[float] = None
    read_iops: Optional[float] = None
    write_iops: Optional[float] = None
    erase_iops: Optional[float] = None
    read_bw_mbps: Optional[float] = None
    write_bw_mbps: Optional[float] = None
    total_bw_mbps: Optional[float] = None
    read_bw_raw_mbps: Optional[float] = None
    write_bw_raw_mbps: Optional[float] = None
    pool_rejects: Optional[int] = None
    bus_xfers: Optional[int] = None
    log_path: str = ""


def _pick_float(text: str, key: str) -> Optional[float]:
    match = PATTERNS[key].search(text)
    if not match:
        return None
    return float(match.group(1))


def _pick_int(text: str, key: str) -> Optional[int]:
    match = PATTERNS[key].search(text)
    if not match:
        return None
    return int(match.group(1))


def parse_log(name: str, log_path: Path) -> CaseResult:
    meta = CASE_META.get(
        name,
        {
            "category": "unknown",
            "label": name,
            "block": "?",
            "read_ratio": 0,
            "write_ratio": 0,
        },
    )
    result = CaseResult(
        case=name,
        category=str(meta["category"]),
        label=str(meta["label"]),
        block=str(meta["block"]),
        read_ratio=int(meta["read_ratio"]),
        write_ratio=int(meta["write_ratio"]),
        status="MISSING",
        log_path=str(log_path),
    )
    if not log_path.is_file():
        return result

    text = log_path.read_text(encoding="utf-8", errors="replace")
    if "Failed to load config" in text or "Failed to init perf model" in text:
        result.status = "FAIL"
        return result

    result.total_iops = _pick_float(text, "total_iops")
    result.read_iops = _pick_float(text, "read_iops")
    result.write_iops = _pick_float(text, "write_iops")
    result.erase_iops = _pick_float(text, "erase_iops")
    result.read_bw_mbps = _pick_float(text, "read_bw_mbps")
    result.write_bw_mbps = _pick_float(text, "write_bw_mbps")
    result.total_bw_mbps = _pick_float(text, "total_bw_mbps")
    result.read_bw_raw_mbps = _pick_float(text, "read_bw_raw_mbps")
    result.write_bw_raw_mbps = _pick_float(text, "write_bw_raw_mbps")
    result.pool_rejects = _pick_int(text, "pool_rejects")
    result.bus_xfers = _pick_int(text, "bus_xfers")

    if result.total_iops is None:
        result.status = "FAIL"
    else:
        result.status = "PASS"
    return result


def collect_results(out_dir: Path) -> list[CaseResult]:
    rows: list[CaseResult] = []
    seen: set[str] = set()

    for name in CASE_ORDER:
        rows.append(parse_log(name, out_dir / f"{name}.log"))
        seen.add(name)

    for log_path in sorted(out_dir.glob("*.log")):
        name = log_path.stem
        if name not in seen:
            rows.append(parse_log(name, log_path))
    return rows


def write_csv(path: Path, rows: list[CaseResult]) -> None:
    fields = [
        "case",
        "category",
        "label",
        "block",
        "read_ratio",
        "write_ratio",
        "status",
        "total_iops",
        "read_iops",
        "write_iops",
        "erase_iops",
        "read_bw_mbps",
        "write_bw_mbps",
        "total_bw_mbps",
        "read_bw_raw_mbps",
        "write_bw_raw_mbps",
        "pool_rejects",
        "bus_xfers",
        "log_path",
    ]
    with path.open("w", newline="", encoding="utf-8") as fp:
        writer = csv.DictWriter(fp, fieldnames=fields)
        writer.writeheader()
        for row in rows:
            writer.writerow(asdict(row))


def _fmt_num(value: Optional[float], width: int = 10) -> str:
    if value is None:
        return f"{'—':>{width}}"
    return f"{value:>{width}.2f}"


def write_txt(path: Path, rows: list[CaseResult], out_dir: Path) -> None:
    header = (
        f"{'Case':<22} {'Type':<13} {'Block':<6} {'R/W':<9} "
        f"{'Read IOPS':>11} {'Write IOPS':>11} {'Read MB/s':>10} "
        f"{'Write MB/s':>10} {'Total MB/s':>10} {'Status':<6}"
    )
    lines = [
        "Legacy IO Benchmark Summary",
        f"Output directory: {out_dir}",
        "",
        header,
        "-" * len(header),
    ]
    for row in rows:
        rw = f"{row.read_ratio}/{row.write_ratio}"
        lines.append(
            f"{row.case:<22} {row.category:<13} {row.block:<6} {rw:<9} "
            f"{_fmt_num(row.read_iops, 11)} {_fmt_num(row.write_iops, 11)} "
            f"{_fmt_num(row.read_bw_mbps, 10)} {_fmt_num(row.write_bw_mbps, 10)} "
            f"{_fmt_num(row.total_bw_mbps, 10)} {row.status:<6}"
        )

    groups = [
        ("Random Read (4K)", "random_read", "4K"),
        ("Random Write (4K)", "random_write", "4K"),
        ("Mixed (4K)", "mixed", "4K"),
        ("Random Read (128K)", "random_read", "128K"),
        ("Random Write (128K)", "random_write", "128K"),
        ("Mixed (128K)", "mixed", "128K"),
    ]
    lines.extend(["", "Grouped Summary", ""])
    for title, category, block in groups:
        lines.append(title + ":")
        subset = [r for r in rows if r.category == category and r.block == block]
        if not subset:
            lines.append("  (no results)")
            continue
        for row in subset:
            lines.append(
                f"  {row.label:<18} read={_fmt_num(row.read_iops, 1).strip()} IOPS "
                f"({_fmt_num(row.read_bw_mbps, 1).strip()} MB/s)  "
                f"write={_fmt_num(row.write_iops, 1).strip()} IOPS "
                f"({_fmt_num(row.write_bw_mbps, 1).strip()} MB/s)  "
                f"[{row.status}]"
            )
        lines.append("")

    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def print_table(rows: list[CaseResult]) -> None:
    header = (
        f"{'Case':<22} {'Type':<13} {'Block':<6} {'R/W':<9} "
        f"{'Read IOPS':>11} {'Write IOPS':>11} {'Read MB/s':>10} "
        f"{'Write MB/s':>10} {'Total MB/s':>10} {'Status':<6}"
    )
    print("")
    print("======== Legacy IO Benchmark Summary ========")
    print(header)
    print("-" * len(header))
    for row in rows:
        rw = f"{row.read_ratio}/{row.write_ratio}"
        print(
            f"{row.case:<22} {row.category:<13} {row.block:<6} {rw:<9} "
            f"{_fmt_num(row.read_iops, 11)} {_fmt_num(row.write_iops, 11)} "
            f"{_fmt_num(row.read_bw_mbps, 10)} {_fmt_num(row.write_bw_mbps, 10)} "
            f"{_fmt_num(row.total_bw_mbps, 10)} {row.status:<6}"
        )


def main() -> int:
    out_dir = Path(sys.argv[1] if len(sys.argv) > 1 else "tests/out")
    if not out_dir.is_dir():
        print(f"error: output directory not found: {out_dir}", file=sys.stderr)
        return 1

    rows = collect_results(out_dir)
    write_csv(out_dir / "summary.csv", rows)
    write_json(out_dir / "summary.json", rows)
    write_txt(out_dir / "summary.txt", rows, out_dir)
    print_table(rows)

    print("")
    print(f"Wrote {out_dir / 'summary.csv'}")
    print(f"Wrote {out_dir / 'summary.txt'}")
    print(f"Wrote {out_dir / 'summary.json'}")

    failed = [r for r in rows if r.status != "PASS"]
    if failed:
        print(f"{len(failed)} case(s) missing or failed.", file=sys.stderr)
        return 1
    return 0


def write_json(path: Path, rows: list[CaseResult]) -> None:
    payload = {
        "cases": [asdict(row) for row in rows],
        "groups": {
            "random_read_4k": [
                asdict(r)
                for r in rows
                if r.category == "random_read" and r.block == "4K"
            ],
            "random_write_4k": [
                asdict(r)
                for r in rows
                if r.category == "random_write" and r.block == "4K"
            ],
            "mixed_4k": [
                asdict(r) for r in rows if r.category == "mixed" and r.block == "4K"
            ],
            "random_read_128k": [
                asdict(r)
                for r in rows
                if r.category == "random_read" and r.block == "128K"
            ],
            "random_write_128k": [
                asdict(r)
                for r in rows
                if r.category == "random_write" and r.block == "128K"
            ],
            "mixed_128k": [
                asdict(r) for r in rows if r.category == "mixed" and r.block == "128K"
            ],
        },
    }
    path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    raise SystemExit(main())
