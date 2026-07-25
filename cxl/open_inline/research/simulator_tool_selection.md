# Simulator Tool Selection — Deep Research

**Date:** 2026-07-19 · **Scope:** which tool(s) form the SIM-tier backbone (traffic-gen + vDU/vCU/L2)
for [`ARCHITECTURE_v3_SIM.md`](../ARCHITECTURE_v3_SIM.md). Builds on
[`phase1_feasibility_cloud_hw.md`](phase1_feasibility_cloud_hw.md) (original srsRAN choice) and
[`../SRSRAN_LIMITATIONS.md`](../SRSRAN_LIMITATIONS.md) (srsRAN→OCUDU transition).

**Question answered:** given the target pipeline —
`eCPRI traffic-gen → vDU/vCU (L2/L3) ⟷ NIC parse/ingest → GPU (complete L1) → CPU handoff (pinned-mem now, CXL later) → L2` —
is there a single tool that covers it, and if not, which single tool should own the RAN-stack half?

---

## 0. Verdict (TL;DR)

**No single tool covers the whole pipeline — and that's structural, not a gap in the search.**
GPU-resident, vendor-portable L1 does not exist as an open component anywhere (confirmed again this
pass); it's the reason this project exists. "Single tool" is achievable only for the **traffic-gen +
vDU/vCU (L2/L3) half**. For that half:

**Recommendation: srsRAN Project / its successor OCUDU — unchanged from the original phase-1
finding, now reinforced by two new results and one new fact:**

1. **OAI has no synthetic, RF-free O-RU equivalent to `ru_emulator`** (checked directly this pass —
   see §2.2). This was "unverified" before; it's now a confirmed gap on OAI's side, not just an
   assumption.
2. **NVIDIA Aerial's own reference architecture validates the pipeline shape** you described
   (RU Emulator → GPU L1 via cuPHY/DOCA → FAPI → L2) — useful confirmation the design pattern is
   sound, not a candidate to adopt (closed to contributions, CUDA/DOCA-locked).
3. **srsRAN Project → OCUDU is a favorable transition, not a risk**, per the license/governance
   research already done ([`SRSRAN_LIMITATIONS.md`](../SRSRAN_LIMITATIONS.md) follow-up): BSD-3
   relicense, Linux Foundation governance, GPU acceleration on OCUDU's own roadmap, AMD+NVIDIA both
   founding members. Re-pin to OCUDU rather than stay on the archived snapshot (action item, §5).

---

## 1. What "the simulator" actually has to cover

