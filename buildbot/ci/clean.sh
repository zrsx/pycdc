#!/usr/bin/env bash
set -euo pipefail

rm -rf -- .ci/build .ci/scan-build .ci/reports .ci/logs .ci/artifacts
