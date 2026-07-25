# E2E RAN LDPC-Offload — Low-Level Design (LLD)

**Type:** Low-Level Design. Concrete data structures, function signatures, byte layouts, and
pseudocode. This is strict code-level guidance for a mid-level implementer — **follow these
structures and signatures**; if you must deviate, note it in the gate report with rationale.
Verify all srsRAN API names against `third_party/srsRAN_Project` before use (names below are the
intended seams, not guaranteed exact symbols).

Constants used throughout (define in `e2e/common/e2e_defs.h`):
```c
#define Z_DEFAULT        384
#define N_ITER           6
#define LLR_MAX_BYTES    26112   /* n_vn_full(68) * Z(384) — largest LLR buffer  */
#define TB_MAX_BYTES     1056    /* ceil(n_vn_info(22)*Z(384)/8) — largest TB     */
#define RING_CAP_DEFAULT 64      /* bounded queue; MUST be a power of two          */
#define CACHELINE        64
```

---

## 1. Directory / file layout (create exactly this)

```
e2e/
  common/
    e2e_defs.h            # constants above
    cxl_region.h  .c      # libcxlregion: region alloc + ring + slot state machine
    ecpri.h       .c      # eCPRI pack/parse
    ldpc_params.h .c      # BG1/BG2 graph params + bg_detect()
    e2e_config.h  .c      # config load from env/file
    timing.h              # ns_now() inline
  generator/
    generator.cpp         # M1 (host): srsRAN encode/mod/map/awgn/ecpri + ground truth
  guest/
    rx_ecpri.c            # M2: socket RX -> re_grid_t
    phy_uplink.cpp        # M3: srsRAN equalize+soft-demap -> LLR -> CXL slot
    ldpc_accel.c          # M4: CXL LLR -> OpenCL decode -> CXL TB
    consumer.c            # M5: CXL TB -> CRC + compare -> CSV
  scripts/
    run_wsl_integration.sh
    launch_qemu_cxl.sh    # GCP (gate 5)
    setup_numa.sh
    README_GCP.md
  Makefile
```

---

## 2. libe2ecommon — shared types & helpers

### 2.1 `ldpc_params.h/.c` — graph params + BG detection
```c
typedef struct {
    int bg;          /* 1 or 2                        */
    int n_vn_full;   /* 68 (BG1) / 52 (BG2)           */
    int n_cn;        /* 46 (BG1) / 42 (BG2)           */
    int n_vn_info;   /* 22 (BG1) / 10 (BG2)           */
    int Z;
} ldpc_graph_t;

/* Returns 0 on success, -1 if llr_len/Z is not a known srsRAN benchmark config.
 * n_vn_eff = llr_len / Z ; BG1 accepts {24,66}; BG2 accepts {12,50}. NEVER guess. */
int bg_detect(uint32_t llr_len, int Z, ldpc_graph_t *out);

/* BG shift tables (reuse existing tables in the repo). */
const uint16_t *bg_shifts(int bg, int ls_idx, size_t *n_elems); /* [n_cn*n_vn_full] */
```
BG-detect pseudocode:
```
n_vn_eff = llr_len / Z
if n_vn_eff in {24,66}: bg=1,n_vn_full=68,n_cn=46,n_vn_info=22
elif n_vn_eff in {12,50}: bg=2,n_vn_full=52,n_cn=42,n_vn_info=10
else: return -1   /* caller logs + marks slot error, does NOT skip silently */
```

### 2.2 `timing.h`
```c
static inline uint64_t ns_now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*1000000000ull + t.tv_nsec; }
```

### 2.3 `e2e_config.h/.c`
```c
typedef struct {
    int      cxl_node;        /* env CXL_NODE, default 0 (WSL) / 1 (GCP)   */
    char     region_name[64]; /* e.g. "/e2e_region"                        */
    int      ring_cap;        /* default RING_CAP_DEFAULT (power of two)   */
    int      n_slots;         /* total slots to run                        */
    int      Z, bg_force;     /* bg_force=0 => generator cycles configs    */
    int      mod_order;       /* 2=QPSK,4=16QAM,6=64QAM,8=256QAM           */
    double   snr_db;          /* single value; sweep handled by harness    */
    char     udp_addr[64]; int udp_port;
    char     gt_dir[256];     /* ground-truth dir                          */
    char     csv_path[256];
} e2e_config_t;
int e2e_config_load(e2e_config_t *cfg);  /* env overrides defaults */
```

---

## 3. libcxlregion — the CXL-backed shared region (portability core)

