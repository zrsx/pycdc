#!/usr/bin/env bash
set -euo pipefail

master_dir=/var/lib/buildbot/master
mkdir -p "${master_dir}" /var/lib/buildbot/artifacts
if [[ ! -f "${master_dir}/buildbot.tac" ]]; then
    buildbot create-master "${master_dir}"
fi
ln -sfn /opt/buildbot-config/master.cfg "${master_dir}/master.cfg"
exec buildbot start --nodaemon "${master_dir}"
