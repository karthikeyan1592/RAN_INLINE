## Spec

Gate 1 — Uprobe fires on GCP build of ldpc_decoder_benchmark

Criteria:
- (a) Uprobe offset from `/etc/cxl_poc_uprobe_offset` matches GCP build (NOT 0x35280 from DO, NOT 0x2fef0 from in-VM build)
- (b) Uprobe fires on at least 1 decode() call (confirmed via tracefs or bpftime handler)
- (c) Binary confirmed as PIE (base=0), so nm output = file offset = perf_event offset

GCP host build offset: `0x30cf0` (from `nm` on GCP-built binary, as saved by build_tools.sh).
If the instance was reprovisioned and srsRAN rebuilt, re-derive:
```bash
nm /path/to/ldpc_decoder_benchmark | grep -i 'ldpc_decoder_impl.*decode\b' | awk '{print $1}'
```

## Commands

```bash
# Inside VM (where binary was rsynced + rebuilt natively):
cat /etc/cxl_poc_uprobe_offset

BENCH=$(find /root/cxl/third_party/srsRAN_Project/build \
  -name ldpc_decoder_benchmark -type f | head -1)
echo "BENCH=$BENCH"
file "$BENCH"

OFFSET=$(cat /etc/cxl_poc_uprobe_offset | tr -d 'UPROBE_OFFSET= ')
echo "Offset=$OFFSET"

# Quick uprobe fire check via tracefs (no bpftime, just kernel):
TRACE=/sys/kernel/debug/tracing
echo 0 > $TRACE/tracing_on
echo "" > $TRACE/trace
echo "p:g1_check $BENCH:$OFFSET" >> $TRACE/uprobe_events
echo 1 > $TRACE/events/uprobes/g1_check/enable
echo 1 > $TRACE/tracing_on
"$BENCH" -L 384 -I 5 -T avx2 -R 3
echo 0 > $TRACE/tracing_on
grep g1_check $TRACE/trace | head -5
echo "hit count:"
grep -c g1_check $TRACE/trace
echo 0 > $TRACE/events/uprobes/g1_check/enable
echo "-:g1_check" >> $TRACE/uprobe_events
```

## Raw evidence

<!-- PASTE VERBATIM TERMINAL OUTPUT BELOW -->

```

```

<!-- END RAW EVIDENCE -->

## Self-verdict

| Criterion | Status | Evidence |
|-----------|--------|----------|
| (a) offset from /etc/cxl_poc_uprobe_offset | PENDING | |
| (b) at least 1 uprobe fire | PENDING | |
| (c) binary is PIE | PENDING | |

**Overall: PENDING**
