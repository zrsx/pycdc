#!/usr/bin/env bash
set -euo pipefail

mode="${1:?analysis mode is required}"
mkdir -p .ci/reports
source .ci/environment

mapfile -d '' sources < <(
    find . -maxdepth 2 -type f \
        \( -name '*.cpp' -o -name '*.h' -o -name '*.inl' \) \
        -not -path './.ci/*' -print0 | sort -z
)

case "${mode}" in
    format)
        clang-format --dry-run --Werror "${sources[@]}" \
            > .ci/reports/clang-format.txt 2>&1
        ;;
    cppcheck)
        cppcheck --project=.ci/build/compile_commands.json \
            --enable=warning,performance,portability \
            --inline-suppr --error-exitcode=1 --xml --xml-version=2 \
            2> .ci/reports/cppcheck.xml
        ;;
    clang-tidy)
        runner=""
        for candidate in run-clang-tidy run-clang-tidy.py; do
            if command -v "${candidate}" >/dev/null 2>&1; then
                runner="${candidate}"
                break
            fi
        done
        if [[ -z "${runner}" ]]; then
            echo "run-clang-tidy is not installed" >&2
            exit 1
        fi
        "${runner}" -p .ci/build -j "${JOBS}" -quiet \
            > .ci/reports/clang-tidy.txt 2>&1
        ;;
    scan-build)
        rm -rf -- .ci/scan-build
        mkdir -p .ci/scan-build
        scan-build --status-bugs -o .ci/scan-build \
            cmake --build .ci/build --clean-first --parallel "${JOBS}" \
            > .ci/reports/scan-build.txt 2>&1
        ;;
    *)
        echo "Unknown analysis mode: ${mode}" >&2
        exit 2
        ;;
esac
