#!/usr/bin/env python3
"""Notification hook: alert on permission prompts in MiniRedis project.

Reads hook event from stdin JSON. Prints a focused alert on permission requests.
"""
import json
import sys


def main() -> int:
    try:
        event = json.load(sys.stdin)
    except json.JSONDecodeError:
        return 0

    notification_type = event.get("notification_type", "")

    if notification_type == "permission_prompt":
        tool_name = event.get("context", {}).get("tool_name", "unknown")
        print(f"MiniRedis: permission requested for '{tool_name}'", file=sys.stderr)

    return 0


if __name__ == "__main__":
    sys.exit(main())