### 3.1 Slot & ring layout
```c
enum slot_status { SLOT_EMPTY=0, SLOT_INUSE=1, SLOT_READY_LLR=2, SLOT_READY_TB=3, SLOT_DONE=4 };

typedef struct __attribute__((aligned(CACHELINE))) {
    _Atomic uint32_t status;      /* slot_status; the state machine driver    */
    uint32_t seq;                 /* global sequence (survives reordering)     */
    uint32_t bg, Z, llr_len;      /* filled by M3                              */
    uint32_t tb_len_bits;         /* filled by M4                              */
    int32_t  bit_errors;          /* filled by M5 (-1 = error/unrecognized)    */
    uint8_t  crc_pass;            /* filled by M5                              */
    uint64_t t_llr_ready_ns;      /* M3 */  uint64_t t_tb_ready_ns;   /* M4 */
    uint64_t t_rx_release_ns;     /* set by producer path for e2e_us           */
    int8_t   llr[LLR_MAX_BYTES];  /* M3 writes; M4 reads (transits CXL)         */
    uint8_t  tb [TB_MAX_BYTES];   /* M4 writes; M5 reads (transits CXL)         */
} e2e_slot_t;

typedef struct {
    uint32_t magic, ring_cap, slot_bytes;
    _Atomic uint64_t head;        /* producer (M3) reserve counter             */
    _Atomic uint64_t tail;        /* consumer (M5) free counter                */
    _Atomic uint64_t cxl_transits;/* incremented by M4 each CB (gate D assert) */
    /* slots[] follow, ring_cap * sizeof(e2e_slot_t) */
} e2e_ring_hdr_t;
```

### 3.2 API
```c
typedef struct { e2e_ring_hdr_t *hdr; e2e_slot_t *slots; int node; int fd; size_t bytes; } cxl_region_t;

/* Create-or-attach a named region on NUMA node cfg->cxl_node.
 * WSL: node 0 (single node). GCP: node 1 (CXL system-ram). Backend chosen ONLY here. */
cxl_region_t *cxl_region_open(const e2e_config_t *cfg, int create);
void          cxl_region_close(cxl_region_t *r);

/* Producer (M3): block until a slot is free (head-tail<cap), return it INUSE. Backpressure here. */
e2e_slot_t   *ring_reserve(cxl_region_t *r, uint32_t seq);
/* Wait (bounded spin + sched_yield) until slots[idx].status == want; returns slot ptr. */
e2e_slot_t   *ring_wait_status(cxl_region_t *r, uint64_t idx, enum slot_status want);
/* Publish a status transition with release semantics. */
void          slot_publish(e2e_slot_t *s, enum slot_status st);
/* Consumer (M5): mark DONE and advance tail (frees slot for producer). */
void          ring_release(cxl_region_t *r, uint64_t idx);
```
**Memory ordering:** writers fill fields THEN `atomic_store_explicit(&status, st, release)`;
readers `atomic_load_explicit(&status, acquire)` THEN read fields. This guarantees the payload is
visible before the status flip.

**Allocation (the ONLY WSL/GCP difference):**
```c
/* inside cxl_region_open */
r->fd = shm_open(cfg->region_name, O_CREAT|O_RDWR, 0600);
ftruncate(r->fd, bytes);
void *p = mmap(NULL, bytes, PROT_READ|PROT_WRITE, MAP_SHARED, r->fd, 0);
if (numa_available() >= 0)
    mbind(p, bytes, MPOL_BIND, node_mask(cfg->cxl_node), maxnode, MPOL_MF_MOVE|MPOL_MF_STRICT);
/* verify: get_mempolicy on p -> log the node actually backing it.  NEVER mmap /dev/dax. */
```
On WSL `mbind` to node 0 is a no-op-ish success; on GCP it lands on the CXL node. Same code path.

---

## 4. eCPRI wire format (`ecpri.h/.c`) — simplified, documented

```c
#pragma pack(push,1)
typedef struct {            /* 8 bytes, eCPRI-like common header */
    uint8_t  version;       /* 0x10 (our simplified marker)      */
    uint8_t  msg_type;      /* 0x00 = U-plane IQ data            */
    uint16_t payload_size;  /* network order, bytes after header */
    uint16_t pc_id;         /* flow id (network order)           */
    uint16_t seq_id;        /* fragment sequence (network order) */
} ecpri_hdr_t;

typedef struct {            /* U-plane section descriptor        */
    uint32_t slot;          /* global slot/seq                   */
    uint16_t symbol;        /* OFDM symbol index                 */
    uint16_t start_re;      /* first resource element in payload */
    uint16_t num_re;        /* REs in this fragment              */
    uint16_t _rsvd;
} uplane_hdr_t;
#pragma pack(pop)
/* Payload: num_re * sizeof(cf_t) interleaved float32 I,Q (simplified; real 7.2x uses BFP int16). */
```
API:
```c
/* Fragment a slot's RE grid into <=MTU eCPRI packets. Returns #packets written to buf list. */
int  ecpri_pack_grid(const re_grid_t *g, uint32_t seq, uint8_t **pkts, int *lens, int max);
/* Parse one packet into (uplane_hdr, cf_t* payload). Returns 0/-1. */
int  ecpri_parse(const uint8_t *pkt, int len, uplane_hdr_t *uh, const cf_t **iq);
```

