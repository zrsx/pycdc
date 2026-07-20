#!/usr/bin/env bash
set -euo pipefail

mkdir -p .ci/logs .ci/reports

if [[ -n "${CXX:-}" ]] && command -v "${CXX}" >/dev/null 2>&1; then
    selected_cxx="${CXX}"
elif command -v clang++ >/dev/null 2>&1; then
    selected_cxx="clang++"
elif command -v g++ >/dev/null 2>&1; then
    selected_cxx="g++"
elif command -v c++ >/dev/null 2>&1; then
    selected_cxx="c++"
else
    echo "No supported C++ compiler (Clang or GCC) is installed" >&2
    exit 1
fi

case "$(basename "${selected_cxx}")" in
    clang++*) selected_cc="${CC:-clang}" ;;
    g++*) selected_cc="${CC:-gcc}" ;;
    c++*) selected_cc="${CC:-cc}" ;;
    *) selected_cc="${CC:-cc}" ;;
esac

if ! command -v "${selected_cc}" >/dev/null 2>&1; then
    echo "Matching C compiler ${selected_cc} is not installed" >&2
    exit 1
fi

if [[ -f CMakeLists.txt ]] && command -v cmake >/dev/null 2>&1; then
    build_system="cmake"
elif [[ -f Makefile || -f makefile || -f GNUmakefile ]]; then
    build_system="make"
else
    echo "Neither a usable CMake project nor a Makefile was found" >&2
    exit 1
fi

if command -v nproc >/dev/null 2>&1; then
    jobs="$(nproc)"
else
    jobs="2"
fi
if (( jobs > 8 )); then
    jobs=8
fi

cat > .ci/environment <<EOF
CC=${selected_cc}
CXX=${selected_cxx}
BUILD_SYSTEM=${build_system}
JOBS=${jobs}
CCACHE_DIR=${CCACHE_DIR:-/var/cache/ccache}
EOF

echo "Build system: ${build_system}"
echo "C compiler: $(command -v "${selected_cc}")"
"${selected_cc}" --version | head -n 1
echo "C++ compiler: $(command -v "${selected_cxx}")"
"${selected_cxx}" --version | head -n 1
echo "Parallel jobs: ${jobs}"
if command -v ccache >/dev/null 2>&1; then
    ccache --version | head -n 1
    ccache --max-size "${CCACHE_MAXSIZE:-2G}"
fi
