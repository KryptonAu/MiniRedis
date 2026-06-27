#!/usr/bin/env python3
"""PostToolUse hook: auto-format .cpp/.h/.hpp files with clang-format (Google style).

Reads hook event from stdin JSON. Only runs on Edit/Write of C++ source files.
"""
import json
import subprocess
import sys
import os


def main() -> int:
    try:
        event = json.load(sys.stdin)
    except json.JSONDecodeError:
        return 0  # Not valid JSON, silently pass

    tool_name = event.get("tool_name", "")
    if tool_name not in ("Edit", "Write"):
        return 0

    file_path = event.get("tool_input", {}).get("file_path", "")
    if not file_path:
        return 0

    # Only format C++ source/header files
    if not file_path.endswith((".cpp", ".h", ".hpp", ".cc", ".cxx")):
        return 0

    # Check if file still exists (it might have been deleted)
    if not os.path.isfile(file_path):
        return 0

    # Run clang-format in-place with Google style
    result = subprocess.run(
        ["clang-format", "-i", "-style=Google", file_path],
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        print(f"clang-format failed on {file_path}: {result.stderr}", file=sys.stderr)
        return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
