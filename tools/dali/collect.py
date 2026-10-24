#!/usr/bin/env python3
"""Collect surviving dali UEFI ramoops evidence after returning to Android."""
import argparse
import json
from pathlib import Path
import re
import shlex
import subprocess


RESET_LOG_METHODS = {"psci-cold": "psci-system-reset", "psci-warm": "psci-system-reset2", "aee": "psci-aee-reset2"}


def parse_reset_qualification(text, xnu_match, expected_run_token, expected_method):
    """A saved, timed SMC request is evidence of a request, not a reset cause."""
    result = {"pass": False, "method": expected_method, "watchdog_disabled": False,
              "delay_verified": False, "smc_recorded": False}
    if not xnu_match or expected_method not in RESET_LOG_METHODS or not expected_run_token or \
            xnu_match.group(3) != expected_run_token or xnu_match.group(10) != "PASS":
        return result
    token = re.escape(expected_run_token)
    setup = re.search(rf"DALI: reset qualification run={token} marker=([0-9a-fA-F]+) mode=([0-9a-fA-F]+) watchdog=off", text)
    policy = re.search(rf"DALI: reset policy run={token} method={re.escape(expected_method)} delay=5 firmware-watchdogs=off status=Success", text)
    records = []
    for match in re.finditer(rf"(?m)^DALI-RESET: run={token} ([^\r\n]+)\r?$", text):
        fields = {}
        for word in match.group(1).split():
            if word.count("=") != 1:
                return result
            key, value = word.split("=", 1)
            if key in fields:
                return result
            fields[key] = value
        records.append((match.start(), fields))
    if not setup or not policy or setup.end() > xnu_match.start() or policy.end() > xnu_match.start():
        return result
    result["watchdog_disabled"] = int(setup.group(1), 16) & 15 == 3 and int(setup.group(2), 16) & 1 == 0
    try:
        expected_type = 0x80000201 if expected_method == "aee" else 0
        required = []
        for stage in ("wait", "issue", "smc"):
            found = [(position, fields) for position, fields in records if fields.get("stage") == stage]
            if len(found) != 1:
                return result
            position, fields = found[0]
            if fields.get("method") != RESET_LOG_METHODS[expected_method] or fields.get("qualification") != "1" or \
                    fields.get("delay_ms") != "5000" or fields.get("freq") != "13000000" or \
                    int(fields.get("type", "-1"), 16) != expected_type or \
                    int(fields.get("marker", "-1"), 16) & 15 != 3 or \
                    int(fields.get("mode", "-1"), 16) & 1 != 0:
                return result
            if stage != "wait" and int(fields.get("elapsed_ticks", "-1")) < 65000000:
                return result
            required.append(position)
        if any(fields.get("stage") not in ("wait", "issue", "smc") for _, fields in records):
            return result
        result["delay_verified"] = xnu_match.end() < required[0] < required[1] < required[2]
        result["smc_recorded"] = result["delay_verified"]
        result["pass"] = result["watchdog_disabled"] and result["delay_verified"]
    except ValueError:
        return result
    return result


