#!/usr/bin/env bash
# Runs one RakNetTests case N times and prints how many runs passed and failed.
#
#   tools/repeat-test.sh "<Catch2 test name or filter>" [N] [--wsl]
#
# N defaults to 20. Builds RakNetTests first: Windows Release in build/ (override with
# RN_WINDOWS_BUILD), or with --wsl, GCC in WSL Ubuntu's ~/rn-build (override with
# RN_LINUX_BUILD, set on the Linux side). Run from Git Bash on Windows, or from bash on
# Linux without --wsl.
#
# Exits 2 when the build fails or the filter matches no test, so a typo never reads as
# a red or green run. Otherwise exits 0, red or green: the summary is the result. The
# output of the first failing run is kept, and its path printed.

set -u

name=""
runs=20
via_wsl=0
for arg in "$@"; do
    case "$arg" in
        --wsl) via_wsl=1 ;;
        --linux) ;; # Kept for the re-invocation below; Linux is detected from uname.
        ''|*[!0-9]*) name="$arg" ;;
        *) runs="$arg" ;;
    esac
done
if [ -z "$name" ]; then
    sed -n '2,13p' "$0" | sed 's/^# \{0,1\}//'
    exit 2
fi

root=$(cd "$(dirname "$0")/.." && pwd)

if [ "$via_wsl" = 1 ]; then
    # Git Bash rewrites /mnt/... arguments into C:/Program Files/Git/mnt/...;
    # MSYS_NO_PATHCONV stops it. --exec skips WSL's shell, which would split the name.
    windows_root=$(cd "$root" && pwd -W)
    drive=$(printf '%s' "${windows_root:0:1}" | tr '[:upper:]' '[:lower:]')
    MSYS_NO_PATHCONV=1 exec wsl.exe -d Ubuntu --exec bash "/mnt/$drive${windows_root:2}/tools/repeat-test.sh" --linux "$name" "$runs"
fi

build_log=$(mktemp)
case "$(uname -s)" in
    Linux)
        build="${RN_LINUX_BUILD:-$HOME/rn-build}"
        platform="Linux ($build)"
        make -C "$build" -j"$(nproc)" RakNetTests >"$build_log" 2>&1
        status=$?
        exe=$(find "$build" -name RakNetTests -type f -perm -u+x | head -1)
        ;;
    *)
        platform="Windows Release"
        build="${RN_WINDOWS_BUILD:-$root/build}"
        cmake --build "$build" --config Release --target RakNetTests >"$build_log" 2>&1
        status=$?
        exe="$build/Tests/Release/RakNetTests.exe"
        ;;
esac
if [ "$status" != 0 ] || [ ! -x "$exe" ]; then
    grep -iE "error" "$build_log" | head -20
    echo "repeat-test: build failed; full log in $build_log"
    exit 2
fi
rm -f "$build_log"

passed=0
failed=0
first_failure=""
for _ in $(seq 1 "$runs"); do
    out=$("$exe" "$name" </dev/null 2>&1)
    status=$?
    if printf '%s\n' "$out" | grep -q "No test cases matched"; then
        printf '%s\n' "$out" | grep "No test cases matched"
        echo "repeat-test: the filter matched no test (escape commas as \\, and use * only at an end)"
        exit 2
    fi
    if [ "$status" = 0 ]; then
        passed=$((passed + 1))
    else
        failed=$((failed + 1))
        if [ -z "$first_failure" ]; then
            first_failure=$(mktemp)
            printf '%s\n' "$out" >"$first_failure"
        fi
    fi
done

echo "$runs runs on $platform: $passed passed, $failed failed"
if [ -n "$first_failure" ]; then
    echo "First failing run's output: $first_failure"
fi
exit 0
