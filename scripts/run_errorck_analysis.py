#!/usr/bin/env python3

"""Run errorck analyses and summarize ignored calls.

Outputs:
  all_funcs_report.db
  all_funcs.txt
  all_report.db
  all_ignored.txt
  notable_report.db
  notable_ignored.txt
  report.db
  ignored.txt
"""

from __future__ import annotations

import argparse
from pathlib import Path
import subprocess
import sys


def resolve_compdb_path(path: Path) -> Path:
    if path.is_dir():
        return path
    if path.name == "compile_commands.json":
        return path.parent
    raise ValueError(f"Compilation database not found: {path}")


def resolve_errorck_path(repo_root: Path, value: str | None) -> Path:
    if value:
        return Path(value)
    candidate = repo_root / "build" / "errorck"
    return candidate


def run(cmd: list[str]) -> None:
    print(" ".join(cmd), file=sys.stderr)
    subprocess.run(cmd, check=True)


def write_list_calls(list_calls: Path, db: Path, handling: str, out: Path) -> None:
    out.write_text(
        subprocess.check_output(
            [sys.executable, str(list_calls), str(db), handling]
        ).decode()
    )


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Run errorck analyses and produce summary files."
    )
    parser.add_argument(
        "--notable-functions",
        required=True,
        help="Path to notable functions.json",
    )
    parser.add_argument(
        "--ignored-functions",
        required=True,
        help="Path to functions.json that are okay to ignore",
    )
    parser.add_argument(
        "--compdb",
        required=True,
        help="Path to compile_commands.json or its directory",
    )
    parser.add_argument(
        "--errorck",
        help="Path to errorck binary (default: build/errorck)",
    )
    parser.add_argument(
        "--output-dir",
        default=".",
        help="Directory for outputs (default: current directory)",
    )
    parser.add_argument("files", nargs="+", help="Files to analyze")
    args = parser.parse_args()

    repo_root = Path(__file__).resolve().parents[1]
    errorck = resolve_errorck_path(repo_root, args.errorck)
    if not errorck.exists():
        print(f"errorck binary not found: {errorck}", file=sys.stderr)
        return 2

    try:
        compdb_dir = resolve_compdb_path(Path(args.compdb))
    except ValueError as exc:
        print(str(exc), file=sys.stderr)
        return 2
    notable = Path(args.notable_functions)
    ignored = Path(args.ignored_functions)
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    list_calls = repo_root / "scripts" / "list_calls.py"
    if not list_calls.exists():
        print(f"list_calls.py not found: {list_calls}", file=sys.stderr)
        return 2

    files = [str(Path(f)) for f in args.files]

    all_funcs_db = output_dir / "all_funcs_report.db"
    all_funcs_txt = output_dir / "all_funcs.txt"
    all_report_db = output_dir / "all_report.db"
    all_ignored_txt = output_dir / "all_ignored.txt"
    notable_db = output_dir / "notable_report.db"
    notable_ignored_txt = output_dir / "notable_ignored.txt"
    report_db = output_dir / "report.db"
    ignored_txt = output_dir / "ignored.txt"

    run(
        [
            str(errorck),
            "--list-non-void-calls",
            "--db",
            str(all_funcs_db),
            "--overwrite-if-needed",
            "-p",
            str(compdb_dir),
        ]
        + files
    )
    write_list_calls(list_calls, all_funcs_db, "observed_non_void", all_funcs_txt)

    run(
        [
            str(errorck),
            "--all-non-void",
            "--db",
            str(all_report_db),
            "--overwrite-if-needed",
            "-p",
            str(compdb_dir),
        ]
        + files
    )
    write_list_calls(list_calls, all_report_db, "ignored", all_ignored_txt)

    run(
        [
            str(errorck),
            "--notable-functions",
            str(notable),
            "--db",
            str(notable_db),
            "--overwrite-if-needed",
            "-p",
            str(compdb_dir),
        ]
        + files
    )
    write_list_calls(list_calls, notable_db, "ignored", notable_ignored_txt)

    run(
        [
            str(errorck),
            "--exclude-notable-functions",
            "--notable-functions",
            str(ignored),
            "--db",
            str(report_db),
            "--overwrite-if-needed",
            "-p",
            str(compdb_dir),
        ]
        + files
    )
    write_list_calls(list_calls, report_db, "ignored", ignored_txt)

    return 0


if __name__ == "__main__":
    sys.exit(main())
