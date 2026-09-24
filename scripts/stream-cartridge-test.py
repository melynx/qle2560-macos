#!/usr/bin/env python3
"""Continuous scratch-tape qualification. Default is a device-free preview.

One write session, configurable tape records (default 8 MiB), a bounded producer queue, and SHA-256
per record. Touch EVIDENCE/STOP to finish writing at EOF with a filemark.
No automatic retry of a failed media command. No periodic tape filemarks.
"""
import argparse
from collections import deque
import hashlib
import json
import os
from pathlib import Path
import queue
import re
import selectors
import subprocess
import threading
import time

MIB = 1024 * 1024


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--run", action="store_true")
    ap.add_argument("--scratch", action="store_true")
    ap.add_argument("--tool", type=Path, default=Path("build/qle-tape"))
    ap.add_argument("--evidence", type=Path, required=True)
    ap.add_argument("--max-mib", type=int, default=4096 * 1024)
    ap.add_argument("--record-mib", type=int, choices=[1, 2, 4, 8], default=8)
    a = ap.parse_args()
    block = a.record_mib * MIB
    if a.max_mib % a.record_mib:
        ap.error("max-mib must be a multiple of record-mib")
    if not 1 <= a.max_mib <= 8192 * 1024:
        ap.error("max-mib must be 1..8388608")
    if not a.run:
        print(
            "PLAN ONLY: continuous random write from BOT until EOM or cap, then streaming verification. "
            "No device access. Add --run --scratch; touch EVIDENCE/STOP to stop cleanly."
        )
        return
    if not a.scratch:
        ap.error("--run requires --scratch")
    a.evidence.mkdir(parents=True, exist_ok=False)
    tool = str(a.tool.resolve())
    stopfile = a.evidence / "STOP"
    report = {
        "phase": "starting",
        "complete": False,
        "eom": False,
        "block_bytes": block,
    }

    def save():
        p = a.evidence / "report.tmp"
        p.write_text(json.dumps(report, indent=2) + "\n")
        p.replace(a.evidence / "report.json")

    def checked(args):
        r = subprocess.run([tool, *args], capture_output=True, timeout=1900)
        with (a.evidence / "commands.jsonl").open("a") as log:
            log.write(
                json.dumps(
                    {
                        "args": args,
                        "exit": r.returncode,
                        "stdout": r.stdout.decode(errors="replace"),
                        "stderr": r.stderr.decode(errors="replace"),
                    }
                )
                + "\n"
            )
        r.check_returncode()
        return r.stdout.decode()

    def cleanup(proc):
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()

    try:
        save()
        checked(["inquiry"])
        if stopfile.exists():
            report["phase"] = "stopped"
            save()
            return
        checked(["rewind"])
        if not re.search(r"partition=0 object=0(?:\s|$)", checked(["position"])):
            raise RuntimeError("Expected partition 0 at BOT")
        pending = queue.Queue(maxsize=32 // a.record_mib)
        halt = threading.Event()
        tail = deque(maxlen=128 // a.record_mib)
        errors = []
        produced = [0]

        def produce():
            try:
                with (a.evidence / "record-sha256.bin").open("wb") as hashes:
                    for index in range(a.max_mib // a.record_mib):
                        if halt.is_set() or stopfile.exists():
                            break
                        chunk = os.urandom(block)
                        hashes.write(hashlib.sha256(chunk).digest())
                        tail.append((index, chunk))
                        produced[0] += 1
                        while not halt.is_set():
                            try:
                                pending.put(chunk, timeout=0.1)
                                break
                            except queue.Full:
                                pass
                    hashes.flush()
                    os.fsync(hashes.fileno())
            except BaseException as e:
                errors.append(str(e))

        producer = threading.Thread(target=produce, daemon=True)
        report["phase"] = "writing"
        save()
        fed = 0
        stopped = False
        start = time.monotonic()
        last = start
        progress = start
        with (a.evidence / "write.log").open("wb") as log:
            proc = subprocess.Popen(
                [tool, "--block-size", str(block), "write"],
                stdin=subprocess.PIPE,
                stderr=log,
                stdout=log,
            )
            os.set_blocking(proc.stdin.fileno(), False)
            sel = selectors.DefaultSelector()
            sel.register(proc.stdin, selectors.EVENT_WRITE)
            producer.start()
            try:
                chunk = None
                offset = 0
                while proc.poll() is None:
                    if stopfile.exists():
                        stopped = True
                        break
                    if time.monotonic() - last > 1900:
                        raise TimeoutError("Writer stopped accepting data")
                    if chunk is None:
                        try:
                            chunk = pending.get(timeout=0.1)
                            offset = 0
                        except queue.Empty:
                            if not producer.is_alive():
                                break
                            continue
                    if not sel.select(timeout=0.2):
                        continue
                    try:
                        n = os.write(proc.stdin.fileno(), memoryview(chunk)[offset:])
                    except BlockingIOError:
                        continue
                    except BrokenPipeError:
                        break
                    fed += n
                    offset += n
                    last = time.monotonic()
                    if offset == len(chunk):
                        chunk = None
                    if last - progress >= 10:
                        report["fed_bytes"] = fed
                        report["write_seconds"] = last - start
                        save()
                        print(
                            f"Writing: {fed/1024**3:.2f} GiB fed, {fed/MIB/(last-start):.1f} MiB/s",
                            flush=True,
                        )
                        progress = last
                halt.set()
                producer.join(timeout=15)
                if producer.is_alive():
                    raise RuntimeError("Producer did not stop")
                sel.close()
                proc.stdin.close()
                code = proc.wait(timeout=1900)
            finally:
                halt.set()
                producer.join(timeout=15)
                sel.close()
                if not proc.stdin.closed:
                    proc.stdin.close()
                cleanup(proc)
        report.update(
            fed_bytes=fed,
            generated_records=produced[0],
            write_exit=code,
            write_seconds=time.monotonic() - start,
        )
        # Persist bounded final source window for partial-record verification.
        with (a.evidence / "tail.bin").open("wb") as out:
            for _, chunk in tail:
                out.write(chunk)
        first_tail = tail[0][0] if tail else produced[0]
        report["tail_first_record"] = first_tail
        tail.clear()
        if errors:
            raise RuntimeError("Producer failure: " + str(errors))
        text = (a.evidence / "write.log").read_text()
        m = re.search(r"Write (?:stopped|complete) (\d+) bytes", text)
        if not m:
            raise RuntimeError("Missing writer accounting")
        acknowledged = int(m.group(1))
        report["acknowledged_bytes"] = acknowledged
        report["eom"] = code == 4
        save()
        if code not in (0, 4):
            raise RuntimeError(f"Write failed with exit {code}; no automatic retry")
        if stopped or stopfile.exists():
            report["phase"] = "stopped"
            report["result"] = "Stopped by user; readback not performed"
            save()
            return
        if code == 0 and acknowledged != fed:
            raise RuntimeError("Successful write byte count mismatch")
        report["phase"] = "verifying"
        save()
        checked(["rewind"])
        received = 0
        record = 0
        buf = bytearray()
        start = time.monotonic()
        last = start
        progress = start
        with (a.evidence / "read.log").open("wb") as log, (
            a.evidence / "record-sha256.bin"
        ).open("rb") as hashes:
            proc = subprocess.Popen(
                [tool, "--block-size", str(block), "read"],
                stdout=subprocess.PIPE,
                stderr=log,
            )
            sel = selectors.DefaultSelector()
            sel.register(proc.stdout, selectors.EVENT_READ)
            try:
                while True:
                    if stopfile.exists():
                        raise InterruptedError("Stopped during verification")
                    if time.monotonic() - last > 1900:
                        raise TimeoutError("Read stalled")
                    if not sel.select(timeout=0.2):
                        continue
                    chunk = os.read(proc.stdout.fileno(), block)
                    if not chunk:
                        break
                    last = time.monotonic()
                    received += len(chunk)
                    if received > fed:
                        raise RuntimeError("Read exceeds supplied data")
                    buf.extend(chunk)
                    while len(buf) >= block:
                        if hashlib.sha256(
                            memoryview(buf)[:block]
                        ).digest() != hashes.read(32):
                            raise RuntimeError(f"Hash mismatch at record {record}")
                        del buf[:block]
                        record += 1
                    if last - progress >= 10:
                        report["verified_bytes"] = record * block
                        save()
                        print(
                            f"Verifying: {received/1024**3:.2f} GiB, {received/MIB/(last-start):.1f} MiB/s",
                            flush=True,
                        )
                        progress = last
                if proc.wait(timeout=10):
                    raise RuntimeError("Read failed; inspect read.log")
                if buf:
                    if not first_tail <= record < produced[0]:
                        raise RuntimeError("Partial record unavailable in source tail")
                    with (a.evidence / "tail.bin").open("rb") as source:
                        source.seek((record - first_tail) * block)
                        if source.read(len(buf)) != buf:
                            raise RuntimeError("Partial terminal record mismatch")
                if received < acknowledged:
                    raise RuntimeError("Previously acknowledged bytes missing")
                if code == 0 and received != fed:
                    raise RuntimeError("Read length mismatch")
            finally:
                sel.close()
                proc.stdout.close()
                cleanup(proc)
        report.update(verified_bytes=received, read_seconds=time.monotonic() - start)
        checked(["rewind"])
        report.update(
            phase="finished",
            complete=report["eom"],
            result=(
                "EOM and readable data verified"
                if report["eom"]
                else "Cap reached; data verified, EOM untested"
            ),
        )
        save()
        print(report["result"], flush=True)
    except BaseException as e:
        report.update(
            complete=False,
            phase="stopped" if isinstance(e, InterruptedError) else "failed",
            error=str(e),
        )
        save()
        raise


if __name__ == "__main__":
    main()
