#!/usr/bin/env python3
"""
Host2 (192.168.49.121) chip24 fpfifo A/B: hp640 vs RTT.

Run ON the test host (needs sudo for /dev/ttyUSB0):
  cd /home/lynxi/xia/xmodem/scripts/hp232x
  sudo python3 ab_fpfifo_host2.py

Or from dev machine:
  HP232X_TEST_HOST=192.168.49.121 python3 ab_fpfifo_host2.py --remote

Rules (learned the hard way):
  - One Link = one test at a time (never parallel fpfifo / other Host jobs)
  - If Link OR KA200 (chip) offline → must reset Link, then re-UART-boot
  - Never fuser ttyUSB (D-state hang)
  - Open serial BEFORE reset; require .U before xmodem
  - hp640: 1 xmodem pass; RTT: 2 passes (bootcode/IRAM0 after biz)
  - Close serial BEFORE fpfifo_stress
  - Kill leftover fpfifo/dfifo/rfifo before starting
"""

from __future__ import print_function

import argparse
import fcntl
import glob
import os
import re
import shutil
import subprocess
import sys
import time

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
DEFAULT_XDIR = "/home/lynxi/xia/xmodem"
DEFAULT_FPFIFO = "/mnt/49.20/lynxlink/staging/fifo_test/fpfifo_stress"
DEFAULT_LOGDIR = "/tmp/fpfifo_ab"
LINK_LOCK = "/tmp/lynxlink_link0_test.lock"

RTT_FW = "rtthread-header.bin"
HP640_FW = "u-boot-spl-hp640-header.bin"

RTT_MARKERS = ["Entering main task processing loop", "Entering main task"]
HP640_MARKERS = [
    "Entering main task processing loop",
    "Heart-beat reported successfully",
    "Querying tasks from eMMC",
]

# Host stress tools that must not share a Link
HOST_TEST_PATTERNS = (
    "fpfifo_stress",
    "dfifo_test",
    "rfifo_test",
    "diagnostic",
)


def run(cmd, timeout=120, check=False):
    print("[cmd] %s" % cmd, flush=True)
    return subprocess.run(
        cmd, shell=True, timeout=timeout, check=check,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)


def strip_ansi(text):
    return re.sub(r"\x1b\[[0-9;]*m", "", text or "")


def get_topo(timeout_s=10):
    return run("timeout %d lynx-showinfo" % timeout_s,
               timeout=timeout_s + 5).stdout or ""


def link_alive(topo_text, link=0):
    clean = strip_ansi(topo_text)
    m = re.search(r"\[Link%d\].*ALIVE:([0-9A-Fa-f]+)" % link, clean, re.I)
    if not m:
        return False
    try:
        return int(m.group(1), 16) != 0
    except ValueError:
        return False


def board_chip_alive(topo_text, board, chip):
    """Board ALIVE bitmap must have chip bit set (KA200 online)."""
    clean = strip_ansi(topo_text)
    m = re.search(r"\[%d\].*ALIVE:([0-9A-Fa-f]+)" % board, clean)
    if not m:
        return False
    try:
        alive = int(m.group(1), 16)
    except ValueError:
        return False
    return bool(alive & (1 << (chip % 32)))


def reset_link(reset_cmd):
    """Reset whole Link. Required whenever Link or KA200 goes offline."""
    cmd = reset_cmd
    if "timeout" not in cmd:
        cmd = "timeout -k 3 20 %s" % cmd
    print("[ab] RESET (Link/KA offline policy): %s" % cmd, flush=True)
    run(cmd, timeout=30)
    time.sleep(3)


