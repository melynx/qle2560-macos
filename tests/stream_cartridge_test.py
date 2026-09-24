import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / "scripts/stream-cartridge-test.py"
FAKE = r"""#!/usr/bin/env python3
import sys, pathlib

p = pathlib.Path(__file__).parent
a = sys.argv[1:]
if a[0] == "--block-size":
    a = a[2:]
c = a[0]
if c == "inquiry":
    print("HP fake tape")
elif c == "position":
    print("partition=0 object=0 file=0")
elif c == "write":
    if (p / "cap").exists():
        b = sys.stdin.buffer.read()
        (p / "data").write_bytes(b)
        print("Write complete " + str(len(b)) + " bytes", file=sys.stderr)
    else:
        b = sys.stdin.buffer.read(1048576 + 123)
        (p / "data").write_bytes(b)
        print("Write stopped 1048576 bytes", file=sys.stderr)
        if (p / "stop").exists():
            (p / "result/STOP").touch()
        sys.exit(1 if (p / "ambiguous").exists() else 4)
elif c == "read":
    b = (p / "data").read_bytes()
    if (p / "corrupt").exists():
        b = b"X" + b[1:]
    if (p / "missing").exists():
        b = b[:1024]
    sys.stdout.buffer.write(b)
"""


class Streaming(unittest.TestCase):
    def case(self, flag=None, record_mib=1):
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp)
            tool = p / "fake"
            tool.write_text(FAKE)
            tool.chmod(0o755)
            if flag:
                (p / flag).touch()
            r = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    "--run",
                    "--scratch",
                    "--record-mib",
                    str(record_mib),
                    "--max-mib",
                    str(max(4, record_mib * 2)),
                    "--tool",
                    str(tool),
                    "--evidence",
                    str(p / "result"),
                ],
                capture_output=True,
                timeout=30,
            )
            return r, json.loads((p / "result/report.json").read_text())

    def test_eom_partial(self):
        r, p = self.case()
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertTrue(p["complete"])
        self.assertEqual(p["verified_bytes"], 1048576 + 123)

    def test_cap(self):
        r, p = self.case("cap")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertFalse(p["complete"])
        self.assertEqual(p["verified_bytes"], 4 * 1048576)

    def test_eight_mib_records(self):
        r, p = self.case("cap", 8)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(p["block_bytes"], 8 * 1048576)
        self.assertEqual(p["verified_bytes"], 16 * 1048576)

    def test_eight_mib_partial(self):
        r, p = self.case(None, 8)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertTrue(p["complete"])
        self.assertEqual(p["verified_bytes"], 1048576 + 123)

    def test_corrupt(self):
        r, p = self.case("corrupt")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("Hash mismatch", p["error"])

    def test_missing(self):
        r, p = self.case("missing")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("acknowledged bytes missing", p["error"])

    def test_ambiguous(self):
        r, p = self.case("ambiguous")
        self.assertNotEqual(r.returncode, 0)
        self.assertFalse(p["eom"])

    def test_stop(self):
        r, p = self.case("stop")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(p["phase"], "stopped")
        self.assertNotIn("verified_bytes", p)


if __name__ == "__main__":
    unittest.main()
