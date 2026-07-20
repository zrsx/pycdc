#!/usr/bin/env bash
set -euo pipefail

staging=".ci/artifacts"
rm -rf -- "${staging}"
mkdir -p "${staging}/bin" "${staging}/logs" "${staging}/reports"

for binary in pycdc pycdas; do
    if [[ -x ".ci/build/${binary}" ]]; then
        cp -p ".ci/build/${binary}" "${staging}/bin/"
    fi
done

if [[ -d .ci/logs ]]; then
    cp -a .ci/logs/. "${staging}/logs/"
fi
if [[ -d .ci/reports ]]; then
    cp -a .ci/reports/. "${staging}/reports/"
fi

if command -v sha256sum >/dev/null 2>&1; then
    find "${staging}" -type f ! -name SHA256SUMS -print0 | sort -z | xargs -0 -r sha256sum \
        > "${staging}/SHA256SUMS"
fi

tar -C "${staging}" -czf "${PWD}/.ci/pycdc-ci-artifacts.tar.gz" .