---

## 5. M1 Generator (`generator.cpp`, host)

```c
typedef struct { int num_re; cf_t *re; int num_prb, num_sym; } re_grid_t; /* freq-domain grid */
```
Pseudocode:
```
cfg = load(); open UDP; mkdir(gt_dir)
for seq in [0, n_slots):
  graph = pick_config(seq, bg_force)                 # cycles BG1/BG2 min/max
  K = graph.n_vn_info * Z                            # info bits
  b = prng(seed=seq, K bits)
  write_file(gt_dir/tb_<seq>.bin, b)                 # GROUND TRUTH (oracle)
  cw   = srsran_ldpc_encode(b, graph)                # real srsRAN encoder
  rm   = rate_match(cw, target_llr_len_for(graph))
  syms = qam_modulate(rm, cfg.mod_order)             # srsRAN modulation_mapper
  grid = map_to_re(syms, graph)                      # RE grid
  add_awgn(grid, cfg.snr_db)                         # so decode is actually exercised
  n = ecpri_pack_grid(grid, seq, pkts,...); send_all(pkts)
  wait_ack_or_gap()                                  # backpressure (non-RT). REQUIRED.
```
Notes: reuse the encoder path already validated in `bit_diff_test.cpp`. `target_llr_len` picks the
benchmark config (25344/19200/9216/4608) so `bg_detect` downstream recognizes it.

---

## 6. M2 RX (`rx_ecpri.c`, guest)

```c
int  rx_open(const e2e_config_t*);
/* Blocks until a full slot is reassembled; fills *g (caller-owned) and *seq. */
int  rx_next_slot(re_grid_t *g, uint32_t *seq);
```
Pseudocode:
```
loop recvfrom():
  ecpri_parse() -> (uh, iq)
  place iq into pending[uh.slot].re[uh.start_re .. +uh.num_re]
  mark received REs; if slot complete -> move to ready queue, record t_rx_release_ns
rx_next_slot(): pop ready queue
```
Handle out-of-order (key by slot/seq) and duplicates (idempotent placement). Timeout an incomplete
slot after K ms → log + drop with a counted `rx_incomplete` metric (do not hang).

---

## 7. M3 PHY (`phy_uplink.cpp`, guest) — REAL srsRAN, no stubs

Intended srsRAN seam (verify exact API in third_party): use the PUSCH demodulation path —
channel estimation + `channel_equalizer` + `demodulation_mapper` (soft) [+ descrambling] to turn
the equalized RE grid into `log_likelihood_ratio` (int8) LLRs. **Do not** use `l1_sim`.
```c
int phy_init(const e2e_config_t*);            /* build srsRAN factories once */
/* Consume grid, produce LLRs into slot->llr (length llr_len), set bg/Z/llr_len. */
int phy_process(const re_grid_t *g, uint32_t seq, cxl_region_t *r);
```
Pseudocode:
```
g,seq = rx_next_slot()
slot = ring_reserve(r, seq); t0 = ns_now(); slot->t_rx_release_ns = g.t_rx_release_ns
llr_i8 = srsran_equalize_and_softdemap(g)      # REAL soft LLRs (srsRAN int8 scale)
graph  = graph_for(seq)                         # or infer from llr length
memcpy(slot->llr, llr_i8, llr_len)
memset(slot->llr+llr_len, 0, graph.n_vn_full*Z - llr_len)   # zero punctured/untransmitted VNs
slot->bg=graph.bg; slot->Z=Z; slot->llr_len=llr_len; slot->t_llr_ready_ns=ns_now()
slot_publish(slot, SLOT_READY_LLR)              # release-store
# phy_us = slot->t_llr_ready_ns - t0  (recorded by M5 from timestamps)
```
LLR scale: map srsRAN LLR range to what `ldpc_decode.cl` expects (int8, ±). Confirm the kernel's
`LLR_MAX=120/INF=127` convention matches; scale if needed (document the mapping).

---

## 8. M4 Accel (`ldpc_accel.c`, guest) — OpenCL, separate process

