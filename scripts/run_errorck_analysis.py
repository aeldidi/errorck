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
import json
from pathlib import Path
import subprocess
import sys
from typing import NamedTuple


def resolve_compdb_path(path: Path) -> Path:
    if path.is_dir():
        return path
    if path.name == "compile_commands.json":
        return path.parent
    raise ValueError(f"Compilation database not found: {path}")


def resolve_errorck_path(repo_root: Path, value: str | None) -> Path:
    if value:
        return Path(value).expanduser()
    return repo_root / "build" / "errorck"


def load_compdb_files(compdb_dir: Path) -> set[Path]:
    compdb_path = compdb_dir / "compile_commands.json"
    if not compdb_path.is_file():
        raise ValueError(f"Compilation database not found: {compdb_path}")
    data = json.loads(compdb_path.read_text())
    files: set[Path] = set()
    for entry in data:
        file_value = entry.get("file")
        if not file_value:
            continue
        path = Path(file_value)
        if not path.is_absolute():
            directory = entry.get("directory")
            if directory:
                path = Path(directory) / path
        files.add(path.expanduser().resolve())
    return files


class ErrorckRun(NamedTuple):
    name: str
    cmd: list[str]
    db: Path
    handling: str
    out: Path


def run_errorck_jobs(runs: list[ErrorckRun]) -> dict[str, int]:
    procs: dict[str, subprocess.Popen] = {}
    for run in runs:
        print(" ".join(run.cmd), file=sys.stderr)
        procs[run.name] = subprocess.Popen(run.cmd)

    results: dict[str, int] = {}
    for run in runs:
        rc = procs[run.name].wait()
        if rc == 1:
            print(
                f"{run.name} reported analysis errors (exit 1); continuing.",
                file=sys.stderr,
            )
        elif rc != 0:
            print(f"{run.name} failed with exit code {rc}.", file=sys.stderr)
        results[run.name] = rc
    return results


def write_list_calls(
    list_calls: Path,
    db: Path,
    handling: str,
    out: Path,
    allow_missing_db: bool,
) -> bool:
    if not db.exists():
        msg = f"list_calls skipped; database not found: {db}"
        if allow_missing_db:
            print(f"{msg}.", file=sys.stderr)
            return True
        print(msg, file=sys.stderr)
        return False

    cmd = [sys.executable, str(list_calls), str(db), handling]
    print(" ".join(cmd), file=sys.stderr)
    proc = subprocess.run(cmd, capture_output=True, text=True)
    if proc.returncode == 1:
        if proc.stderr:
            print(proc.stderr, file=sys.stderr)
        out.write_text(proc.stdout)
        return True
    if proc.returncode != 0:
        if proc.stderr:
            print(proc.stderr, file=sys.stderr)
        return False

    out.write_text(proc.stdout)
    return True


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
        "--compile-flags",
        help="Path to compile_flags.txt with extra compiler args",
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
        compdb_files = load_compdb_files(compdb_dir)
    except (ValueError, json.JSONDecodeError) as exc:
        print(str(exc), file=sys.stderr)
        return 2
    compile_flags = None
    if args.compile_flags:
        compile_flags = Path(args.compile_flags).expanduser()
        if not compile_flags.is_file():
            print(f"compile flags file not found: {compile_flags}", file=sys.stderr)
            return 2
    notable = Path(args.notable_functions)
    ignored = Path(args.ignored_functions)
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    list_calls = repo_root / "scripts" / "list_calls.py"
    if not list_calls.exists():
        print(f"list_calls.py not found: {list_calls}", file=sys.stderr)
        return 2

    # Canonicalize paths so they match compile_commands.json entries.
    input_files = [Path(f).expanduser().resolve() for f in args.files]
    # Only analyze translation units present in the compilation database so we
    # don't fall back to empty compile commands.
    files = [str(path) for path in input_files if path in compdb_files]
    skipped = len(input_files) - len(files)
    if skipped:
        print(
            f"Skipping {skipped} file(s) not found in compile_commands.json.",
            file=sys.stderr,
        )
    if not files:
        print("No input files matched compile_commands.json.", file=sys.stderr)
        return 2

    all_funcs_db = output_dir / "all_funcs_report.db"
    all_funcs_txt = output_dir / "all_funcs.txt"
    all_report_db = output_dir / "all_report.db"
    all_ignored_txt = output_dir / "all_ignored.txt"
    notable_db = output_dir / "notable_report.db"
    notable_ignored_txt = output_dir / "notable_ignored.txt"
    report_db = output_dir / "report.db"
    ignored_txt = output_dir / "ignored.txt"

    extra_errorck_args: list[str] = []
    if compile_flags:
        extra_errorck_args = ["--compile-flags", str(compile_flags)]

    runs = [
        ErrorckRun(
            name="list-non-void-calls",
            cmd=[
                str(errorck),
                "--list-non-void-calls",
                "--db",
                str(all_funcs_db),
                "--overwrite-if-needed",
            ]
            + extra_errorck_args
            + [
                "-p",
                str(compdb_dir),
            ]
            + files,
            db=all_funcs_db,
            handling="observed_non_void",
            out=all_funcs_txt,
        ),
        ErrorckRun(
            name="all-non-void",
            cmd=[
                str(errorck),
                "--all-non-void",
                "--db",
                str(all_report_db),
                "--overwrite-if-needed",
            ]
            + extra_errorck_args
            + [
                "-p",
                str(compdb_dir),
            ]
            + files,
            db=all_report_db,
            handling="ignored",
            out=all_ignored_txt,
        ),
        ErrorckRun(
            name="notable-functions",
            cmd=[
                str(errorck),
                "--notable-functions",
                str(notable),
                "--db",
                str(notable_db),
                "--overwrite-if-needed",
            ]
            + extra_errorck_args
            + [
                "-p",
                str(compdb_dir),
            ]
            + files,
            db=notable_db,
            handling="ignored",
            out=notable_ignored_txt,
        ),
        ErrorckRun(
            name="exclude-notable-functions",
            cmd=[
                str(errorck),
                "--exclude-notable-functions",
                "--notable-functions",
                str(ignored),
                "--db",
                str(report_db),
                "--overwrite-if-needed",
            ]
            + extra_errorck_args
            + [
                "-p",
                str(compdb_dir),
            ]
            + files,
            db=report_db,
            handling="ignored",
            out=ignored_txt,
        ),
    ]

    # Run errorck passes in parallel so one problematic file does not stall
    # the rest of the analysis pipeline.
    results = run_errorck_jobs(runs)

    failed = False
    for run in runs:
        rc = results.get(run.name, 1)
        if rc not in (0, 1):
            failed = True
            continue
        if not write_list_calls(
            list_calls,
            run.db,
            run.handling,
            run.out,
            allow_missing_db=(rc == 1),
        ):
            failed = True

    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
