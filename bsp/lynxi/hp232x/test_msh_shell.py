#!/usr/bin/env python3
"""
HP232X msh shell remote board tests (192.168.49.81, /dev/ttyUSB1).

Usage:
  python3 test_msh_shell.py help       # help command / serial input
  python3 test_msh_shell.py list_isr   # IRQ table (RT_USING_INTERRUPT_INFO)
  python3 test_msh_shell.py ps         # thread list smoke test
  python3 test_msh_shell.py all        # default suite (help + list_isr)
  python3 test_msh_shell.py --list     # list test names
"""

from __future__ import annotations

import argparse
import json
import sys
import time
from dataclasses import dataclass
from typing import Callable, Iterable, List, Optional, Sequence, Tuple

import paramiko

SERVER = "192.168.49.81"
USERNAME = "lynxi"
PASSWORD = "1"
SUDO_PASSWORD = "1"
SERIAL_PORT = "/dev/ttyUSB1"
REMOTE_SCRIPT_PATH = "/tmp/hp232x_msh_shell_test.py"


@dataclass
class MshTestCase:
    name: str
    command: str
    description: str
    pre_delay: float = 1.0
    post_delay: float = 3.0
    wait_cpu1: bool = False
    check: Callable[[str, str], Tuple[bool, str]] = lambda _text, _cmd: (True, "ok")


def _no_exception(text: str, _cmd: str) -> Tuple[bool, str]:
    if "Data abort" in text or "Execption" in text:
        return False, "exception after input"
    return True, "ok"


def _check_help(text: str, cmd: str) -> Tuple[bool, str]:
    ok, msg = _no_exception(text, cmd)
    if not ok:
        return ok, msg
    tail = text.split(cmd)[-1] if cmd in text else text
    if "command not found" in tail:
        return False, "help command not found"
    keywords = [
        "RT-Thread shell commands",
        "list_thread",
        "clear",
        "version",
        "ps",
    ]
    found = [k for k in keywords if k in text]
    if found:
        return True, f"keywords: {found}"
    return False, "no help response detected"


def _check_list_isr(text: str, cmd: str) -> Tuple[bool, str]:
    ok, msg = _no_exception(text, cmd)
    if not ok:
        return ok, msg
    tail = text.split(cmd)[-1] if cmd in text else text
    if "command not found" in tail:
        return False, "list_isr command not found"
    if "handler" in text and "counter" in text:
        return True, "list_isr table header present"
    if "IPI_HANDLER" in text or "apb_tick" in text:
        return True, "IRQ entries present"
    return False, "no list_isr response detected"


def _check_ps(text: str, cmd: str) -> Tuple[bool, str]:
    ok, msg = _no_exception(text, cmd)
    if not ok:
        return ok, msg
    tail = text.split(cmd)[-1] if cmd in text else text
    if "command not found" in tail:
        return False, "ps command not found"
    if "thread" in text.lower() and "priority" in text.lower():
        return True, "ps table present"
    if "tshell" in text or "tidle" in text:
        return True, "thread names present"
    return False, "no ps response detected"


TEST_CASES: dict[str, MshTestCase] = {
    "help": MshTestCase(
        name="help",
        command="help",
        description="msh serial input + help listing",
        pre_delay=1.0,
        post_delay=3.0,
        check=_check_help,
    ),
    "list_isr": MshTestCase(
        name="list_isr",
        command="list_isr",
        description="IRQ handler table and counters",
        pre_delay=1.0,
        post_delay=4.0,
        check=_check_list_isr,
    ),
    "ps": MshTestCase(
        name="ps",
        command="ps",
        description="thread list smoke test",
        pre_delay=1.0,
        post_delay=3.0,
        check=_check_ps,
    ),
}

DEFAULT_SUITE = ("help", "list_isr")


def _remote_script_payload(cases: Sequence[MshTestCase]) -> str:
    spec = [
        {
            "name": c.name,
            "command": c.command,
            "pre_delay": c.pre_delay,
            "post_delay": c.post_delay,
            "wait_cpu1": c.wait_cpu1,
        }
        for c in cases
    ]
    cases_json = json.dumps(spec)
    return f'''
import json
import os
import select
import subprocess
import sys
import time

SUDO = {json.dumps(SUDO_PASSWORD)}
TTY = {json.dumps(SERIAL_PORT)}
CASES = json.loads({json.dumps(cases_json)})


def sudo(cmd):
    subprocess.run(
        f"echo '{{SUDO}}' | sudo -S {{cmd}}",
        shell=True,
        capture_output=True,
    )


def wait_readable(fd, timeout):
    deadline = time.time() + timeout
    while time.time() < deadline:
        r, _, _ = select.select([fd], [], [], 0.2)
        if r:
            return os.read(fd, 4096)
    return b""


def drain(fd, timeout):
    chunks = []
    deadline = time.time() + timeout
    while time.time() < deadline:
        r, _, _ = select.select([fd], [], [], 0.2)
        if not r:
            continue
        chunk = os.read(fd, 4096)
        if chunk:
            chunks.append(chunk)
    return b"".join(chunks)


sudo("pkill -9 -f ttyUSB1 2>/dev/null || true")
time.sleep(1)
sudo(f"stty -F {{TTY}} 115200 raw -echo")
time.sleep(0.5)

fd = os.open(TTY, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
results = []
try:
    sudo("lynd_hp run -d 0 -r wdt -o5")
    time.sleep(0.5)
    out = b""
    deadline = time.time() + 35
    while time.time() < deadline:
        chunk = wait_readable(fd, 0.5)
        if chunk:
            out += chunk
            sys.stdout.buffer.write(chunk)
            sys.stdout.flush()
            if b"msh" in out:
                break
    else:
        print("\\n[TEST] msh prompt NOT seen", file=sys.stderr)
        sys.exit(2)

    for case in CASES:
        name = case["name"]
        cmd = case["command"]
        if case.get("wait_cpu1"):
            extra = b""
            deadline = time.time() + 20
            while time.time() < deadline:
                chunk = wait_readable(fd, 0.5)
                if chunk:
                    extra += chunk
                    out += chunk
                    sys.stdout.buffer.write(chunk)
                    sys.stdout.flush()
                    if b"[SMP] CPU1 ready" in out:
                        break
            time.sleep(2.0)
        else:
            time.sleep(float(case.get("pre_delay", 1.0)))

        os.write(fd, f"\\r\\n{{cmd}}\\r\\n".encode())
        print(f"\\n[TEST] Sent: {{cmd}}", flush=True)
        chunk = drain(fd, float(case.get("post_delay", 3.0)))
        if chunk:
            out += chunk
            sys.stdout.buffer.write(chunk)
            sys.stdout.flush()

        results.append({{"name": name, "command": cmd}})

    sys.stdout.buffer.write(b"\\n")
    sys.stdout.flush()
    with open("/tmp/hp232x_msh_shell_out.bin", "wb") as f:
        f.write(out)
    print("[TEST] --- remote done ---")
finally:
    os.close(fd)
'''


