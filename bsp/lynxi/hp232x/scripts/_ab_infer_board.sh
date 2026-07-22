#!/bin/bash
# Board-side A/B for run_rtt.sh (chip30 flash swap; chip31 stays hp640).
set -euo pipefail

LOOPS=30
LOGDIR=/tmp/apu_infer_ab
HP640_BIN=""
RTT_BIN=""
FIFO_DIR=/home/lynxi/xia/fifo_test
LINK=0
BOARD=2
CHIP30=30
CHIP31=31

while [[ $# -gt 0 ]]; do
  case "$1" in
    --loops) LOOPS="$2"; shift 2 ;;
    --logdir) LOGDIR="$2"; shift 2 ;;
    --hp640) HP640_BIN="$2"; shift 2 ;;
    --rtt) RTT_BIN="$2"; shift 2 ;;
    --fifo) FIFO_DIR="$2"; shift 2 ;;
    *) echo "unknown $1"; exit 2 ;;
  esac
done

mkdir -p "$LOGDIR"
KA200_TOOLS="${KA200_TOOLS:-/usr/local/lynx/tools/ka200_tools}"

flash_pair() {
  local bin30="$1"
  local bin31="$2"
  local tag="$3"
  echo "[ab] flash board$BOARD c30=$(basename "$bin30") c31=$(basename "$bin31") ($tag)"
  if [[ ! -f "$bin30" || ! -f "$bin31" ]]; then
    echo "[ab] FAIL missing bin30=$bin30 bin31=$bin31"
    exit 3
  fi
  lynx-showinfo | tee "$LOGDIR/${tag}_topo_pre.txt" | head -20
  timeout -k 5 120 "$KA200_TOOLS" -u "$bin30" -l "$LINK" -i "$BOARD" -k "$CHIP30" \
    2>&1 | tee "$LOGDIR/${tag}_upgrade_c30.log"
  timeout -k 5 120 "$KA200_TOOLS" -u "$bin31" -l "$LINK" -i "$BOARD" -k "$CHIP31" \
    2>&1 | tee "$LOGDIR/${tag}_upgrade_c31.log"
  echo "[ab] reset link after flash"
  timeout -k 3 30 lynx-showinfo -r -l "$LINK" || true
  sleep 8
  lynx-showinfo | tee "$LOGDIR/${tag}_topo.txt" | head -25
}

run_infer_loops() {
  local tag="$1"
  local log="$LOGDIR/${tag}_infer.log"
  echo "[ab] infer $tag loops=$LOOPS -> $log"
  cd "$FIFO_DIR"
  local ok=0
  local fail=0
  local i
  : >"$log"
  for ((i=0; i<LOOPS; i++)); do
    echo "==== $tag loop $i ====" | tee -a "$log"
    set +e
    ./inferPerf llama_7b_concat_2c_0_c0 \
        llama_7b_concat_2c_0_c0/chip_0/hp640/task_execbd.bin 1 \
        -x 0 -k 95 94 -o c0.dat c1.dat -i in1024.dat in1024.dat \
        >>"$log" 2>&1
    local rc=$?
    set -e
    if [[ $rc -eq 0 ]]; then
      ok=$((ok+1))
      echo "[ab] $tag loop $i OK" | tee -a "$log"
    else
      fail=$((fail+1))
      echo "[ab] $tag loop $i FAIL rc=$rc" | tee -a "$log"
      break
    fi
    sleep 0.1
  done
  echo "$tag ok=$ok fail=$fail loops_done=$((ok+fail))" | tee -a "$LOGDIR/summary.txt"
}

echo "=== A/B infer start $(date) loops=$LOOPS ===" | tee "$LOGDIR/summary.txt"

# Baseline: both chips hp640 4.11
flash_pair "$HP640_BIN" "$HP640_BIN" hp640
run_infer_loops hp640

# DUT: chip30=RTT (UART), chip31=hp640 (same as user setup)
flash_pair "$RTT_BIN" "$HP640_BIN" rtt
run_infer_loops rtt

echo "=== A/B infer done $(date) ===" | tee -a "$LOGDIR/summary.txt"
cat "$LOGDIR/summary.txt"
