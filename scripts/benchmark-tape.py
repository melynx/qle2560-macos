#!/usr/bin/env python3
"""Append a random test file to an explicitly authorized scratch tape and verify it.

Requires --scratch: writes at EOD, adds a filemark, and leaves the tape at EOD.
Stores hashes and command output, not a second full copy of the test data.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--scratch", action="store_true", required=True)
parser.add_argument("--gib", type=int, default=8)
parser.add_argument("--tool", type=Path, default=Path("build/qle-tape"))
parser.add_argument("--evidence", type=Path, required=True)
args = parser.parse_args()
if not 1 <= args.gib <= 64:
    parser.error("--gib must be 1..64")
args.evidence.mkdir(parents=True, exist_ok=True)
tool = str(args.tool.resolve())
report = {"bytes": args.gib * 1024**3, "block_bytes": 1024**2, "verified": False}
report_path = args.evidence / "benchmark.json"


def save():
    report_path.write_text(json.dumps(report, indent=2) + "\n")


def command(*command_args):
    result = subprocess.run(
        [tool, *command_args], capture_output=True, text=True, timeout=1800
    )
    with (args.evidence / "commands.log").open("a") as log:
        log.write(
            repr(command_args)
            + "\n"
            + result.stdout
            + result.stderr
            + f"exit={result.returncode}\n"
        )
    result.check_returncode()
    return result.stdout


try:
    command("eod")
    position = command("position")
    match = re.search(r"partition=(\d+) object=(\d+)", position)
    if not match:
        raise RuntimeError("Cannot establish starting position")
    report["partition"], report["start_object"] = map(int, match.groups())
    save()
    source = hashlib.sha256()
    start = time.monotonic()
    with (args.evidence / "write.log").open("w") as log:
        process = subprocess.Popen(
            [tool, "--block-size", "1048576", "write"],
            stdin=subprocess.PIPE,
            stdout=log,
            stderr=log,
        )
        try:
            for _ in range(args.gib * 1024):
                chunk = os.urandom(1024**2)
                source.update(chunk)
                process.stdin.write(chunk)
            process.stdin.close()
            if process.wait(timeout=1800):
                raise RuntimeError("Tape write failed; position may have advanced")
        except BaseException:
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=30)
            raise
    report["write_seconds"] = time.monotonic() - start
    report["source_sha256"] = source.hexdigest()
    save()
    print("Write complete; verifying", flush=True)
    command("seek", str(report["start_object"]))
    restored = hashlib.sha256()
    received = 0
    start = time.monotonic()
    with (args.evidence / "read.log").open("w") as log:
        process = subprocess.Popen([tool, "read"], stdout=subprocess.PIPE, stderr=log)
        try:
            while True:
                chunk = process.stdout.read(1024**2)
                if not chunk:
                    break
                restored.update(chunk)
                received += len(chunk)
            if process.wait(timeout=1800):
                raise RuntimeError("Tape read failed")
        except BaseException:
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=30)
            raise
    report["read_seconds"] = time.monotonic() - start
    report["received_bytes"] = received
    report["restored_sha256"] = restored.hexdigest()
    report["verified"] = (
        received == report["bytes"] and restored.digest() == source.digest()
    )
    report["final_position"] = command("position").strip()
    save()
    if not report["verified"]:
        raise RuntimeError("Tape hash/length mismatch")
    print(json.dumps(report, indent=2), flush=True)
except BaseException as error:
    report["error"] = str(error)
    save()
    raise
