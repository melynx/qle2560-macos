#!/usr/bin/env python3
"""On scratch media, time out a seek and verify recovery against a known tape file.

No media writes. The caller must provide a known logical object and expected file
hash/size. All command attempts and elapsed times are recorded. A failed seek is
never automatically retried; subsequent explicit positioning is part of the test.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import time

p = argparse.ArgumentParser(description=__doc__)
p.add_argument("--scratch", action="store_true", required=True)
p.add_argument("--tool", type=Path, default=Path("build/qle-tape"))
p.add_argument("--object", type=int, required=True)
p.add_argument("--sha256", required=True)
p.add_argument("--bytes", type=int, required=True)
p.add_argument("--evidence", type=Path, required=True)
p.add_argument(
    "--timeout-command",
    choices=["seek", "eod"],
    default="seek",
    help="Use eod from rewind when the drive satisfies backward seeks from its buffer",
)
a = p.parse_args()
if a.object < 0 or a.bytes < 0 or len(a.sha256) != 64:
    p.error("Provide a valid nonnegative object/size and SHA-256")
a.evidence.mkdir(parents=True, exist_ok=True)
tool = str(a.tool.resolve())
events = []


def save():
    (a.evidence / "events.json").write_text(json.dumps(events, indent=2) + "\n")


def run(args, timeout=1900, binary=False):
    start = time.monotonic()
    try:
        r = subprocess.run([tool, *args], capture_output=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        events.append({"args": args, "harness_timeout": timeout})
        save()
        raise
    event = {
        "args": args,
        "exit": r.returncode,
        "seconds": time.monotonic() - start,
        "stderr": r.stderr.decode(errors="replace"),
    }
    if binary:
        event.update(bytes=len(r.stdout), sha256=hashlib.sha256(r.stdout).hexdigest())
    else:
        event["stdout"] = r.stdout.decode(errors="replace")
    events.append(event)
    save()
    print(event, flush=True)
    return r


run(["eod" if a.timeout_command == "seek" else "rewind"]).check_returncode()
run(["position"]).check_returncode()
forced = ["seek", str(a.object)] if a.timeout_command == "seek" else ["eod"]
r = run(["--timeout-ms", "100", *forced])
if not r.returncode:
    raise RuntimeError(
        "Command finished before its deadline; timeout path was not exercised"
    )
start = time.monotonic()
while True:
    r = run(["status"], timeout=70)
    if not r.returncode:
        break
    if time.monotonic() - start > 60:
        raise RuntimeError("Readiness did not return within 60 seconds")
    time.sleep(2)
run(["position"], timeout=190).check_returncode()
run(["seek", str(a.object)]).check_returncode()
r = run(["read"], binary=True)
r.check_returncode()
if len(r.stdout) != a.bytes or hashlib.sha256(r.stdout).hexdigest() != a.sha256:
    raise RuntimeError("Post-timeout file verification failed")
run(["rewind"]).check_returncode()
(a.evidence / "verified.txt").write_text(
    "Post-timeout file hash and size verified; final rewind completed.\n"
)