def ensure_online_or_reset(args, context=""):
    """
    Check Link + target KA200. If either offline → reset Link.
    Returns (ok, topo, did_reset).
    After reset, UART-booted chips usually stay offline until xmodem re-boot.
    """
    topo = get_topo()
    link_ok = link_alive(topo, 0)
    chip_ok = board_chip_alive(topo, args.board, args.chip)
    if link_ok and chip_ok:
        print("[ab] online OK%s (Link+chip%d)" % (context, args.chip), flush=True)
        return True, topo, False

    why = []
    if not link_ok:
        why.append("Link offline")
    if not chip_ok:
        why.append("KA200 chip%d offline" % args.chip)
    print("[ab] OFFLINE%s: %s → reset" % (context, ", ".join(why)), flush=True)
    print(topo[:600], flush=True)
    reset_link(args.reset_cmd)
    topo2 = get_topo()
    link_ok2 = link_alive(topo2, 0)
    chip_ok2 = board_chip_alive(topo2, args.board, args.chip)
    print("[ab] after reset: Link=%s chip%d=%s" %
          (link_ok2, args.chip, chip_ok2), flush=True)
    return (link_ok2 and chip_ok2), topo2, True


def list_host_tests():
    """Return pids of other Host stress tools (exclude our pid / parents)."""
    me = os.getpid()
    out = []
    try:
        r = subprocess.run(
            ["ps", "-eo", "pid,args"], capture_output=True, text=True, timeout=10)
    except Exception:
        return out
    for line in (r.stdout or "").splitlines():
        line = line.strip()
        if not line:
            continue
        parts = line.split(None, 1)
        if len(parts) < 2:
            continue
        try:
            pid = int(parts[0])
        except ValueError:
            continue
        if pid == me:
            continue
        args = parts[1]
        if any(p in args for p in HOST_TEST_PATTERNS):
            # skip our own recursive invocations shown in ssh wrappers
            if "ab_fpfifo_host2.py" in args:
                continue
            out.append((pid, args[:120]))
    return out