```c
int accel_init(const e2e_config_t*);   /* PoCL context, build ldpc_decode.cl, alloc cl buffers */
void accel_run(cxl_region_t *r);        /* poll loop until n_slots processed */
```
Pseudocode (the decode path — reuse existing `llr_consumer_v8.c` logic, minus the DEV-040/042 hacks
since the region is now real system-RAM):
```
for expected_seq in order:
  idx = expected_seq % ring_cap
  slot = ring_wait_status(r, idx, SLOT_READY_LLR)     # acquire-load
  if bg_detect(slot->llr_len, slot->Z, &graph) < 0:
      slot->bit_errors=-1; slot_publish(slot, SLOT_READY_TB); continue   # log, do not skip
  clEnqueueFillBuffer(c2v, 0)                          # zero edge buffer
  llr_buf = clCreateBuffer(USE_HOST_PTR, slot->llr)    # reads FROM the CXL region (gate D)
  set_args(graph.n_vn_full,n_cn,n_vn_info,Z,N_ITER,0)
  t0=ns_now(); clEnqueueNDRangeKernel(gws=1); clFinish(); decode_us=(ns_now()-t0)/1000
  copy kernel output bits -> slot->tb; slot->tb_len_bits = graph.n_vn_info*Z
  slot->t_tb_ready_ns=ns_now(); atomic_fetch_add(&hdr->cxl_transits,1)
  slot_publish(slot, SLOT_READY_TB)
# at end: assert hdr->cxl_transits == processed_cb_count  (gate D)
```
The OpenCL kernel, its args, and BG params are the existing, bit-exact `ldpc_decode.cl` — do not
rewrite it.

---

## 9. M5 Consumer (`consumer.c`, guest) — the oracle + CSV

CSV columns (exact): `seq,bg,Z,llr_len,snr_db,crc_pass,bit_errors,phy_us,xfer_us,decode_us,e2e_us,source`
```c
int consumer_run(cxl_region_t *r, const e2e_config_t *cfg);
```
Pseudocode:
```
open csv (write header once)
for expected_seq in order:
  idx=expected_seq % ring_cap
  slot = ring_wait_status(r, idx, SLOT_READY_TB)
  b_hat = unpack_bits(slot->tb, slot->tb_len_bits)
  crc_pass = crc24_check(b_hat)                         # srsRAN crc_calculator (real)
  b_true = read_file(gt_dir/tb_<slot->seq>.bin)
  bit_errors = (slot->bit_errors==-1)? -1 : hamming(b_hat[:K], b_true)
  phy_us   = (slot->t_llr_ready_ns - slot->t_rx_release_ns)/1000   # note: crosses M2/M3, same clock
  xfer_us  = (t_pickup_ns - slot->t_llr_ready_ns)/1000             # t_pickup recorded by M4 (optional field)
  decode_us= (slot->t_tb_ready_ns - t_pickup_ns)/1000
  e2e_us   = (ns_now() - slot->t_rx_release_ns)/1000
  fprintf(csv, ...); ring_release(r, idx)               # frees slot
# end: print totals; expect bit_errors==0 at high SNR, >0 at low SNR
```
If you need `xfer_us`/`decode_us` split precisely, add `t_pickup_ns` to `e2e_slot_t` (M4 sets it at
`ring_wait_status` return). Keep every interval within a single clock domain.

---

## 10. Portability (WSL ↔ GCP) — the config-flip contract

- The ONLY environment-specific code is: `cxl_region_open()` NUMA node selection (via `CXL_NODE`)
  and the launch scripts. Nothing else may branch on WSL/GCP.
- WSL: `CXL_NODE=0`, all processes + generator on one host, UDP loopback, PoCL CPU.
- GCP: `CXL_NODE=1` (CXL system-ram node), generator optionally outside the QEMU guest, same binaries.
- `README_GCP.md` documents: QEMU `cxl-type3` + `daxctl -m system-ram` bring-up, `CXL_NODE=1`,
  and (for 2-NUMA overlay) placing the region on a CPU-less remote node.
- Acceptance for portability: `grep -rn "WSL\|GCP\|node 1\|node1\|CXL_NODE" e2e/` shows matches
  ONLY in `cxl_region.c`, `e2e_config.c`, and `scripts/`.

---

## 11. What to reuse vs write new

| Reuse (do not rewrite) | Write new |
|---|---|
| `ldpc_decode.cl` (bit-exact kernel) | M1 generator, M2 rx, M3 phy wrapper, M5 consumer |
| BG shift tables | libcxlregion API (refactor from `phase5_cxl/cxl_region.c`) |
| `bit_diff_test.cpp` encoder path (as generator reference) | ecpri, ldpc_params, config, timing |
| srsRAN PHY/encoder/CRC (third_party) | Makefile, scripts, README_GCP |

**Do not carry over** the DEV-040 (write-once CXL) and DEV-042 (OCL-reads-stack) shortcuts — those
existed only because the old region was device-dax. With system-RAM NUMA backing, the accelerator
reads LLRs directly from the CXL region for every CB (gate D).
