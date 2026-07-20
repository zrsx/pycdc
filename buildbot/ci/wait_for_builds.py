#!/usr/bin/env python3
"""Wait for the Buildbot builds triggered over localhost to finish.

Polls the master REST API until every expected builder has a completed build,
then exits non-zero if any of those builds did not succeed. Used by the
Buildbot-CI GitHub workflow to turn a real Buildbot run into a pass/fail gate.
"""

import json
import os
import sys
import time
import urllib.error
import urllib.request


# Buildbot result codes (buildbot/process/results.py).
SUCCESS = 0
WARNINGS = 1
RESULT_NAMES = {
    0: "success",
    1: "warnings",
    2: "failure",
    3: "skipped",
    4: "exception",
    5: "retry",
    6: "cancelled",
}


def _get(base, path):
    url = f"{base.rstrip('/')}{path}"
    with urllib.request.urlopen(url, timeout=30) as response:
        return json.load(response)


def _builder_name(worker):
    return f"linux-{worker}"


def main():
    base = os.environ.get("BUILDBOT_WEB_URL", "http://localhost:8010")
    workers = [
        name.strip()
        for name in os.environ.get("EXPECTED_WORKERS", "").split(",")
        if name.strip()
    ]
    if not workers:
        print("EXPECTED_WORKERS is empty", file=sys.stderr)
        return 2
    expected = {_builder_name(worker) for worker in workers}
    timeout = int(os.environ.get("WAIT_TIMEOUT", "2400"))
    deadline = time.monotonic() + timeout

    # Map the expected builder names to their numeric ids.
    builder_ids = {}
    while time.monotonic() < deadline:
        try:
            builders = _get(base, "/api/v2/builders")["builders"]
        except (urllib.error.URLError, KeyError, TimeoutError) as exc:
            print(f"Waiting for master API: {exc}")
            time.sleep(3)
            continue
        builder_ids = {
            b["name"]: b["builderid"]
            for b in builders
            if b["name"] in expected
        }
        if set(builder_ids) == expected:
            break
        time.sleep(3)
    missing = expected - set(builder_ids)
    if missing:
        print(f"Builders never appeared: {', '.join(sorted(missing))}", file=sys.stderr)
        return 1

    results = {}
    while time.monotonic() < deadline:
        pending = []
        for name, builder_id in builder_ids.items():
            if name in results:
                continue
            builds = _get(
                base,
                f"/api/v2/builders/{builder_id}/builds"
                "?order=-number&limit=1&field=number&field=complete&field=results",
            )["builds"]
            if builds and builds[0]["complete"]:
                results[name] = builds[0]["results"]
                label = RESULT_NAMES.get(results[name], str(results[name]))
                print(f"{name}: build #{builds[0]['number']} finished ({label})")
            else:
                pending.append(name)
        if not pending:
            break
        print(f"Still running: {', '.join(sorted(pending))}")
        time.sleep(10)

    unfinished = expected - set(results)
    if unfinished:
        print(f"Builds did not finish in time: {', '.join(sorted(unfinished))}", file=sys.stderr)
        return 1

    failed = {
        name: result
        for name, result in results.items()
        if result not in (SUCCESS, WARNINGS)
    }
    if failed:
        for name, result in sorted(failed.items()):
            label = RESULT_NAMES.get(result, str(result))
            print(f"FAILED {name}: {label}", file=sys.stderr)
        return 1

    print("All expected builders succeeded")
    return 0


if __name__ == "__main__":
    sys.exit(main())
