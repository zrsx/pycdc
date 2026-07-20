#!/usr/bin/env bash
set -euo pipefail

: "${BUILDMASTER:?DockerLatentWorker did not provide BUILDMASTER}"
: "${BUILDMASTER_PORT:?DockerLatentWorker did not provide BUILDMASTER_PORT}"
: "${WORKERNAME:?DockerLatentWorker did not provide WORKERNAME}"
: "${WORKERPASS:?DockerLatentWorker did not provide WORKERPASS}"

worker_root=/var/lib/buildbot-worker
mkdir -p "${worker_root}" /var/cache/ccache
cd "${worker_root}"

buildbot-worker create-worker --force \
    --keepalive="${WORKER_KEEPALIVE:-60}" \
    --umask=0022 \
    . "${BUILDMASTER}:${BUILDMASTER_PORT}" "${WORKERNAME}" "${WORKERPASS}"

exec buildbot-worker start --nodaemon .
