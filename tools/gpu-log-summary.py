#!/usr/bin/env python3
"""Read-only summary of SWEET 0148-0151 CM3/initial-HFI console evidence.

Accepts a SWEETLOG v1 binary capture or decoded UTF-8 text. This does not
establish which image was booted, capture freshness, or GPU rendering.
Exit: 0 complete initial-HFI report, 2 invalid input, 3 incomplete/CM3-only,
4 an explicit nonzero completion. No device access or log text is executed.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct

MAX_INPUT = 8 * 1024 * 1024
MAX_RING = 4 * 1024 * 1024
H = r"[0-9a-fA-F]{1,8}"
N = r"[0-9]+"
BOOT_BEGIN = "SWEET GMU BOOT: begin first CM3 boot; no teardown or retry"
HFI_BEGIN = "SWEET GMU HFI: begin GX and initial INIT/VERSION; no GPU submission"
HFI_END = "SWEET GMU HFI: retained until reboot; PERF/BW/TEST and acceleration not started"
HFI_STAGES = ("empty", "preflight", "OOB request", "GX status", "SPTP power-on",
              "HFI control", "INIT", "VERSION", "health observation", "initial HFI ready")
HFI_ERRORS = {-1: "invalid or repeated call", -2: "I/O failure or all-ones read",
              -3: "guard or protocol rejection", -4: "deadline expired",
              -5: "error interrupt", -6: "queue or ACK error", -7: "firmware error"}
FIELDS = {
    "cm3": (rf"SWEET GMU BOOT: done rc=(-?{N}) phase=({N}) held=({N}) release=({N}) init=({H}) host_irq=({H}) ao_irq=({H}) pdc_before=({H}) last=({N})/({H})",
            "rc phase held release init:x host_irq:x ao_irq:x pdc_before:x last_region last_offset:x"),
    "hfi": (rf"SWEET GMU HFI: done rc=(-?{N}) phase=({N}) OOB=({N})/({N}) SPTP=({N}) HFI=({N}) GX=({H}) status=({H}) wire=({H}) fw=({H})",
            "rc phase oob_issued oob_ack sptp_issued hfi_issued gx:x status:x wire:x firmware:x"),
    "ack": (rf"SWEET GMU HFI: INIT=({N})/({N}) VERSION=({N})/({N}) err=({H}) irq=({H})/({H}) last=({H})/({H}) ack=({H})/({H}) queues=({N})/({N}) ({N})/({N})",
            "init_sent init_ack version_sent version_ack error:x host_irq:x ao_irq:x last_offset:x last_value:x response_header:x original_header:x cmd_read cmd_write msg_read msg_write"),
    "events": (rf"SWEET GMU HFI: DEBUG=({N}) event=({H})/({H}) dbg_queue=({N})/({N}) observations=({N})",
               "debug_messages event_header:x event_code:x dbg_read dbg_write observations"),
    "sample": (rf"SWEET GMU HFI: observed ({N})s irq=({H})/({H}) GX=({H}) HFI=({H}) MSG=({N})/({N}) DBG=({N})/({N})",
               "seconds host_irq:x ao_irq:x gx:x status:x msg_read msg_write dbg_read dbg_write"),
}


def decode(data, binary=False):
    if len(data) > MAX_INPUT:
        raise ValueError("capture exceeds 8 MiB limit")
    meta = {"input_sha256": hashlib.sha256(data).hexdigest(), "format": "text",
            "wrapped": None, "freshness": "not established", "source_identity": "not established"}
    if binary or data.startswith(b"SWEETLOG"):
        if len(data) < 4096:
            raise ValueError("truncated SWEETLOG header")
        magic, version, size, total, seq = struct.unpack_from("<8sIIQQ", data)
        if magic != b"SWEETLOG" or version != 1:
            raise ValueError("unsupported SWEETLOG magic/version")
        if not size or size % 4096 or size > MAX_RING or len(data) != 4096 + size:
            raise ValueError("invalid ring size or incomplete/overlong binary capture")
        meta.update(format="SWEETLOG", version=version, ring_size=size, total=total,
                    flush_sequence=seq, wrapped=total > size)
        ring = data[4096:]
        if total <= size:
            data = ring[:total]
        else:
            start = total % size
            data = ring[start:] + ring[:start]
    # Replacement bytes never turn a malformed field into a valid number.
    # Report them explicitly so a damaged capture cannot earn success.
    text = data.decode("utf-8", "replace").replace("\r", "")
    meta["decode_errors"] = "\ufffd" in text
    return text, meta


def records(text, name):
    pattern, fields = FIELDS[name]
    result = []
    for match in re.finditer("^" + pattern + "$", text, re.M):
        row = {}
        for field, value in zip(fields.split(), match.groups()):
            key, _, kind = field.partition(":")
            row[key] = int(value, 16 if kind == "x" else 10)
        row["line"] = text.count("\n", 0, match.start()) + 1
        result.append(row)
    return result


def analyze(text, meta):
    found = {name: records(text, name) for name in FIELDS}
    issues = []
    lines = text.splitlines()
    boot_starts = [i + 1 for i, line in enumerate(lines) if line == BOOT_BEGIN]
    hfi_starts = [i + 1 for i, line in enumerate(lines) if line == HFI_BEGIN]
    hfi_ends = [i + 1 for i, line in enumerate(lines) if line == HFI_END]
    kernels = sum(line.startswith("Darwin Kernel Version ") for line in lines)
    if meta.get("wrapped"):
        issues.append("ring wrapped: earlier boot evidence may be missing")
    if meta.get("decode_errors"):
        issues.append("capture contains invalid UTF-8 or replacement characters")
    if meta.get("format") == "SWEETLOG" and not meta.get("flush_sequence"):
        issues.append("binary capture has no completed flush sequence")
    if kernels != 1:
        issues.append("expected exactly one kernel-start record")
    if len(boot_starts) != 1:
        issues.append("expected exactly one CM3 begin record")
    for name in ("cm3", "hfi", "ack", "events"):
        if len(found[name]) > 1:
            issues.append("duplicate " + name + " records: session is ambiguous")
    for name, prefix in (("cm3", "SWEET GMU BOOT: done"), ("hfi", "SWEET GMU HFI: done"),
                         ("ack", "SWEET GMU HFI: INIT="), ("events", "SWEET GMU HFI: DEBUG="),
                         ("sample", "SWEET GMU HFI: observed")):
        if sum(line.startswith(prefix) for line in lines) != len(found[name]):
            issues.append("malformed " + name + " record")
    cm3 = found["cm3"][0] if len(found["cm3"]) == 1 else None
    hfi = found["hfi"][0] if len(found["hfi"]) == 1 else None
    ack = found["ack"][0] if len(found["ack"]) == 1 else None
    events = found["events"][0] if len(found["events"]) == 1 else None
    cm3_ready = bool(cm3 and cm3["rc"] == 0 and cm3["phase"] == 8 and
                     cm3["held"] == cm3["release"] == 1 and cm3["init"] == 0xbabeface and
                     not cm3["host_irq"] & 0x8000 and not cm3["ao_irq"] & 0x29)
    if not cm3_ready:
        issues.append("complete successful CM3 report missing or inconsistent")
    if not hfi_starts:
        issues.append("initial HFI not reported as started")
    elif len(hfi_starts) != 1:
        issues.append("multiple HFI attempts: session is ambiguous")
    if not hfi or not ack or not events or len(hfi_ends) != 1:
        issues.append("initial HFI completion/ACK/event/retention records incomplete")
    if cm3 and hfi and ack and events and len(boot_starts) == len(hfi_starts) == len(hfi_ends) == 1:
        order = [boot_starts[0], cm3["line"], hfi_starts[0], hfi["line"], ack["line"], events["line"]]
        order += [s["line"] for s in found["sample"]] + hfi_ends
        if order != sorted(set(order)):
            issues.append("startup/completion records out of order")
        if not (hfi["rc"] == 0 and hfi["phase"] == 9 and
                hfi["oob_issued"] == hfi["oob_ack"] == hfi["sptp_issued"] == hfi["hfi_issued"] == 1):
            issues.append("HFI stage/transition flags do not report success")
        if not ack["version_ack"]:
            issues.append("VERSION exchange not completed; compatibility is unestablished")
        elif (hfi["wire"] >> 28) >= 2 or (hfi["firmware"] >> 28) != 1 or ((hfi["firmware"] >> 16) & 0xfff) < 8:
            issues.append("unsupported legacy wire/firmware version")
        if not all(ack[k] == 1 for k in ("init_sent", "init_ack", "version_sent", "version_ack")):
            issues.append("INIT/VERSION pair not both sent and acknowledged")
        if ack["error"]:
            issues.append("nonzero firmware error reported")
        # The actual codec checks ACK type2/id126. The echoed original header
        # carries sequence2; the outer ACK sequence is not part of that check.
        if ack["version_ack"] and (ack["original_header"] != 0x00200201 or
                (ack["response_header"] & 0xf00ff) != 0x2007e or
                ((ack["response_header"] >> 8) & 255) < 4):
            issues.append("VERSION response/original header mismatch")
        if ack["cmd_read"] != 7 or ack["cmd_write"] != 7:
            issues.append("CMD queue does not show the two initial packets consumed")
        if events["observations"] != 2 or [s["seconds"] for s in found["sample"]] != [1, 10]:
            issues.append("both 1s and 10s health samples required")
        if (ack["host_irq"] & 0x8007) or (ack["ao_irq"] & 0x29):
            issues.append("pending service/error interrupt at completion")
        if not (0 <= ack["msg_read"] == ack["msg_write"] < 1024 and
                0 <= events["dbg_read"] == events["dbg_write"] < 1024):
            issues.append("completion queue indexes are not empty/in range")
        if not hfi["hfi_issued"]:
            issues.append("HFI activation not attempted")
        for row in ([hfi] if hfi["hfi_issued"] else []) + found["sample"]:
            if row["gx"] & 0xdc != 8 or not row["status"] & 1:
                issues.append("GX/SPTP/HFI status inconsistent")
                break
        for row in found["sample"]:
            if ((row["host_irq"] & 0x8007) or (row["ao_irq"] & 0x29) or
                    not 0 <= row["msg_read"] == row["msg_write"] < 1024 or
                    not 0 <= row["dbg_read"] == row["dbg_write"] < 1024):
                issues.append("health sample contains pending/error events or nonempty queues")
                break
    failed = any(row["rc"] != 0 for name in ("cm3", "hfi") for row in found[name])
    if failed:
        state, code = "log_reports_failure", 4
    elif not issues:
        state, code = "log_reports_initial_hfi_ready", 0
    elif cm3_ready and not hfi_starts and len(boot_starts) == kernels == 1 and len(found["cm3"]) == 1 and not meta.get("wrapped") and not meta.get("decode_errors") and not any("malformed" in issue or "flush" in issue for issue in issues):
        state, code = "log_reports_cm3_only", 3
    else:
        state, code = "incomplete_or_inconsistent", 3
    progress = {"cm3_ready_reported": cm3_ready if cm3 else None,
                "oob_ack_reported": bool(hfi["oob_ack"]) if hfi else None,
                "gx_power_clock_on_reported": (not bool(hfi["gx"] & 0xc0)) if hfi and hfi["phase"] >= 3 and hfi["oob_ack"] else None,
                "init_ack_reported": bool(ack["init_ack"]) if ack else None,
                "version_ack_reported": bool(ack["version_ack"]) if ack else None,
                "health_sample_seconds": [row["seconds"] for row in found["sample"]]}
    failure = None
    if hfi and hfi["rc"]:
        failure = {"component": "post-CM3", "code": hfi["rc"], "phase": hfi["phase"],
                   "stage": HFI_STAGES[hfi["phase"]] if hfi["phase"] < len(HFI_STAGES) else "unknown",
                   "meaning": HFI_ERRORS.get(hfi["rc"], "unknown"), "not_attempted": []}
        for label, record, field in (("SPTP power-on request", hfi, "sptp_issued"),
                                     ("HFI control", hfi, "hfi_issued"),
                                     ("INIT", ack, "init_sent"), ("VERSION", ack, "version_sent")):
            if record and not record[field]:
                failure["not_attempted"].append(label)
        # Zero versions/ACKs at this point are unperformed work, not evidence
        # of a rejected version or an actual malformed firmware response.
    elif cm3 and cm3["rc"]:
        failure = {"component": "CM3", "code": cm3["rc"], "phase": cm3["phase"]}
    return {"profile": "sweet_k6a-0148-0151", "state": state, "exit_code": code,
            "progress": progress, "failure": failure,
            "capture": meta, "issues": issues, "records": found,
            "scope": "CM3 and initial INIT/VERSION only; no proof of PERF/BW/TEST, GPU submission or rendering"}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path)
    parser.add_argument("--json", action="store_true", help="emit structured summary to stdout")
    args = parser.parse_args()
    try:
        with args.capture.open("rb") as stream:
            data = stream.read(MAX_INPUT + 1)
        text, meta = decode(data, args.capture.suffix.lower() == ".bin")
        result = analyze(text, meta)
    except (OSError, ValueError) as exc:
        parser.exit(2, f"Invalid capture: {exc}\n")
    if args.json:
        print(json.dumps(result, indent=2))
    else:
        print(result["state"])
        if result["failure"]:
            print("Recorded stop: " + json.dumps(result["failure"]))
        if result["issues"]:
            print("Requirements for initial HFI readiness not satisfied:")
        for issue in result["issues"]:
            print("- " + issue)
        print(result["scope"])
        print("Capture freshness and kernel source identity require separate boot/image evidence.")
    return result["exit_code"]


if __name__ == "__main__":
    raise SystemExit(main())
