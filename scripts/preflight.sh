#!/bin/sh
# Read-only environment report. Does not activate extensions or touch registers.
set -u

if [ "$(uname -s)" != Darwin ]; then
    echo 'This preflight requires macOS.' >&2
    exit 1
fi

echo 'Host'
sw_vers
uname -m
echo '\nPCI devices'
system_profiler SPPCIDataType
echo '\nDeveloper tools'
missing=0
if xcode-select -p; then
    if ! xcrun --sdk driverkit --show-sdk-path; then
        missing=1
    fi
else
    missing=1
fi
if [ "$missing" -ne 0 ]; then
    echo 'BLOCKED: select a full Xcode installation containing the DriverKit SDK.'
    exit 2
fi
echo 'DriverKit SDK located. Signing and hardware operation are not verified.'
