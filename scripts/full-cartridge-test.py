#!/usr/bin/env python3
"""Scratch cartridge qualification. Default is a plan; --run --scratch overwrites tape.

Writes bounded random files until an explicitly classified EOM, then verifies
all completed files and the terminal file's readable prefix. Stops at early EOM;
never retries a write or pushes through the warning toward physical tape end.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import selectors
import subprocess
import time


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--run", action="store_true")
    p.add_argument("--scratch", action="store_true")
    p.add_argument("--tool", type=Path, default=Path("build/qle-tape"))
    p.add_argument("--evidence", type=Path, required=True)
    p.add_argument("--chunk-mib", type=int, default=1024)
    p.add_argument("--max-gib", type=int, default=4096)
    a = p.parse_args()
    if not 1 <= a.chunk_mib <= 1024 or not 1 <= a.max_gib <= 8192:
        p.error("chunk-mib must be 1..1024; max-gib must be 1..8192")
    if not a.run:
        print(
            f"PLAN ONLY: overwrite scratch tape with {a.chunk_mib} MiB random files; "
            f"stop at EOM or {a.max_gib} GiB cap; verify by streaming readback. "
            "No device access. Add --run --scratch to execute."
        )
        return
    if not a.scratch:
        p.error("--run requires --scratch")
    # Never overwrite evidence from a previous or interrupted test.
    a.evidence.mkdir(parents=True, exist_ok=False)
    tool = str(a.tool.resolve())
    report = {
        "complete": False,
        "phase": "starting",
        "files": [],
        "eom": False,
        "block_bytes": 1048576,
        "chunk_bytes": a.chunk_mib * 1048576,
    }
    data = a.evidence / "terminal-source.bin"

    def save():
        temp = a.evidence / "report.tmp"
        temp.write_text(json.dumps(report, indent=2) + "\n")
        temp.replace(a.evidence / "report.json")

    def command(args, input_file=None):
        t = time.monotonic()
        r = subprocess.run(
            [tool, *args], stdin=input_file, capture_output=True, timeout=1900
        )
        event = {
            "args": args,
            "exit": r.returncode,
            "seconds": time.monotonic() - t,
            "stdout": r.stdout.decode(errors="replace"),
            "stderr": r.stderr.decode(errors="replace"),
        }
        with (a.evidence / "commands.jsonl").open("a") as log:
            log.write(json.dumps(event) + "\n")
        return r

    def checked(args):
        r = command(args)
        r.check_returncode()
        return r.stdout.decode()

    def position():
        s = checked(["position"])
        m = re.search(r"partition=(\d+) object=(\d+)", s)
        if not m:
            raise RuntimeError("Position unavailable")
        return tuple(map(int, m.groups()))

    def verify(entry, terminal=False):
        # Stream through a pipe with a hard byte cap and wall-clock deadline.
        # Never accumulate a cartridge in memory or on disk.
        digest = hashlib.sha256()
        received = 0
        start = time.monotonic()
        expected = data.open("rb") if terminal else None
        log_path = a.evidence / f'read-{entry["index"]:05d}.log'
        with log_path.open("wb") as log:
            proc = subprocess.Popen([tool, "read"], stdout=subprocess.PIPE, stderr=log)
            selector = selectors.DefaultSelector()
            selector.register(proc.stdout, selectors.EVENT_READ)
            try:
                while True:
                    if time.monotonic() - start > 1900:
                        raise TimeoutError("Read deadline exceeded")
                    if not selector.select(timeout=1):
                        continue
                    chunk = os.read(proc.stdout.fileno(), 1048576)
                    if not chunk:
                        break
                    received += len(chunk)
                    if received > entry["bytes"]:
                        raise RuntimeError("Read exceeded expected file length")
                    digest.update(chunk)
                    if expected and chunk != expected.read(len(chunk)):
                        raise RuntimeError("Terminal file prefix mismatch")
                code = proc.wait(timeout=10)
                if code:
                    raise RuntimeError(
                        f"Read failed with exit {code}; inspect {log_path}"
                    )
                if not terminal and (
                    received != entry["bytes"] or digest.hexdigest() != entry["sha256"]
                ):
                    raise RuntimeError("File hash/length mismatch")
                if terminal and received < entry["confirmed_before_error"]:
                    raise RuntimeError(
                        "Previously acknowledged bytes missing from terminal file"
                    )
                entry["verification"] = {
                    "bytes": received,
                    "sha256": digest.hexdigest(),
                    "seconds": time.monotonic() - start,
                    "prefix_only": terminal,
                }
            finally:
                selector.close()
                proc.stdout.close()
                if proc.poll() is None:
                    proc.terminate()
                    try:
                        proc.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        proc.kill()
                        proc.wait()
                if expected:
                    expected.close()

    try:
        save()
        checked(["inquiry"])
        checked(["rewind"])
        partition, obj = position()
        if partition != 0 or obj != 0:
            raise RuntimeError("Expected partition 0 at beginning of tape")
        total = 0
        while total < a.max_gib * 1024**3:
            n = min(report["chunk_bytes"], a.max_gib * 1024**3 - total)
            digest = hashlib.sha256()
            with data.open("wb") as out:
                for _ in range(n // 1048576):
                    chunk = os.urandom(1048576)
                    out.write(chunk)
                    digest.update(chunk)
                out.flush()
                os.fsync(out.fileno())
            part, obj = position()
            if part != partition:
                raise RuntimeError("Partition changed")
            entry = {
                "index": len(report["files"]),
                "object": obj,
                "bytes": n,
                "sha256": digest.hexdigest(),
                "write_complete": False,
            }
            report["files"].append(entry)
            report["phase"] = "writing"
            save()
            print(
                f'Writing file {entry["index"]}, {total/1024**3:.2f} GiB completed',
                flush=True,
            )
            with data.open("rb") as source:
                r = command(["--block-size", "1048576", "write"], source)
            entry["write_exit"] = r.returncode
            if r.returncode == 4:
                # The CLI excludes ambiguous/deferred failures from its EOM exit.
                m = re.search(rb"Write stopped (\d+) bytes", r.stderr)
                if not m:
                    raise RuntimeError("Missing terminal write accounting")
                entry["confirmed_before_error"] = int(m.group(1))
                entry["terminal"] = True
                report["eom"] = True
                save()
                break
            r.check_returncode()
            entry["write_complete"] = True
            total += n
            save()
        report["completed_bytes"] = total
        report["phase"] = "verifying"
        save()
        checked(["rewind"])
        for entry in report["files"]:
            if position() != (partition, entry["object"]):
                raise RuntimeError("Unexpected readback file boundary")
            print(f'Verifying file {entry["index"]}', flush=True)
            verify(entry, terminal=entry.get("terminal", False))
            save()
        checked(["rewind"])
        report["phase"] = "finished"
        report["complete"] = report["eom"]
        report["result"] = (
            "EOM detected; completed files and terminal prefix verified"
            if report["eom"]
            else "Capacity cap reached; written files verified but EOM untested"
        )
        save()
        print(report["result"], flush=True)
        if not report["eom"]:
            raise RuntimeError("EOM not reached within cap")
    except BaseException as error:
        report["complete"] = False
        report["error"] = str(error)
        save()
        raise


if __name__ == "__main__":
    main()
