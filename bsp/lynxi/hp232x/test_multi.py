#!/usr/bin/env python3
"""Run HP232X board tests: SMP-msh, SMP-pmon, UP-msh."""

import paramiko
import re
import sys
import time

SERVER = "192.168.49.81"
USERNAME = "lynxi"
PASSWORD = "1"
SUDO_PASSWORD = "1"
RTCONFIG = "/work/rt-thread/bsp/lynxi/hp232x/rtconfig.h"
BSP_DIR = "/work/rt-thread/bsp/lynxi/hp232x"


def patch_rtconfig(mode):
    with open(RTCONFIG, "r") as f:
        lines = f.readlines()

    out = []
    for line in lines:
        s = line.strip()
        if s.startswith("#define RT_USING_SMP") or s.startswith("/* #define RT_USING_SMP"):
            if mode in ("smp_msh", "smp_pmon"):
                out.append("#define RT_USING_SMP\n")
            else:
                out.append("/* #define RT_USING_SMP */\n")
            continue
        if s.startswith("#define RT_CPUS_NR") or s.startswith("/* #define RT_CPUS_NR"):
            out.append("#define RT_CPUS_NR 1\n")
            continue
        if s.startswith("#define RT_USING_MSH") or s.startswith("/* #define RT_USING_MSH"):
            if mode.endswith("msh"):
                out.append("#define RT_USING_MSH\n")
            else:
                out.append("/* #define RT_USING_MSH */\n")
            continue
        if s.startswith("#define RT_USING_FINSH") or s.startswith("/* #define RT_USING_FINSH"):
            if mode.endswith("msh"):
                out.append("#define RT_USING_FINSH\n")
            else:
                out.append("/* #define RT_USING_FINSH */\n")
            continue
        if s.startswith("#define RT_BSP_PMON_TEST") or s.startswith("/* #define RT_BSP_PMON_TEST"):
            if mode.endswith("pmon"):
                out.append("#define RT_BSP_PMON_TEST\n")
            else:
                out.append("/* #define RT_BSP_PMON_TEST */\n")
            continue
        out.append(line)

    with open(RTCONFIG, "w") as f:
        f.writelines(out)


def run_board_test(mode, send_help=False):
    import subprocess
    print(f"\n{'='*60}\n=== BUILD {mode} ===\n{'='*60}")
    patch_rtconfig(mode)
    r = subprocess.run(["scons", "-j8"], cwd=BSP_DIR, capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stdout[-2000:])
        print(r.stderr[-2000:])
        return {"mode": mode, "ok": False, "error": "build failed"}

    ssh = paramiko.SSHClient()
    ssh.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    ssh.connect(SERVER, username=USERNAME, password=PASSWORD, timeout=10)

    transport = ssh.get_transport()
    session = transport.open_session()
    session.get_pty()
    session.exec_command(
        f"echo '{SUDO_PASSWORD}' | sudo -S timeout 45 cat /dev/ttyUSB1"
    )
    time.sleep(1.5)

    ssh.exec_command(f"echo '{SUDO_PASSWORD}' | sudo -S lynd_hp run -d 0 -r wdt -o5")
    time.sleep(2)

    out = ""
    deadline = time.time() + 45
    help_sent = False
    while time.time() < deadline:
        if session.recv_ready():
            chunk = session.recv(8192).decode(errors="ignore")
            out += chunk
            print(chunk, end="", flush=True)
            if send_help and "msh" in out and not help_sent:
                help_sent = True
                time.sleep(1.0)
                w = transport.open_session()
                w.exec_command(
                    f"echo '{SUDO_PASSWORD}' | sudo -S sh -c "
                    f"'printf \"help\\r\" > /dev/ttyUSB1'"
                )
                time.sleep(0.5)
                w.close()
        else:
            time.sleep(0.05)

    session.close()
    ssh.close()

    result = {
        "mode": mode,
        "ok": True,
        "msh": "msh" in out,
        "hi": "Hi, this is RT-Thread" in out,
        "pmon": "[GIC Monitor]" in out or "[GIC]" in out,
        "help": "help" in out.lower() and ("RT-Thread shell commands" in out or "command" in out.lower()),
        "version": bool(re.search(r"5\.3\.0 build", out)),
    }
    return result


def main():
    backup = open(RTCONFIG).read()
    results = []
    try:
        for mode, send_help in [("smp_msh", True), ("smp_pmon", False), ("up_msh", True)]:
            results.append(run_board_test(mode, send_help=send_help))
    finally:
        with open(RTCONFIG, "w") as f:
            f.write(backup)

    print("\n\n=== SUMMARY ===")
    for r in results:
        print(r)
    return 0 if all(r.get("ok") for r in results) else 1


if __name__ == "__main__":
    sys.exit(main())
