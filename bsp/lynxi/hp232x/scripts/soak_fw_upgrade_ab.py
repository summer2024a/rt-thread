#!/usr/bin/env python3
"""
单芯片 Flash 升级生效挂测：hp640 ↔ RTT 交替升级 → 复位 → lynx-showinfo 验上线+版本。

默认 Host1：Link0 Board2 Chip30（Flash 冷启，非 UART）。
拓扑用 --linkid/--boardid/--chipid（或 -l/-i/-k，对齐 ka200_tools）。

在测试机上：
  python3 soak_fw_upgrade_ab.py --cycles 100 -l 0 -i 2 -k 30 \\
    --hp640 /home/lynxi/xia/ka200/hp232x_ka200_4.11.bin \\
    --rtt   /home/lynxi/xia/ka200/HP232x_KA200_Serdes_Update_20260720_v5.0.bin

在开发机上：
  python3 scripts/soak_fw_upgrade_ab.py --remote --cycles 100 \\
    --linkid 0 --boardid 2 --chipid 30
"""

from __future__ import print_function

import argparse
import os
import re
import shlex
import subprocess
import sys
import time
from datetime import datetime

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
BSP_DIR = os.path.dirname(SCRIPT_DIR)

DEFAULT_HOST = "192.168.58.36"
DEFAULT_USER = "lynxi"
DEFAULT_PASS = "lx@123"
KA200_TOOLS = "/usr/local/lynx/tools/ka200_tools"
DEFAULT_HP640 = "/home/lynxi/xia/ka200/hp232x_ka200_4.11.bin"
DEFAULT_RTT = (
    "/home/lynxi/xia/ka200/HP232x_KA200_Serdes_Update_20260720_v5.0.bin"
)
DEFAULT_LOGDIR = "/tmp/soak_fw_upgrade_ab"


def run(cmd, timeout=180):
    print("[cmd] %s" % cmd, flush=True)
    return subprocess.run(
        cmd, shell=True, timeout=timeout,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)


def strip_ansi(text):
    return re.sub(r"\x1b\[[0-9;]*m", "", text or "")


def get_topo(timeout_s=15):
    r = run("timeout %d lynx-showinfo" % timeout_s, timeout=timeout_s + 5)
    return r.stdout or ""


def reset_link(link, settle_sec):
    run("timeout -k 3 30 lynx-showinfo -r -l %d" % link, timeout=40)
    time.sleep(settle_sec)


def board_chip_alive(topo, board, chip):
    clean = strip_ansi(topo)
    m = re.search(r"\[%d\].*ALIVE:([0-9A-Fa-f]+)" % board, clean)
    if not m:
        return False
    try:
        alive = int(m.group(1), 16)
    except ValueError:
        return False
    return bool(alive & (1 << (chip % 32)))


def link_alive(topo, link=0):
    clean = strip_ansi(topo)
    m = re.search(r"\[Link%d\].*ALIVE:([0-9A-Fa-f]+)" % link, clean, re.I)
    if not m:
        return False
    try:
        return int(m.group(1), 16) != 0
    except ValueError:
        return False


def _parse_chip_token(token, ver_map):
    """Parse one token like '30:5.0' or '0~29:4.11' or '31~31:4.11'."""
    token = token.strip()
    if not token or ":" not in token:
        return
    left, ver = token.rsplit(":", 1)
    ver = ver.strip()
    if "~" in left:
        a, b = left.split("~", 1)
        lo, hi = int(a), int(b)
        for c in range(lo, hi + 1):
            ver_map[c] = ver
    else:
        ver_map[int(left)] = ver


def parse_ka200_versions(topo, board=None):
    """
    Return dict chip_id -> version string from lynx-showinfo KA200> field.
    Forms:
      KA200>4.11
      KA200>0~29:4.11 30:5.0
      KA200>0~30:4.11 31~31:5.0
    Prefer the line for ``board`` when given (avoid other boards).
    """
    clean = strip_ansi(topo)
    if board is not None:
        m = re.search(
            r"\[%d\].*?KA200>((?:(?!\|\|).)+)" % board, clean, re.S | re.I)
    else:
        m = re.search(r"KA200>((?:(?!\|\|).)+)", clean)
    if not m:
        return {}
    body = m.group(1).strip()
    ver_map = {}
    # Uniform version for all chips on that board line (no chip id)
    if re.match(r"^\d+\.\d+$", body):
        return {"__all__": body}
    for tok in body.split():
        _parse_chip_token(tok, ver_map)
    return ver_map