def ensure_link_exclusive(force_kill=True):
    """
    One Link supports only one Host test. Abort or kill leftovers.
    Also take a flock so two ab_fpfifo_host2 cannot run together.
    """
    lock_fd = open(LINK_LOCK, "w")
    try:
        fcntl.flock(lock_fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        print("[ab] FAIL: another ab_fpfifo / Link test holds %s" % LINK_LOCK,
              flush=True)
        sys.exit(2)
    # keep lock_fd open for process lifetime
    ensure_link_exclusive._lock_fd = lock_fd  # noqa: B010

    others = list_host_tests()
    if others:
        print("[ab] WARN: leftover Host tests on Link:", flush=True)
        for pid, args in others:
            print("  pid=%d %s" % (pid, args), flush=True)
        if not force_kill:
            print("[ab] FAIL: refuse to run while Link busy", flush=True)
            sys.exit(3)
        for pid, _ in others:
            try:
                os.kill(pid, 9)
            except OSError:
                pass
        time.sleep(1.5)
        left = list_host_tests()
        if left:
            print("[ab] FAIL: still busy after kill:", left, flush=True)
            sys.exit(3)
        print("[ab] killed leftovers — Link free", flush=True)
    else:
        print("[ab] Link exclusive OK (no other Host test)", flush=True)


def point_spl(xdir, fw_name):
    fw = os.path.join(xdir, fw_name)
    if not os.path.isfile(fw):
        raise FileNotFoundError(fw)
    spl_link = os.path.join(xdir, "u-boot-spl.bin")
    if os.path.islink(spl_link) or os.path.exists(spl_link):
        os.remove(spl_link)
    os.symlink(fw_name, spl_link)
    print("[ab] u-boot-spl.bin -> %s" % fw_name, flush=True)


def parse_avg_fps(text):
    # Prefer last "Avg FPS: x.xx"
    hits = re.findall(r"Avg FPS:\s*([0-9]+\.[0-9]+)", text)
    if hits:
        return float(hits[-1])
    return None


def run_fpfifo(fpfifo, board, chip, blk, test_time, glevel, log_path,
               run_as=None):
    # Re-check exclusivity right before starting (Link = one test)
    others = list_host_tests()
    if others:
        print("[ab] FAIL: Link busy before fpfifo: %s" % others, flush=True)
        return None, "link_busy", ""
    cmd = "%s -d 0 -B %d:%d -b%d -T %d -g %d" % (
        fpfifo, board, chip, blk, test_time, glevel)
    # Prefer non-root: Host DMA UAPI often fails oddly under sudo/root.
    if run_as and os.geteuid() == 0:
        cmd = "sudo -u %s -E %s" % (run_as, cmd)
    print("[ab] fpfifo: %s" % cmd, flush=True)
    r = run(cmd, timeout=test_time + 40)
    with open(log_path, "w") as f:
        f.write(r.stdout or "")
    print(r.stdout[-2000:] if r.stdout else "(no out)", flush=True)
    fps = parse_avg_fps(r.stdout or "")
    err = None
    out = r.stdout or ""
    if "Reduce fifo empty" in out:
        err = "reduce_empty"
    elif "离线" in out or "offline" in out.lower():
        err = "offline"
    elif fps is None and r.returncode != 0:
        err = "rc_%d" % r.returncode
    return fps, err, out


def boot_fw(name, xdir, fw, markers, passes, args, logdir):
    from flash_common import flash_until_marker, patch_boot_wrapper_spl

    print("\n======== %s boot (passes=%d) ========" % (name, passes), flush=True)
    point_spl(xdir, fw)
    patch_boot_wrapper_spl(
        os.path.join(xdir, "u-boot-spl.bin"),
        os.path.join(xdir, "boot-wrapper.bin"))
    ok, boot_text = flash_until_marker(
        xdir, markers, passes=passes, log_sec=args.boot_log_sec,
        reset_cmd=args.reset_cmd)
    boot_log = os.path.join(logdir, "%s_boot.log" % name)
    with open(boot_log, "w") as f:
        f.write(boot_text)
    if not ok:
        print("[ab] %s BOOT FAIL — see %s" % (name, boot_log), flush=True)
        return False
    settle = max(2.0, float(args.settle_sec))
    print("[ab] settle %.1fs after boot (serial closed)" % settle, flush=True)
    time.sleep(settle)
    return True


def ab_one_side(name, xdir, fw, markers, passes, args, logdir, max_tries=2):
    """
    Boot + verify online + fpfifo.
    Offline (Link/KA) → reset + re-boot (policy).
    """
    run_as = args.run_as if args.run_as else None

    for attempt in range(1, max_tries + 1):
        print("\n---- %s attempt %d/%d ----" % (name, attempt, max_tries),
              flush=True)

        # If already offline before boot, reset first (then xmodem)
        online, topo, did_reset = ensure_online_or_reset(
            args, context=" before %s" % name)
        if did_reset and not online:
            print("[ab] reset done; UART chip needs re-boot", flush=True)

        if not boot_fw(name, xdir, fw, markers, passes, args, logdir):
            if attempt < max_tries:
                reset_link(args.reset_cmd)
                continue
            return None, "boot_fail"

        online, topo, did_reset = ensure_online_or_reset(
            args, context=" after %s boot" % name)
        with open(os.path.join(logdir, "%s_topo.txt" % name), "w") as f:
            f.write(topo)
        print(topo[:800], flush=True)

        if not online:
            # Policy: offline → reset; then next attempt re-boots
            print("[ab] still offline after boot → reset + retry", flush=True)
            if not did_reset:
                reset_link(args.reset_cmd)
            continue

        fps, err, _ = run_fpfifo(
            args.fpfifo, args.board, args.chip, args.blk, args.test_time,
            args.glevel, os.path.join(logdir, "%s_fpfifo.log" % name),
            run_as=run_as)

        if err == "offline" or err == "reduce_empty":
            # Offline during/after test, or Host path broken → reset + re-boot
            print("[ab] fpfifo err=%s → check online / reset" % err, flush=True)
            online2, _, _ = ensure_online_or_reset(
                args, context=" after fpfifo err")
            if not online2 or err == "offline":
                if attempt < max_tries:
                    continue
            elif err == "reduce_empty" and attempt < max_tries:
                # one soft retry without full re-boot if still online
                time.sleep(3)
                fps, err, _ = run_fpfifo(
                    args.fpfifo, args.board, args.chip, args.blk,
                    args.test_time, args.glevel,
                    os.path.join(logdir, "%s_fpfifo_retry.log" % name),
                    run_as=run_as)
                if fps is not None:
                    print("[ab] %s FPS=%s err=%s" % (name, fps, err), flush=True)
                    return fps, err
                # still bad → reset + full re-boot next attempt
                reset_link(args.reset_cmd)
                continue

        print("[ab] %s FPS=%s err=%s" % (name, fps, err), flush=True)
        return fps, err

    return None, "retry_exhausted"


def run_local(args):
    sys.path.insert(0, SCRIPT_DIR)
    os.chdir(SCRIPT_DIR)

    ensure_link_exclusive(force_kill=True)

    logdir = args.logdir
    os.makedirs(logdir, exist_ok=True)
    xdir = args.xdir

    results = {}
    # hp640 first (1 pass) — user confirmed works
    fps, err = ab_one_side(
        "hp640", xdir, HP640_FW, HP640_MARKERS, 1, args, logdir)
    results["hp640"] = (fps, err)

    # RTT needs 2 passes after any prior biz (or after hp640 run)
    fps, err = ab_one_side(
        "rtt", xdir, RTT_FW, RTT_MARKERS, 2, args, logdir)
    results["rtt"] = (fps, err)

    print("\n======== A/B SUMMARY ========", flush=True)
    for k, (fps, err) in results.items():
        print("  %s: FPS=%s err=%s" % (k, fps, err), flush=True)
    hp, rtt = results.get("hp640", (None, None))[0], results.get("rtt", (None, None))[0]
    if hp and rtt and hp > 0:
        gap = (hp - rtt) / hp * 100.0
        print("  gap: RTT is %.1f%% slower (hp640=%.2f rtt=%.2f)" %
              (gap, hp, rtt), flush=True)
    with open(os.path.join(logdir, "summary.txt"), "w") as f:
        for k, (fps, err) in results.items():
            f.write("%s FPS=%s err=%s\n" % (k, fps, err))
    return 0 if (hp and rtt) else 1


def run_remote(args):
    """Upload scripts/fw and execute on test host via sshpass."""
    import shlex

    host = args.host
    user = args.user
    password = args.password
    bsp = os.path.dirname(SCRIPT_DIR)
    rtt_fw = os.path.join(bsp, "rtthread-header.bin")
    hp640_fw = args.hp640_fw

    def ssh(cmd, timeout=600):
        full = "sshpass -p %s ssh -o StrictHostKeyChecking=no %s@%s %s" % (
            shlex.quote(password), user, host, shlex.quote(cmd))
        print("[ssh] %s" % (cmd[:140],), flush=True)
        return subprocess.run(full, shell=True, timeout=timeout)

    def scp_put(src, dst):
        full = "sshpass -p %s scp -o StrictHostKeyChecking=no %s %s@%s:%s" % (
            shlex.quote(password), shlex.quote(src), user, host,
            shlex.quote(dst))
        return subprocess.run(full, shell=True, timeout=120)

    def scp_get(src, dst):
        full = "sshpass -p %s scp -o StrictHostKeyChecking=no %s@%s:%s %s" % (
            shlex.quote(password), user, host, shlex.quote(src),
            shlex.quote(dst))
        return subprocess.run(full, shell=True, timeout=60)

    if not os.path.isfile(rtt_fw):
        print("ERROR: missing %s — build first" % rtt_fw)
        return 1
    if not os.path.isfile(hp640_fw):
        print("ERROR: missing %s" % hp640_fw)
        return 1

    print("[remote] upload firmware + scripts", flush=True)
    scp_put(rtt_fw, "%s/rtthread-header.bin" % args.xdir)
    scp_put(hp640_fw, "%s/%s" % (args.xdir, HP640_FW))
    tar = "/tmp/hp232x_ab_scripts.tgz"
    subprocess.check_call(
        "cd %s && tar czf %s *.py" % (SCRIPT_DIR, tar), shell=True)
    scp_put(tar, "/tmp/hp232x_ab_scripts.tgz")
    ssh("mkdir -p %s/scripts/hp232x && "
        "cd %s/scripts/hp232x && tar xzf /tmp/hp232x_ab_scripts.tgz" %
        (args.xdir, args.xdir))

    inner = (
        "cd {xdir}/scripts/hp232x && "
        "python3 ab_fpfifo_host2.py --local "
        "--xdir {xdir} --fpfifo {fpfifo} --board {board} --chip {chip} "
        "--blk {blk} --test-time {tt} --glevel {g} --logdir {logdir} "
        "--boot-log-sec {bls} --settle-sec {settle} --run-as {run_as}"
    ).format(
        xdir=args.xdir, fpfifo=args.fpfifo, board=args.board, chip=args.chip,
        blk=args.blk, tt=args.test_time, g=args.glevel, logdir=args.logdir,
        bls=args.boot_log_sec, settle=args.settle_sec, run_as=args.run_as)
    remote_cmd = "echo %s | sudo -S -p '' bash -lc %s" % (
        shlex.quote(password), shlex.quote(inner))
    print("[remote] exec A/B (T=%ds blk=%d)" % (args.test_time, args.blk),
          flush=True)
    rc = ssh(remote_cmd, timeout=args.test_time * 4 + 500)

    scp_get("%s/summary.txt" % args.logdir, "/tmp/fpfifo_ab_summary.txt")
    if os.path.isfile("/tmp/fpfifo_ab_summary.txt"):
        print("\n[remote] summary:", flush=True)
        print(open("/tmp/fpfifo_ab_summary.txt").read(), flush=True)
    for name in ("hp640_fpfifo.log", "rtt_fpfifo.log",
                 "hp640_boot.log", "rtt_boot.log"):
        dst = "/tmp/%s" % name
        scp_get("%s/%s" % (args.logdir, name), dst)
        if os.path.isfile(dst) and name.endswith("fpfifo.log"):
            print("[remote] %s Avg FPS=%s" % (name, parse_avg_fps(open(dst).read())),
                  flush=True)
    return rc.returncode


def main():
    ap = argparse.ArgumentParser(description="chip24 fpfifo A/B hp640 vs RTT")
    ap.add_argument("--remote", action="store_true",
                    help="orchestrate from dev machine via SSH")
    ap.add_argument("--local", action="store_true",
                    help="run on test host (default if no --remote)")
    ap.add_argument("--host", default=os.environ.get(
        "HP232X_TEST_HOST", "192.168.49.121"))
    ap.add_argument("--user", default="lynxi")
    ap.add_argument("--password", default="lx@123")
    ap.add_argument("--xdir", default=DEFAULT_XDIR)
    ap.add_argument("--fpfifo", default=DEFAULT_FPFIFO)
    ap.add_argument("--hp640-fw", default=os.environ.get(
        "HP640_FW",
        "/work/lynxlink/output/uboot/spl/u-boot-spl-hp640-header.bin"))
    ap.add_argument("--board", type=int, default=0)
    ap.add_argument("--chip", type=int, default=24)
    ap.add_argument("--blk", type=int, default=16)
    ap.add_argument("--test-time", type=int, default=20)
    ap.add_argument("--glevel", type=int, default=0)
    ap.add_argument("--boot-log-sec", type=int, default=30)
    ap.add_argument("--settle-sec", type=float, default=4.0,
                    help="wait after boot before fpfifo (serial must be free)")
    ap.add_argument("--run-as", default="lynxi",
                    help="run fpfifo as this user when script is root")
    ap.add_argument("--logdir", default=DEFAULT_LOGDIR)
    ap.add_argument("--reset-cmd", default="lynx-showinfo -r -l 0")
    args = ap.parse_args()

    if args.remote:
        return run_remote(args)
    return run_local(args)


if __name__ == "__main__":
    sys.exit(main() or 0)
