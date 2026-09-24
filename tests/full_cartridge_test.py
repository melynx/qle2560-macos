#!/usr/bin/env python3
"""Exercise the qualification harness with a fake tape; never access hardware."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / "scripts/full-cartridge-test.py"
FAKE = r"""#!/usr/bin/env python3
import sys, json, pathlib

root = pathlib.Path(__file__).parent
state = root / "state.json"
s = json.loads(state.read_text()) if state.exists() else {"object": 0, "writes": 0}
a = sys.argv[1:]
if a[:1] == ["--block-size"]:
    a = a[2:]
c = a[0]
if c == "inquiry":
    print("HP mock sequential device")
elif c == "rewind":
    s["object"] = 0
elif c == "position":
    print("partition=0 object=" + str(s["object"]))
elif c == "seek":
    s["object"] = int(a[1])
elif c == "write":
    data = sys.stdin.buffer.read()
    index = s["writes"]
    s["writes"] += 1
    if index == 0:
        (root / "file0").write_bytes(data)
        s["object"] = 2
    else:
        (root / "file2").write_bytes(data[: len(data) // 2])
        state.write_text(json.dumps(s))
        print("SCSI EOM=1", file=sys.stderr)
        print("Write stopped 0 bytes in 1.00 s", file=sys.stderr)
        sys.exit(1 if (root / "ambiguous").exists() else 4)
elif c == "read":
    data = (root / ("file" + str(s["object"]))).read_bytes()
    if (root / "corrupt").exists():
        data = b"X" + data[1:]
    sys.stdout.buffer.write(data)
    s["object"] += 2
state.write_text(json.dumps(s))
"""


class Qualification(unittest.TestCase):
    def run_case(self, flag=None):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            tool = root / "fake"
            tool.write_text(FAKE)
            tool.chmod(0o755)
            if flag:
                (root / flag).touch()
            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    "--run",
                    "--scratch",
                    "--chunk-mib",
                    "1",
                    "--max-gib",
                    "1",
                    "--tool",
                    str(tool),
                    "--evidence",
                    str(root / "result"),
                ],
                capture_output=True,
            )
            report = json.loads((root / "result/report.json").read_text())
            return result, report

    def test_eom_and_prefix(self):
        r, p = self.run_case()
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertTrue(p["complete"])
        self.assertEqual(len(p["files"]), 2)
        self.assertEqual(p["files"][1]["verification"]["bytes"], 524288)
        self.assertTrue(p["files"][1]["verification"]["prefix_only"])

    def test_corruption_fails(self):
        r, p = self.run_case("corrupt")
        self.assertNotEqual(r.returncode, 0)
        self.assertFalse(p["complete"])
        self.assertIn("mismatch", p["error"])

    def test_ambiguous_failure_is_not_eom(self):
        r, p = self.run_case("ambiguous")
        self.assertNotEqual(r.returncode, 0)
        self.assertFalse(p["eom"])
        self.assertFalse(p["complete"])

    def test_plan_never_accesses_device(self):
        with tempfile.TemporaryDirectory() as tmp:
            dest = Path(tmp) / "unused"
            r = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    "--tool",
                    "/nonexistent",
                    "--evidence",
                    str(dest),
                ],
                capture_output=True,
            )
            self.assertEqual(r.returncode, 0)
            self.assertFalse(dest.exists())


if __name__ == "__main__":
    unittest.main()
