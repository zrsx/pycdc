#!/usr/bin/env bash
set -euo pipefail

source .ci/environment
export CC CXX CCACHE_DIR

if [[ "${BUILD_SYSTEM}" == "cmake" ]]; then
    cmake --build .ci/build --parallel "${JOBS}"
else
    case "$(cat .ci/build/build-type)" in
        Debug) flags="-O0 -g3" ;;
        Release) flags="-O3 -DNDEBUG" ;;
        RelWithDebInfo) flags="-O2 -g -DNDEBUG" ;;
        MinSizeRel) flags="-Os -DNDEBUG" ;;
    esac
    make -j"${JOBS}" CC="ccache ${CC}" CXX="ccache ${CXX}" \
        CFLAGS="${flags}" CXXFLAGS="${flags}"
    cp -f pycdc pycdas .ci/build/
fi

test -x .ci/build/pycdc
test -x .ci/build/pycdas
ccache --show-stats || true
