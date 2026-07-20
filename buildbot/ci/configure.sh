#!/usr/bin/env bash
set -euo pipefail

build_type="${1:-Release}"
case "${build_type}" in
    Debug|Release|RelWithDebInfo|MinSizeRel) ;;
    *) echo "Unsupported build type: ${build_type}" >&2; exit 2 ;;
esac

source .ci/environment
export CC CXX CCACHE_DIR

if [[ "${BUILD_SYSTEM}" == "cmake" ]]; then
    generator=()
    if command -v ninja >/dev/null 2>&1; then
        generator=(-G Ninja)
    fi
    cmake -S . -B .ci/build "${generator[@]}" \
        -DCMAKE_BUILD_TYPE="${build_type}" \
        -DCMAKE_C_COMPILER="${CC}" \
        -DCMAKE_CXX_COMPILER="${CXX}" \
        -DCMAKE_C_COMPILER_LAUNCHER=ccache \
        -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
else
    mkdir -p .ci/build
    printf '%s\n' "${build_type}" > .ci/build/build-type
fi