def chip_version(topo, chip, board=None):
    vers = parse_ka200_versions(topo, board=board)
    if "__all__" in vers:
        return vers["__all__"]
    return vers.get(chip)


def versions_match(got, expect):
    """Compare '5.0' / '4.11' loosely (prefix ok)."""
    if got is None or expect is None:
        return False
    g = str(got).strip()
    e = str(expect).strip()
    return g == e or g.startswith(e) or e.startswith(g)


def upgrade_chip(fw, link, board, chip, timeout=180):
    """
    Host-side ka200_tools -u.

    Note: \"Send Update Command/Firmware Successfully\" only means Host finished
    sending; KA200 FlashWrite may still be in progress. Caller must wait
    ``post_upgrade_delay`` before link reset.
    """
    cmd = (
        "timeout -k 5 %d %s -u %s -l %d -i %d -k %d"
        % (timeout, KA200_TOOLS, shlex.quote(fw), link, board, chip)
    )
    t0 = time.time()
    r = run(cmd, timeout=timeout + 30)
    out = r.stdout or ""
    elapsed = time.time() - t0
    # Strict: Host tool's success string (do not trust bare rc==0)
    ok = "Send Update Command/Firmware Successfully" in out
    if not ok and "Successfully" in out and r.returncode == 0:
        ok = True
    print("[up] ka200_tools rc=%d elapsed=%.1fs ok=%s" % (
        r.returncode, elapsed, ok), flush=True)
    return ok, out


def wait_online_version(args, expect_ver, rounds=8):
    """Poll showinfo until chip online and version matches (or rounds exhausted)."""
    last_topo = ""
    for i in range(rounds):
        last_topo = get_topo()
        alive = board_chip_alive(last_topo, args.board, args.chip)
        ver = chip_version(last_topo, args.chip, board=args.board)
        link_ok = link_alive(last_topo, args.link)
        print(
            "[chk] poll %d/%d link=%s chip%d alive=%s ver=%s expect=%s"
            % (i + 1, rounds, link_ok, args.chip, alive, ver, expect_ver),
            flush=True,
        )
        if link_ok and alive and versions_match(ver, expect_ver):
            return True, last_topo, ver
        time.sleep(args.poll_sec)
    return False, last_topo, chip_version(last_topo, args.chip, board=args.board)


def one_side(args, tag, fw, expect_ver, logdir, cycle):
    stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    prefix = os.path.join(logdir, "c%03d_%s_%s" % (cycle, tag, stamp))
    print("\n======== cycle %d %s fw=%s expect=%s ========" % (
        cycle, tag, fw, expect_ver), flush=True)

    if not os.path.isfile(fw):
        print("[FAIL] missing firmware: %s" % fw, flush=True)
        return False

    # Pre: chip must be online, then settle before ka200_tools -u
    topo0 = get_topo()
    if not (link_alive(topo0, args.link) and
            board_chip_alive(topo0, args.board, args.chip)):
        print("[pre] offline → reset before upgrade", flush=True)
        reset_link(args.link, args.settle_sec)
        topo0 = get_topo()
        if not (link_alive(topo0, args.link) and
                board_chip_alive(topo0, args.board, args.chip)):
            print("[FAIL] chip still offline before upgrade", flush=True)
            with open(prefix + "_pre_topo.txt", "w") as f:
                f.write(topo0)
            return False

    print("[pre] chip%d online → wait %.1fs then upgrade" % (
        args.chip, args.pre_upgrade_delay), flush=True)
    time.sleep(args.pre_upgrade_delay)

    attempts = 1 + max(0, args.upgrade_retries)
    last_ver = None
    last_topo = ""
    for attempt in range(1, attempts + 1):
        if attempt > 1:
            print("[retry] upgrade attempt %d/%d (prev ver=%s)" % (
                attempt, attempts, last_ver), flush=True)
            # Ensure chip online before re-flash
            topo_r = get_topo()
            if not (link_alive(topo_r, args.link) and
                    board_chip_alive(topo_r, args.board, args.chip)):
                reset_link(args.link, args.settle_sec)

        ok_up, up_out = upgrade_chip(fw, args.link, args.board, args.chip)
        with open(prefix + "_upgrade_a%d.log" % attempt, "w") as f:
            f.write(up_out)
        if not ok_up:
            print("[FAIL] ka200_tools upgrade failed (attempt %d)" % attempt,
                  flush=True)
            print(up_out[-1500:], flush=True)
            if attempt < attempts:
                continue
            return False
        print("[ok] Host sent firmware (Successfully); "
              "FlashWrite may still be running on KA200", flush=True)

        # Critical: do NOT reset until KA200 has time to finish FlashWrite.
        # Host \"Successfully\" != flash done (see flash_host_upgrade_*.py UART wait).
        delay = args.post_upgrade_delay
        print("[ab] wait %.1fs after upgrade before reset (FlashWrite settle)"
              % delay, flush=True)
        time.sleep(delay)

        print("[ab] reset after upgrade", flush=True)
        reset_link(args.link, args.settle_sec)

        ok, last_topo, last_ver = wait_online_version(
            args, expect_ver, rounds=args.poll_rounds)
        with open(prefix + "_topo_a%d.txt" % attempt, "w") as f:
            f.write(last_topo)
        if ok:
            print("[PASS] cycle %d %s online ver=%s" % (cycle, tag, last_ver),
                  flush=True)
            return True
        print("[warn] attempt %d: after reset got ver=%s expect=%s" % (
            attempt, last_ver, expect_ver), flush=True)

    with open(prefix + "_topo.txt", "w") as f:
        f.write(last_topo)
    print("[FAIL] after reset: alive/version check failed (got ver=%s)" % last_ver,
          flush=True)
    print(strip_ansi(last_topo)[:800], flush=True)
    return False