| Pipeline stage | Tool-selectable? | Owner |
|---|---|---|
| eCPRI traffic-gen (RU-side, split 7.2x, synthetic/no-RF) | ✅ yes | candidate comparison below |
| vDU/vCU (MAC/RLC/PDCP/SDAP, CU-CP/CU-UP, NGAP to core) | ✅ yes, same tool as above (they ship together) | candidate comparison below |
| Core network (5GC: AMF/SMF/UPF) | ✅ yes, but already decided | Open5GS (unchanged, not re-litigated here) |
| NIC parse/ingest (af_packet in SIM, dmabuf+raw-QP in PHYSICAL) | ❌ no — this is `ingest_backend`, our own code by design (SIM §3 contract) | ours |
| GPU L1 (depacketize→chanest→eq→demap→LDPC) | ❌ no — **the empty intersection**, confirmed again this pass (§3) | ours |
| CPU handoff (pinned-mem now / CXL later) | ❌ no — `handoff_backend`, our own code | ours |
| PHY↔L2 seam | ❌ no — custom IPC by design, confirmed correct in the FAPI research (see `SRSRAN_LIMITATIONS.md` follow-up: packed FAPI is commercially gated in srsRAN; OAI's nFAPI+Aerial path reintroduces NVIDIA dependency) | ours |

So "pick one tool" has a real, bounded scope: the top two rows only. Everything below the line was
already decided to be our own code, independent of which RAN-stack tool wins — restating this so the
tool choice doesn't accidentally get read as a decision about the GPU/ingest/seam architecture too.

---

## 2. Candidates for the RAN-stack half

### 2.1 srsRAN Project / OCUDU — recommended

- **Traffic-gen:** `ru_emulator` — DPDK-bound, split-7.2x OFH (C-plane + U-plane), YAML-configured,
  **genuinely RF-free by design** (already the turnkey finding from phase-1; not re-derived here).
- **vDU/vCU:** integrated `gnb` binary (CU+DU), test-mode MAC-emulated UEs — real L2/L3 on CPU.
- **License/governance (updated):** BSD-3-Clause-OpenMPI, Linux Foundation-hosted, neutral
  multi-vendor governance (AMD, AT&T, DeepSig, Ericsson, Nokia, NVIDIA, Softbank, SRS, Verizon +
  21 general members + 17 research institutions). Was AGPLv3 under the old single-company structure
  — the relicense removes a real copyleft/network-service-disclosure risk.
- **GPU relevance:** not GPU-native today (same CPU codebase, commit history carried over
  intact — 18,810 commits), but GPU acceleration is an explicit OCUDU v2.x roadmap item, and the
  project explicitly positions itself as hardware-neutral ("x86, ARM, GPU, and more"). Directionally
  aligned with our thesis rather than competing with it.
- **Docker/deploy:** official `docker compose up` deployment carries forward
  (`ocudu-docs-604e90.gitlab.io/user_manual/installation`); migration guide describes renamed
  identifiers but "most changes do not require a rewrite" — high-confidence structural continuity,
  **not yet directly verified file-by-file** (action item, §5).
- **Known gaps (already catalogued, unchanged):** `ldpc_decoder_benchmark` default mode has no
  ground-truth LLR (DEV-044); `ru_emulator` ships static UL IQ by default, needs our injection patch
  (already planned as feature `p3-live-tap-ul-inject`). Both are engineering tasks, not blockers.
- **Golden vectors:** in-repo (`tests/unittests/phy`, srsRAN_matlab-derived) — already the basis of
  the `p2-phy-kernels` validation plan; carries forward (same codebase lineage).

### 2.2 OpenAirInterface (OAI) — not recommended for the RAN-stack role

- **Traffic-gen:** **no RF-free O-RU emulator exists.** Checked directly this pass against the
  official `ORAN_FHI7.2_Tutorial.md`: the "no RU attached (for benchmarking)" mode still requires
  real NIC/DPDK interface config, no loopback or synthetic-IQ-injection path documented. The only
  RF-free-adjacent option is **ProtO-RU**, and even that needs a physical SDR (USRP B210) — not
  RF-free at all, just SDR-based rather than COTS-RU-based. This directly contradicts what would be
  needed to match `ru_emulator`'s role.
- **RFsimulator:** real and useful, but it's the ZMQ-equivalent problem already catalogued in
  `SRSRAN_LIMITATIONS.md` §3 — time-domain baseband IQ between an integrated DU+RU and a UE process,
  no split, no eCPRI, no NIC. Wrong layer for fronthaul-mechanism testing, same as srsRAN's ZMQ path.
- **FHI7.2/xran:** real, production-grade, MWC-demoed — but built for a real RU on the other end,
  not as a standalone synthetic generator. Confirmed real but not fit for our "no RF" requirement.
- **Verdict:** OAI remains the stronger option only if we ever need the *stretch-goal* real-FAPI/
  real-L2 integration (per `SRSRAN_LIMITATIONS.md` — OAI+Aerial's FAPI path is proven; srsRAN's
  packed FAPI is commercially gated). Not a reason to make OAI the SIM-tier backbone now.

### 2.3 NVIDIA Aerial (`aerial-cuda-accelerated-ran`) — reference only, not a candidate

- Confirms the exact pipeline shape: **RU Emulator** (`cuPHY-CP/ru-emulator/`, verifies FH timing,
  DL IQ integrity, schedules UL IQ) → **cuPHYController** (sits between L2 and RU fronthaul,
  operates GPU+NIC via **cuPHY + DOCA GPUNetIO + DPDK**) → **TestMAC** (L1/L2 interface, SCF FAPI
  222.10.02/222.10.04) → optionally real OAI L2 instead of TestMAC (confirmed via NVIDIA forum
  thread — this is a documented, asked-about integration path).
- **Why not adopt it:** repo is Apache-2.0 but **"not accepting contributions"**; RU Emulator lives
  nested inside the cuPHY-CP build tree with undocumented standalone-buildability (not confirmed
  buildable without the CUDA/DOCA toolchain); the GPU↔NIC leg uses **closed DOCA GPUNetIO**, not the
  open `dmabuf`/`ibv_reg_dmabuf_mr` mechanism this project's M1 targets. Adopting it would mean
  building on the exact proprietary stack the project exists to provide an open alternative to.
- **Why it still matters:** it's the strongest existing evidence the target architecture *works* —
  useful as a design reference for the RU-Emulator↔GPU-controller↔L2 seam shape, and as the
  benchmark to eventually compare against (never link/copy, per existing licensing hygiene rule).

### 2.4 Ruled out without a deep dive (reasoning only)

| Candidate | Why not |
|---|---|
| **O-RAN-SC O-DU/O-RU simulators** | Python-based, OAM/FCAPS-layer simulators (file-based PM, SMO integration) for management-plane testing at scale (e.g. 1000 simulated DUs) — not a C/U-plane eCPRI traffic source. Wrong layer entirely. |
| **ns-3 / 5G-LENA / ns-O-RAN** | Discrete-event *network* simulators — model scheduling/RF behavior abstractly, don't execute a real protocol stack or emit real wire-format eCPRI frames. Can't feed a real GPU pipeline real bytes. |
| **Amarisoft** | Mature, capable, industry-standard — but closed-source/commercial. Directly conflicts with the project's open-source thesis; not evaluated further. |
| **DeepSig** (OCUDU founding member) | AI-native signal-processing tooling, not a CU/DU/traffic-gen stack — tangential to this decision, not a candidate. |

---

## 3. Re-confirmation: the GPU-L1 gap is still empty (no new candidate found)

Re-searched this pass specifically looking for anything that might have closed the gap since the
original finding (`ARCHITECTURE_v3.md` §6): still true that a complete GPU L1 exists only as
CUDA-locked (Aerial cuPHY), and vendor-portable L1 exists only on CPU (srsRAN/OCUDU, OAI). No new
entrant found. This is not a tooling oversight — it's the thesis.

---

## 4. Recommendation

**Single tool for the RAN-stack half: srsRAN Project, re-pinned to OCUDU.** Unchanged conclusion
from phase-1, now with OAI's gap directly confirmed (not just assumed) and the license/governance
transition resolved favorably. Everything below the NIC in the pipeline (ingest, GPU L1, handoff,
seam) stays our own code under any choice — that scope was never in play.

## 5. Action items surfaced by this research

1. **Re-pin to OCUDU before more spec work references srsRAN specifics** — verify directly (clone
   or browse `gitlab.com/ocudu/ocudu`) that `ru_emulator`, `docker/docker-compose.yml`, and
   `tests/unittests/phy` golden vectors carry over in the same relative paths before updating
   `ARCHITECTURE_v3*.md` and the `p0`/`p1`/`p2` specs. Migration-guide language ("most changes do
   not require a rewrite") is high-confidence but not yet file-verified.
2. Update version pin across docs from `srsRAN Project release_24_10_1` to whatever OCUDU release
   tag is current (first OCUDU release was **26.04**, per `srslte.com` press release — verify this
   is still current before pinning).
3. `p0-rig-scaffold` spec (already written, 3 files) references srsRAN Project specifically —
   review for OCUDU-specific renames once #1 is confirmed; likely small/mechanical per the migration
   guide, not a rewrite.

## Sources

srsRAN ru_emulator (unchanged from phase-1): github.com/srsran/srsRAN_Project/discussions/517 ·
OAI FHI7.2 tutorial (RF-free gap confirmed this pass): github.com/OPENAIRINTERFACE/openairinterface5g/blob/develop/doc/ORAN_FHI7.2_Tutorial.md ·
OAI RFsimulator: gitlab.eurecom.fr/oai/openairinterface5g/-/blob/develop/radio/rfsimulator/README.md ·
ProtO-RU (SDR-based, not RF-free): researchgate.net/publication/398269744 ·
NVIDIA Aerial repo/architecture: github.com/NVIDIA/aerial-cuda-accelerated-ran,
docs.nvidia.com/aerial (Test MAC and RU Emulator Architecture Overview, cuBB End-to-End guides) ·
Aerial+OAI L2 integration question: forums.developer.nvidia.com/t/question-about-using-oai-l2-instead-of-testmac-with-ru-emulator-and-cuphy-controller/371560 ·
O-RAN-SC simulators (ruled out): o-ran-sc.org/blog/2025/07/24, docs.o-ran-sc.org ·
OCUDU migration/governance: ocudu-docs-604e90.gitlab.io/migration, srslte.com/press_releases/srsran_becomes_ocudu,
gitlab.com/ocudu/ocudu, github.com/srsran/srsRAN_Project/discussions/1470 ·
srsRAN limitations baseline: `../SRSRAN_LIMITATIONS.md`.
