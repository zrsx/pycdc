#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
parallelism=1
declare -a requested=()

# Usage: build-images.sh [-jN] [name ...]
# With no names, every image is built. Pass names (e.g. debian fedora rocky ubi)
# to build a subset -- the Linux-only GitHub workflow uses this to skip freebsd.
for arg in "$@"; do
    case "${arg}" in
        -j*) parallelism="${arg#-j}" ;;
        [0-9]*) parallelism="${arg}" ;;
        *) requested+=("${arg}") ;;
    esac
done

declare -A images=(
    [debian]=pycdc-worker-debian:stable
    [fedora]=pycdc-worker-fedora:latest
    [rocky]=pycdc-worker-rocky:9
    [ubi]=pycdc-worker-ubi:9
    [freebsd]=pycdc-worker-freebsd:15.1
)

if (( ${#requested[@]} == 0 )); then
    requested=("${!images[@]}")
fi

for name in "${requested[@]}"; do
    if [[ -z "${images[${name}]:-}" ]]; then
        echo "Unknown worker image: ${name}" >&2
        exit 2
    fi
done

build_one() {
    local name="$1"
    echo "Building ${images[${name}]}"
    docker build --pull --file "${root}/buildbot/docker/workers/${name}.Dockerfile" \
        --tag "${images[${name}]}" "${root}"
}

if (( parallelism > 1 )); then
    for name in "${requested[@]}"; do
        printf '%s\0%s\0' "${name}" "${images[${name}]}"
    done | xargs -0 -r -n2 -P"${parallelism}" bash -c '
        echo "Building $2"
        docker build --pull --file "'"${root}"'/buildbot/docker/workers/$1.Dockerfile" --tag "$2" "'"${root}"'"
    ' _
else
    for name in "${requested[@]}"; do
        build_one "${name}"
    done
fi
