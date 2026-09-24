#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"
mkdir -p evidence build
python3 scripts/generate-firmware.py
clang++ -std=c++17 -Wall -Wextra -Werror tools/qle-tape.cpp \
    -framework IOKit -framework CoreFoundation -o build/qle-tape
# Build only. Does not sign, install, or activate the extension.
if ! xcodebuild -project QLE2560Driver.xcodeproj -scheme QLE2560DriverApp \
    -configuration Debug -derivedDataPath "$root/build/DerivedData" \
    CODE_SIGNING_ALLOWED=NO CLANG_MODULE_CACHE_PATH="$root/build/ModuleCache" \
    build > evidence/build.log 2>&1; then
    tail -70 evidence/build.log >&2
    exit 1
fi
python3 tests/check_bundle.py
echo "Build succeeded: $root/build/DerivedData/Build/Products/Debug/QLE2560 Driver.app"