def parse_evidence(data, *, expected_run_token=None, expected_watchdog_timeout=None, expected_reset_method=None):
    """Describe logged milestones; framebuffer readback is not visible output."""
    text = data.decode("utf-8", errors="replace")
    # Do not let an older successful boot supply milestones for a newer failure.
    start = text.rfind("DALI: SEC diagnostics v1")
    if start >= 0:
        text = text[start:]
    keys = [{"number": int(n), "scan": int(s, 16), "unicode": int(u, 16)}
            for n, s, u in re.findall(r"DALI: INPUT key=(\d+) scan=([\da-fA-F]+) unicode=([\da-fA-F]+)", text)]
    xnu_match = re.search(
        r"DALI-XNU: darwin=([\d.]+) build=([\d.]+) run=([0-9a-f]{32}) bootarg=(\d+) registered=(\d+) "
        r"platform=(\d+) gic=(\d+) timer=(\d+) mpidr=0x([\da-fA-F]+) checkpoint=(PASS|FAIL)", text)
    fastboot_match = re.search(
        r"DALI: auto-fastboot armed run=(none|[0-9a-f]{32}) marker=([\da-fA-F]+) timeout=(\d+) seconds", text)
    reset_match = re.search(
        r"(?m)^DALI-RESET: run=([0-9a-f]{32}) method=(?:rgu-swrst|psci-system-reset2?|psci-aee-reset2) stage=issue "
        r"marker=0x([\da-fA-F]+) mode=0x([\da-fA-F]+)\r?$", text)
    reset_returned = bool(re.search(r"DALI-RESET: run=[0-9a-f]{32} method=(?:rgu-swrst|psci-system-reset2?|psci-aee-reset2) stage=returned ", text))
    failures = [line for line in text.splitlines()
                if "ASSERT" in line or "UEFI_DIAGNOSTIC_FAIL" in line or
                "UEFI_STAGE_FAIL" in line or "Exception at" in line or
                "DALI-XNU:" in line and "checkpoint=FAIL" in line or
                ("DALI-RESET:" in line and any(stage in line for stage in ("stage=returned ", "stage=state-refused ", "stage=mode-refused ", "stage=features-refused ", "stage=delay-refused "))) or
                ("UNIXNU: driver " in line and ("load failed:" in line or "start failed:" in line))]
    xnu = {
        "checkpoint_present": xnu_match is not None,
        "darwin_version": xnu_match.group(1) if xnu_match else None,
        "build_version": xnu_match.group(2) if xnu_match else None,
        "run_token": xnu_match.group(3) if xnu_match else None,
        "run_token_matches_expected": expected_run_token is None or bool(
            xnu_match and xnu_match.group(3) == expected_run_token),
        "boot_argument_forwarded": bool(xnu_match and xnu_match.group(4) == "1"),
        "cpu_registered": bool(xnu_match and xnu_match.group(5) == "1"),
        "platform_configured": bool(xnu_match and xnu_match.group(6) == "1"),
        "gic_ready": bool(xnu_match and xnu_match.group(7) == "1"),
        "timer_interrupts": int(xnu_match.group(8)) if xnu_match else 0,
        "mpidr": int(xnu_match.group(9), 16) if xnu_match else None,
        "single_boot_cpu": bool(xnu_match and (int(xnu_match.group(9), 16) & 0xFF00FFFFFF) == 0),
        "checkpoint_pass": bool(xnu_match and xnu_match.group(10) == "PASS"),
    }
    xnu["pass"] = all((xnu["checkpoint_present"], xnu["boot_argument_forwarded"], xnu["cpu_registered"],
                       xnu["platform_configured"], xnu["gic_ready"], xnu["timer_interrupts"] > 0,
                       xnu["single_boot_cpu"], xnu["checkpoint_pass"], xnu["run_token_matches_expected"]))
    watchdog_seconds = int(fastboot_match.group(3)) if fastboot_match else None
    qualification = parse_reset_qualification(text, xnu_match, expected_run_token, expected_reset_method)
    qualification["pass"] = qualification["pass"] and xnu["pass"]
    return {
        "new_boot_marker": start >= 0,
        "diagnostic_pass": "DALI: UEFI_DIAGNOSTIC_PASS" in text,
        "stages": {stage: marker in text for stage, marker in {
            "shim": "DALI: UEFI relocation shim entered", "sec": "DALI: SEC diagnostics v1",
            "mmu": "DALI: SEC MMU and memory HOBs ready", "dxe": "DALI: diagnostic DXE loaded",
            "bds": "DALI: BDS entered"}.items()},
        "io": {
            "gop_published": "DALI: GOP status=Success" in text,
            "text_output_connected": "DALI: text output status=Success" in text,
            "gop_cpu_readback_pass": "DALI: GOP_BLT_PASS" in text,
            "text_input_connected": "DALI: text input status=Success" in text,
            "input_events": keys,
            "keypad_events": [{"scan": int(s, 16), "unicode": int(u, 16)} for s, u in
                              re.findall(r"DALI: keypad event scan=([\da-fA-F]+) unicode=([\da-fA-F]+)", text)],
            "pmic_key_events": {source: bool(re.search(
                rf"DALI: keypad event scan={scan} unicode={unicode} time-ms=\d+ source={source}(?:\r?\n|$)", text))
                for source, scan, unicode in [("volume-down", "2", "0"), ("power", "0", "D")]},
            "event_wait_pass": "DALI: WaitForEvent timer status=Success" in text,
            "inherited_watchdog_disabled": bool(re.search(r"DALI: inherited RGU WDT [^\r\n]* disabled=1", text)),
            "pmic_samples": [{"raw": int(raw, 16), "time_ms": int(ms)} for raw, ms in
                             re.findall(r"DALI: MT6363 TOPSTATUS=([\da-fA-F]+) time-ms=(\d+)", text)],
            "app_heartbeats": [{"number": int(n), "time_ms": int(ms)} for n, ms in
                               re.findall(r"DALI: app heartbeat=(\d+) time-ms=(\d+)", text)],
            "frame_submission_enabled": "DALI: frame submission enabled;" in text,
            "frame_submissions": [int(n) for n in re.findall(r"DALI: frame submit=(\d+)", text)],
            "frame_completion_samples": re.findall(r"DALI: frame completion sample[^\r\n]*", text),
            "display_visibility": "unverified",
        },
        "failures": failures,
        "recovery": {
            "auto_fastboot_armed": fastboot_match is not None,
            "run_token": fastboot_match.group(1) if fastboot_match else None,
            "run_token_matches_expected": expected_run_token is None or
                bool(fastboot_match and fastboot_match.group(1) == expected_run_token),
            "bootloader_marker": int(fastboot_match.group(2), 16) if fastboot_match else None,
            "device_watchdog_seconds": watchdog_seconds,
            "timeout_matches_expected": expected_watchdog_timeout is None or
                watchdog_seconds == expected_watchdog_timeout,
            "software_reset_requested": bool(expected_reset_method is None and reset_match and xnu["pass"] and fastboot_match and
                fastboot_match.start() < xnu_match.start() and
                reset_match.start() > xnu_match.end() and
                reset_match.group(1) == xnu_match.group(3) == fastboot_match.group(1) and
                (expected_run_token is None or reset_match.group(1) == expected_run_token) and
                int(reset_match.group(2), 16) & 0xF == 3 and
                int(reset_match.group(3), 16) & 9 == 1 and not reset_returned) or bool(xnu["pass"] and qualification["pass"]),
            "qualification": qualification,
            "software_reset_returned": reset_returned,
            "automatic_fastboot": "unverified; requires host enumeration and unattended observation",
        },
        "xnu": xnu,
        "unixnu": {
            "payload_filesystem_pass": "DALI: payload filesystem=PASS" in text,
            "firmware_ready": "DALI: UEFI_UNIXNU_READY" in text,
            "started": "UNIXNU: UniXNU " in text,
            "config_loaded": "UNIXNU: config: loaded" in text,
            "menu_shown": "UNIXNU: menu: shown" in text,
            "sample_driver_started": bool(re.search(r"UNIXNU: driver [^\r\n]*SampleDriver\.efi \(config\): started", text)),
            "hello_returned_success": bool(re.search(r"UNIXNU: boot: [^\r\n]*HelloPayload\.efi returned Success", text)),
            "exit_action": "UNIXNU: action: exit" in text,
            "returned_to_firmware": "DALI: UniXNU returned status=Aborted" in text,
        },
    }


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--serial", required=True)
    p.add_argument("--output", type=Path, required=True)
    p.add_argument("--require-unixnu", action="store_true")
    p.add_argument("--require-xnu", action="store_true")
    p.add_argument("--require-fastboot-arm", action="store_true")
    p.add_argument("--require-software-reset", action="store_true")
    p.add_argument("--expected-run-token")
    p.add_argument("--expected-watchdog-timeout", type=int)
    p.add_argument("--expected-reset-method", choices=tuple(RESET_LOG_METHODS))
    p.add_argument("--require-pmic-keys", action="store_true")
    args = p.parse_args()
    if args.require_xnu and not args.expected_run_token:
        p.error("--require-xnu requires a per-cycle --expected-run-token")
    if args.require_fastboot_arm and (args.expected_watchdog_timeout is None or not args.expected_run_token):
        p.error("--require-fastboot-arm requires --expected-watchdog-timeout and --expected-run-token")
    if args.require_software_reset and not args.expected_run_token:
        p.error("--require-software-reset requires --expected-run-token")
    if args.expected_run_token and not re.fullmatch(r"[0-9a-f]{32}", args.expected_run_token):
        p.error("--expected-run-token must be 32 lowercase hexadecimal characters")

    def read(command):
        return subprocess.check_output(["adb", "-s", args.serial, "exec-out",
                                        "su -c " + shlex.quote(command)], timeout=30)

    if read("getprop ro.product.device").strip() != b"dali":
        raise SystemExit("Expected dali")
    args.output.mkdir(parents=True, exist_ok=False, mode=0o700)
    result = {"slot": read("getprop ro.boot.slot_suffix").decode().strip(), "files": [], "diagnostic_pass": False}
    filenames = read("ls /sys/fs/pstore").decode().splitlines()
    combined = b""
    for name in filenames:
        if "/" in name or not name.startswith(("console-ramoops", "dmesg-ramoops", "pmsg-ramoops")):
            continue
        data = read("cat " + shlex.quote("/sys/fs/pstore/" + name))
        (args.output / name).write_bytes(data)
        result["files"].append(name)
        combined += data
    (args.output / "kernel-dmesg.txt").write_bytes(read("dmesg"))
    (args.output / "bootconfig.txt").write_bytes(read("cat /proc/bootconfig"))
    result["boot_reasons"] = {}
    for property_name in ("ro.boot.bootreason", "sys.boot.reason", "sys.boot.reason.last"):
        result["boot_reasons"][property_name] = read("getprop " + property_name).decode(errors="replace").strip()
    (args.output / "boot-reasons.json").write_text(json.dumps(result["boot_reasons"], indent=2) + "\n")
    result.update(parse_evidence(combined, expected_run_token=args.expected_run_token,
                                 expected_watchdog_timeout=args.expected_watchdog_timeout,
                                 expected_reset_method=args.expected_reset_method))
    (args.output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))
    if not result["diagnostic_pass"]:
        raise SystemExit("No hardware PASS evidence collected; missing logs do not prove UEFI did not execute")
    if result["failures"]:
        raise SystemExit("Core PASS exists, but later failures require inspection")
    if args.require_unixnu and not all(result["unixnu"].values()):
        raise SystemExit("UniXNU UEFI stage is incomplete; inspect the separate milestones")
    if args.require_xnu and (not result["new_boot_marker"] or not result["xnu"]["pass"]):
        raise SystemExit("XNU early-boot checkpoint is incomplete; inspect boot args, Apple DT, GIC and timer evidence")
    if args.require_fastboot_arm and (not result["recovery"]["auto_fastboot_armed"] or
                                     result["recovery"]["bootloader_marker"] & 0xF != 3 or
                                     not result["recovery"]["timeout_matches_expected"] or
                                     not result["recovery"]["run_token_matches_expected"]):
        raise SystemExit("UEFI did not arm the expected one-shot bootloader recovery marker")
    if args.require_pmic_keys and not all(result["io"]["pmic_key_events"].values()):
        raise SystemExit("Missing source-specific Volume down or Power key evidence")
    if args.require_software_reset and not result['recovery']['software_reset_requested']:
        raise SystemExit("No matching post-checkpoint RGU software-reset request; USB enumeration alone is insufficient")


if __name__ == "__main__":
    main()