def _analyze_output(text: str, cases: Sequence[MshTestCase]) -> List[Tuple[str, bool, str]]:
    report = []
    for case in cases:
        passed, detail = case.check(text, case.command)
        report.append((case.name, passed, detail))
    return report


def run_tests(names: Iterable[str]) -> int:
    name_list = list(names)
    cases = []
    for name in name_list:
        if name not in TEST_CASES:
            print(f"Unknown test: {name}", file=sys.stderr)
            return 2
        cases.append(TEST_CASES[name])

    if len(cases) == 1:
        return _run_remote_session(cases)

    print("=" * 60)
    print(" HP232X msh shell tests (multi-case, one reset each)")
    print("=" * 60)
    print("Tests:", ", ".join(c.name for c in cases))
    print()

    exit_code = 0
    for idx, case in enumerate(cases):
        print(f"\n--- [{idx + 1}/{len(cases)}] {case.name} ---")
        code = _run_remote_session([case])
        if code != 0 and exit_code == 0:
            exit_code = code
    return exit_code


def _run_remote_session(cases: Sequence[MshTestCase]) -> int:
    if len(cases) == 1:
        print("=" * 60)
        print(" HP232X msh shell tests")
        print("=" * 60)
        print("Tests:", cases[0].name)
        print()

    ssh = paramiko.SSHClient()
    ssh.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    ssh.connect(SERVER, username=USERNAME, password=PASSWORD, timeout=10)

    sftp = ssh.open_sftp()
    with sftp.file(REMOTE_SCRIPT_PATH, "w") as remote_file:
        remote_file.write(_remote_script_payload(cases))
    sftp.close()

    _, stdout, stderr = ssh.exec_command(
        f"echo '{SUDO_PASSWORD}' | sudo -S python3 {REMOTE_SCRIPT_PATH}",
        timeout=120,
    )
    captured: List[str] = []
    while not stdout.channel.exit_status_ready():
        if stdout.channel.recv_ready():
            chunk = stdout.channel.recv(4096).decode(errors="ignore")
            captured.append(chunk)
            sys.stdout.write(chunk)
            sys.stdout.flush()
        time.sleep(0.05)

    rest = stdout.read().decode(errors="ignore")
    remote_err = stderr.read().decode(errors="ignore")
    remote_code = stdout.channel.recv_exit_status()
    if rest:
        captured.append(rest)
        sys.stdout.write(rest)
    remote_out = "".join(captured)
    if remote_err:
        sys.stderr.write(remote_err)

    ssh.exec_command(f"rm -f {REMOTE_SCRIPT_PATH} /tmp/hp232x_msh_shell_out.bin")
    ssh.close()

    if remote_code != 0:
        print(f"\nRemote runner failed with exit code {remote_code}")
        return remote_code

    text = remote_out
    report = _analyze_output(text, cases)

    print("\n[TEST] --- analysis ---")
    exit_code = 0
    for idx, (name, passed, detail) in enumerate(report):
        status = "PASS" if passed else "FAIL"
        print(f"[TEST] {status}: {name} — {detail}")
        if not passed:
            exit_code = 10 + idx

    print(f"\nRemote exit code: {remote_code}")
    return exit_code


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(description="HP232X msh shell remote board tests")
    parser.add_argument(
        "tests",
        nargs="*",
        help=f"test name(s): {', '.join(TEST_CASES)} or all",
    )
    parser.add_argument("--list", action="store_true", help="list available tests")
    args = parser.parse_args(argv)

    if args.list:
        for name, case in TEST_CASES.items():
            mark = "*" if name in DEFAULT_SUITE else " "
            print(f"{mark} {name:10} {case.description}")
        print("\n* included in 'all'")
        return 0

    if not args.tests or args.tests == ["all"]:
        names = list(DEFAULT_SUITE)
    else:
        names = list(args.tests)

    return run_tests(names)


if __name__ == "__main__":
    sys.exit(main())