def find_latest_rtt_local():
    import glob
    pats = sorted(glob.glob(os.path.join(
        BSP_DIR, "HP232x_KA200_Serdes_Update_*.bin")))
    return pats[-1] if pats else None


def run_board(args):
    os.makedirs(args.logdir, exist_ok=True)
    summary = os.path.join(args.logdir, "summary.txt")
    print("[cfg] linkid=%d boardid=%d chipid=%d cycles=%d" % (
        args.link, args.board, args.chip, args.cycles), flush=True)
    with open(summary, "a") as f:
        f.write("=== soak start %s cycles=%d link=%d board=%d chip=%d ===\n" % (
            datetime.now(), args.cycles, args.link, args.board, args.chip))

    # sides: (tag, fw, expect_ver) alternating; start with hp640 then rtt
    sides = [
        ("hp640", args.hp640, args.hp640_ver),
        ("rtt", args.rtt, args.rtt_ver),
    ]
    if args.start == "rtt":
        sides = list(reversed(sides))

    ok_n = 0
    fail_n = 0
    for c in range(1, args.cycles + 1):
        tag, fw, ver = sides[(c - 1) % 2]
        ok = one_side(args, tag, fw, ver, args.logdir, c)
        line = "cycle=%d %s %s\n" % (c, tag, "PASS" if ok else "FAIL")
        with open(summary, "a") as f:
            f.write(line)
        if ok:
            ok_n += 1
        else:
            fail_n += 1
            if not args.keep_going:
                break
        time.sleep(args.gap_sec)

    msg = "=== soak done ok=%d fail=%d ===\n" % (ok_n, fail_n)
    print(msg, flush=True)
    with open(summary, "a") as f:
        f.write(msg)
    return 0 if fail_n == 0 else 1


def run_remote(args):
    password = args.password
    user = args.user
    host = args.host

    def ssh(cmd, timeout=86400):
        full = "sshpass -p %s ssh -o StrictHostKeyChecking=no %s@%s %s" % (
            shlex.quote(password), user, host, shlex.quote(cmd))
        return run(full, timeout=timeout)

    def scp_to(src, dst):
        full = (
            "sshpass -p %s scp -o StrictHostKeyChecking=no %s %s@%s:%s"
            % (shlex.quote(password), shlex.quote(src), user, host,
               shlex.quote(dst))
        )
        return run(full, timeout=300)

    # Push latest RTT image if present locally
    rtt_local = find_latest_rtt_local()
    if rtt_local and os.path.isfile(rtt_local):
        remote_rtt = "/home/lynxi/xia/ka200/%s" % os.path.basename(rtt_local)
        scp_to(rtt_local, remote_rtt)
        args.rtt = remote_rtt

    scp_to(__file__, "/tmp/soak_fw_upgrade_ab.py")
    print("[remote] linkid=%d boardid=%d chipid=%d cycles=%d" % (
        args.link, args.board, args.chip, args.cycles), flush=True)
    inner = (
        "python3 /tmp/soak_fw_upgrade_ab.py "
        "--cycles %d --linkid %d --boardid %d --chipid %d "
        "--hp640 %s --rtt %s --hp640-ver %s --rtt-ver %s "
        "--settle %g --pre-upgrade-delay %g --post-upgrade-delay %g "
        "--upgrade-retries %d --poll-sec %g --poll-rounds %d "
        "--gap %g --logdir %s"
        % (
            args.cycles, args.link, args.board, args.chip,
            shlex.quote(args.hp640), shlex.quote(args.rtt),
            shlex.quote(args.hp640_ver), shlex.quote(args.rtt_ver),
            args.settle_sec, args.pre_upgrade_delay, args.post_upgrade_delay,
            args.upgrade_retries, args.poll_sec, args.poll_rounds, args.gap_sec,
            shlex.quote(args.logdir),
        )
    )
    if args.keep_going:
        inner += " --keep-going"
    if args.start == "rtt":
        inner += " --start rtt"
    r = ssh(inner, timeout=args.cycles * 300 + 600)
    print(r.stdout or "", flush=True)
    return r.returncode


