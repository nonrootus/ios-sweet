#!/usr/bin/env python3
"""Offline capture integrity and no-false-success cases; no device required."""
import importlib.util
from pathlib import Path
import struct
import unittest

spec = importlib.util.spec_from_file_location("summary", Path(__file__).with_name("gpu-log-summary.py"))
summary = importlib.util.module_from_spec(spec)
spec.loader.exec_module(summary)

# Synthetic console records in the current adapter's format, NOT a phone log.
# ACK 0x2047e uses the codec's actual type2/id126; echoed VERSION seq2/header
# 0x200201 matches sweet_hfi_v1_version. Outer ACK sequence is not checked.
CM3 = """Darwin Kernel Version fixture
SWEET GMU BOOT: begin first CM3 boot; no teardown or retry
SWEET GMU BOOT: done rc=0 phase=8 held=1 release=1 init=babeface host_irq=00000000 ao_irq=00000000 pdc_before=00000000 last=0/24c14
"""
HFI = """SWEET GMU HFI: begin GX and initial INIT/VERSION; no GPU submission
SWEET GMU HFI: done rc=0 phase=9 OOB=1/1 SPTP=1 HFI=1 GX=00000008 status=00000001 wire=10000000 fw=10080001
SWEET GMU HFI: INIT=1/1 VERSION=1/1 err=00000000 irq=00000000/00000000 last=14600/00000001 ack=0002047e/00200201 queues=7/7 7/7
SWEET GMU HFI: DEBUG=0 event=00000000/00000000 dbg_queue=0/0 observations=2
SWEET GMU HFI: observed 1s irq=00000000/00000000 GX=00000008 HFI=00000001 MSG=7/7 DBG=0/0
SWEET GMU HFI: observed 10s irq=00000000/00000000 GX=00000008 HFI=00000001 MSG=7/7 DBG=0/0
SWEET GMU HFI: retained until reboot; PERF/BW/TEST and acceleration not started
"""
GOOD = CM3 + HFI


def analyze(text):
    return summary.analyze(*summary.decode(text.encode()))


def binary(text, *, size=4096, total=None, seq=1, version=1):
    raw = text.encode()
    if total is None:
        total = len(raw)
    ring = bytearray(size)
    for i, value in enumerate(raw):
        ring[i % size] = value
    header = struct.pack("<8sIIQQ", b"SWEETLOG", version, size, total, seq)
    return header.ljust(4096, b"\0") + ring


