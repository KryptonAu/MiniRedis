#!/usr/bin/env python3
"""PreToolUse hook: block direct edits to build/ and .codegraph/ directories.

Reads hook event from stdin JSON. Exits with error if Edit/Write targets a protected path.
"""
import json
import sys


# Paths that should never be edited by Claude Code
PROTECTED_PATTERNS = ("build/", "Build/", ".codegraph/", "_deps/")


def main() -> int:
    try:
        event = json.load(sys.stdin)
    except json.JSONDecodeError:
        return 0

    tool_name = event.get("tool_name", "")
    if tool_name not in ("Edit", "Write"):
        return 0

    file_path = event.get("tool_input", {}).get("file_path", "")
    if not file_path:
        return 0

    for pattern in PROTECTED_PATTERNS:
        if pattern in file_path:
            print(
                f"ERROR: Direct edits to '{pattern}' paths are blocked. "
                f"Attempted: {file_path}",
                file=sys.stderr,
            )
            return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