def main():
    ap = argparse.ArgumentParser(
        description="Soak: alternate hp640/RTT single-chip flash upgrade + verify")
    ap.add_argument("--remote", action="store_true",
                    help="orchestrate via SSH to test host")
    ap.add_argument("--host", default=os.environ.get("HP232X_TEST_HOST", DEFAULT_HOST))
    ap.add_argument("--user", default=os.environ.get("HP232X_TEST_USER", DEFAULT_USER))
    ap.add_argument("--password",
                    default=os.environ.get("HP232X_TEST_PASS", DEFAULT_PASS))
    ap.add_argument("--cycles", type=int, default=50,
                    help="number of upgrade rounds (each round one fw)")
    # Topology: same meaning as ka200_tools -l / -i / -k
    ap.add_argument(
        "-l", "--link", "--linkid", dest="link", type=int,
        default=int(os.environ.get("HP232X_LYNXLINK", "0")),
        help="link id (ka200_tools -l), default 0 / env HP232X_LYNXLINK")
    ap.add_argument(
        "-i", "--board", "--boardid", dest="board", type=int,
        default=int(os.environ.get("HP232X_BOARD", "2")),
        help="board id (ka200_tools -i), default 2 / env HP232X_BOARD")
    ap.add_argument(
        "-k", "--chip", "--chipid", dest="chip", type=int,
        default=int(os.environ.get("HP232X_CHIP", "30")),
        help="chip id (ka200_tools -k), default 30 / env HP232X_CHIP")
    ap.add_argument("--hp640", default=DEFAULT_HP640)
    ap.add_argument("--rtt", default=DEFAULT_RTT)
    ap.add_argument("--hp640-ver", default="4.11",
                    help="expected lynx-showinfo version after hp640 boot")
    ap.add_argument("--rtt-ver", default="5.0",
                    help="expected lynx-showinfo version after RTT boot")
    ap.add_argument("--start", choices=("hp640", "rtt"), default="hp640",
                    help="which firmware to flash first")
    ap.add_argument("--settle", dest="settle_sec", type=float, default=8.0,
                    help="seconds to wait after link reset")
    ap.add_argument(
        "--pre-upgrade-delay", type=float, default=1.0,
        help="after KA200 online confirmed, sleep N seconds before ka200_tools -u "
             "(default 1.0)")
    ap.add_argument(
        "--post-upgrade-delay", type=float, default=8.0,
        help="after ka200_tools -u returns Successfully, sleep N seconds BEFORE "
             "lynx-showinfo -r so KA200 can finish FlashWrite (default 8.0; "
             "Host Successfully != flash done)")
    ap.add_argument(
        "--upgrade-retries", type=int, default=1,
        help="extra upgrade+reset attempts if version mismatch (default 1)")
    ap.add_argument("--poll-sec", type=float, default=3.0)
    ap.add_argument("--poll-rounds", type=int, default=10)
    ap.add_argument("--gap", dest="gap_sec", type=float, default=1.0,
                    help="pause between cycles")
    ap.add_argument("--logdir", default=DEFAULT_LOGDIR)
    ap.add_argument("--keep-going", action="store_true",
                    help="continue after a FAIL (default: stop)")
    args = ap.parse_args()

    if args.remote:
        return run_remote(args)
    return run_board(args)


if __name__ == "__main__":
    sys.exit(main() or 0)