class SummaryTest(unittest.TestCase):
    def test_complete_initial_report(self):
        result = analyze(GOOD)
        self.assertEqual(result["exit_code"], 0, result["issues"])
        self.assertEqual(result["capture"]["source_identity"], "not established")
        self.assertIn("no proof", result["scope"])
        self.assertEqual(analyze(GOOD.replace("0002047e", "abc2047e"))["exit_code"], 0)
        self.assertEqual(analyze(GOOD.replace("\n", "\r\n"))["exit_code"], 0)

    def test_cm3_does_not_imply_hfi(self):
        result = analyze(CM3)
        self.assertEqual(result["state"], "log_reports_cm3_only")
        self.assertEqual(result["exit_code"], 3)
        self.assertIsNone(result["progress"]["gx_power_clock_on_reported"])

    def test_early_gx_stop_does_not_claim_queue_or_version_failure(self):
        # Synthetic0150-shaped stop. Register values are not a full device log.
        text = CM3 + """SWEET GMU HFI: begin GX and initial INIT/VERSION; no GPU submission
SWEET GMU HFI: done rc=-3 phase=3 OOB=1/1 SPTP=0 HFI=0 GX=00000028 status=00000000 wire=00000000 fw=00000000
SWEET GMU HFI: INIT=0/0 VERSION=0/0 err=00000000 irq=00000000/00000000 last=14340/00000028 ack=00000000/00000000 queues=0/0 0/0
SWEET GMU HFI: DEBUG=0 event=00000000/00000000 dbg_queue=0/0 observations=0
SWEET GMU HFI: retained until reboot; PERF/BW/TEST and acceleration not started
"""
        result = analyze(text)
        self.assertEqual(result["exit_code"], 4)
        self.assertEqual(result["failure"]["stage"], "GX status")
        self.assertEqual(result["failure"]["meaning"], "guard or protocol rejection")
        self.assertEqual(result["failure"]["not_attempted"], ["SPTP power-on request", "HFI control", "INIT", "VERSION"])
        self.assertTrue(result["progress"]["gx_power_clock_on_reported"])
        self.assertFalse(result["progress"]["init_ack_reported"])
        self.assertFalse(result["progress"]["version_ack_reported"])
        self.assertNotIn("unsupported legacy wire/firmware version", result["issues"])
        self.assertNotIn("VERSION response/original header mismatch", result["issues"])

    def test_every_completion_line_required(self):
        lines = GOOD.splitlines(keepends=True)
        for i in range(len(lines)):
            with self.subTest(line=i):
                self.assertNotEqual(analyze("".join(lines[:i] + lines[i + 1:]))["exit_code"], 0)

    def test_failure_and_duplicate_session(self):
        for rc in (-1, -2, -3, -4, -5, -6, -7):
            with self.subTest(rc=rc):
                self.assertEqual(analyze(GOOD.replace("done rc=0 phase=9", f"done rc={rc} phase=9"))["exit_code"], 4)
        self.assertEqual(analyze(GOOD + GOOD)["exit_code"], 3)
        self.assertEqual(analyze(GOOD + "SWEET GMU HFI: done rc=-[unreadable]\n")["exit_code"], 3)

    def test_contradictions(self):
        cases = [
            ("phase=9", "phase=8"), ("OOB=1/1", "OOB=1/0"),
            ("SPTP=1", "SPTP=0"), ("HFI=1 GX", "HFI=0 GX"),
            ("init=babeface", "init=0"), ("held=1", "held=0"),
            ("release=1", "release=0"), ("INIT=1/1", "INIT=1/0"),
            ("VERSION=1/1", "VERSION=1/0"), ("err=00000000", "err=00000001"),
            ("wire=10000000", "wire=20000000"), ("fw=10080001", "fw=10070001"),
            ("fw=10080001", "fw=20080001"), ("0002047e", "000204ff"),
            ("0002047e", "0001047e"), ("0002047e", "0002037e"),
            ("00200201", "00300201"), ("queues=7/7", "queues=5/7"),
            ("7/7 7/7", "7/7 6/7"), ("dbg_queue=0/0", "dbg_queue=0/4"),
            ("observations=2", "observations=1"), ("observed 10s", "observed 1s"),
            ("GX=00000008", "GX=000000c8"), ("status=00000001", "status=00000000"),
            ("HFI=00000001", "HFI=00000000"), ("MSG=7/7", "MSG=6/7"),
            ("DBG=0/0", "DBG=1024/1024"), ("irq=00000000/00000000", "irq=00000002/00000000"),
            ("irq=00000000/00000000", "irq=00008000/00000000"),
            ("irq=00000000/00000000", "irq=00000000/00000020"),
        ]
        for original, replacement in cases:
            with self.subTest(original=original, replacement=replacement):
                self.assertIn(original, GOOD)
                self.assertNotEqual(analyze(GOOD.replace(original, replacement))["exit_code"], 0)

    def test_out_of_order_or_invalid_encoding(self):
        lines = GOOD.splitlines(keepends=True)
        lines[-2], lines[-3] = lines[-3], lines[-2]
        self.assertEqual(analyze("".join(lines))["exit_code"], 3)
        self.assertEqual(summary.analyze(*summary.decode(b"\xff\n" + GOOD.encode()))["exit_code"], 3)

    def test_binary_capture_integrity(self):
        data = binary(GOOD)
        self.assertEqual(summary.analyze(*summary.decode(data))["exit_code"], 0)
        for bad in (data[:-1], data + b"x", data[:100], binary(GOOD, version=2),
                    binary(GOOD, size=4095), b"x" * (summary.MAX_INPUT + 1)):
            with self.subTest(length=len(bad)):
                with self.assertRaises(ValueError):
                    summary.decode(bad, True)
        self.assertEqual(summary.analyze(*summary.decode(binary(GOOD, seq=0)))["exit_code"], 3)
        # A wrapped capture must not silently become an intact boot report.
        text, meta = summary.decode(binary("x" * 4096 + GOOD))
        self.assertTrue(meta["wrapped"])
        self.assertEqual(summary.analyze(text, meta)["exit_code"], 3)


if __name__ == "__main__":
    unittest.main()
