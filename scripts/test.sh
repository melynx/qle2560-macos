#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"
python3 scripts/generate-firmware.py
mkdir -p build/tests
for name in mailbox firmware fcp requests tape_diagnostics shutdown; do
    clang++ -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined \
        "tests/${name}_test.cpp" -o "build/tests/${name}_test"
    "build/tests/${name}_test"
done
python3 tests/full_cartridge_test.py
python3 tests/stream_cartridge_test.py
clang++ -std=c++17 -Wall -Wextra -Werror tools/qle-tape.cpp \
    -framework IOKit -framework CoreFoundation -o build/qle-tape
build/qle-tape --help >/dev/null 2>&1
if build/qle-tape --block-size 0 write >/dev/null 2>&1; then
    echo 'Invalid block size was accepted' >&2; exit 1
fi

for args in 'seek' 'seek -1' 'seek 18446744073709551616' 'log-page 64' '--timeout-ms 0 status' '--timeout-ms 1800001 status'; do
    if build/qle-tape $args >build/tests/invalid-cli.log 2>&1; then
        echo "Invalid command accepted: $args" >&2; exit 1
    fi
    if ! rg -q 'Usage:' build/tests/invalid-cli.log; then
        echo "Invalid command reached device access: $args" >&2; exit 1
    fi
done
