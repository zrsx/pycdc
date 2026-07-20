#!/usr/bin/env bash
set -euo pipefail

: "${BUILDBOT_WORKER_VERSION:?BUILDBOT_WORKER_VERSION must be set}"
python3 -m venv /opt/buildbot-worker
/opt/buildbot-worker/bin/python -m pip install --no-cache-dir --upgrade pip setuptools wheel
/opt/buildbot-worker/bin/pip install --no-cache-dir \
    "buildbot-worker[docker]==${BUILDBOT_WORKER_VERSION}"
