#!/usr/bin/env python3

"""List calls from an errorck report by handling type.

Example:
  scripts/list_calls.py report.db ignored
"""

import argparse
import sqlite3
import sys

def main() -> int:
    parser = argparse.ArgumentParser(
        description="List calls from an errorck report by handling type."
    )
    parser.add_argument("report_db", help="Path to the errorck report database")
    parser.add_argument("handling_type", help="Handling type to filter on")
    args = parser.parse_args()

    conn = sqlite3.connect(args.report_db)
    cursor = conn.cursor()

    query = """
        SELECT name, filename, line, column
        FROM watched_calls
        WHERE handling_type = ?
        ORDER BY filename, line, column
    """

    for name, filename, line, column in cursor.execute(
        query, (args.handling_type,)
    ):
        print(f"{name}: {filename}:{line}:{column}")

    conn.close()
    return 0

if __name__ == "__main__":
    sys.exit(main())
