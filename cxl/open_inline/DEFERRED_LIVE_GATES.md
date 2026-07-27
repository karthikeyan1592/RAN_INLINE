# DEFERRED_LIVE_GATES.md — cumulative runbook for the next GCP-VM session

Single, growing record of every gate that could not be run on this host and was instead built +
verified up to the live-rig wall locally. Never edit an entry to claim a deferred gate passed —
append a **Result** subsection under the entry once it's actually been run on the GCP VM, dated,
with the real output. One entry per gate; entries are grouped by feature in build order.

**Correction (found 2026-07-25, during p5 build):** this file's header previously said "no SCTP"
was the wall on this host. Re-running `check_sctp.sh` for real during p5's work shows
`sctp=available (already loaded)` on this WSL2 host now — the kernel module is present here after
all (whether it was loaded by an earlier session or was always available under a kernel this
project hadn't re-checked isn't determinable in retrospect; the check itself is real and current).
This does **not** change any deferral decision below: every gate in this file needs the actual
**live rig up** (real 5GC+gNB+ru_emu containers, NG setup, a real fronthaul bridge with real
traffic) — SCTP kernel support alone doesn't provide that, and standing the full rig up locally is
still out of scope per this session's explicit "no cloud, and don't spin up the heavy rig locally
either" instruction. p5's own `p1-check-sctp` gate (see that feature's section below) now
genuinely PASSes locally as a result, which is itself evidence supporting this correction, not a
reason to attempt any of the deferred gates locally.

Prereqs for the whole session (once, not per-gate): start the GCP VM (`oi-p1-rig`,
`asia-south2-a`, project `cxl-systems-lab-26`), confirm SCTP via
`features/p1-ran-baseline/helpers/check_sctp.sh` (must print `sctp=available`), and rsync this
repo's `open_inline/` tree over (`deploy_and_bring_up.sh`'s own step 2 pattern — this repo has no
git remote, see that script's header for why rsync not clone).

---

## 2026-07-26 third session log — WSL2 local attempt (autonomous overnight run, decisions logged)

**Explicit user directive, superseding the "no cloud, don't spin up the heavy rig locally either"
framing above**: attempt to close p3/p4/p5's remaining live gates on THIS WSL2 host instead of the
GCP VM. Rationale given: SCTP is now available locally (correction already noted above), and the
src-MAC BPF fix cut ingest CPU load ~3x, so the rig may now fit on a 4-core/~7GB host where it
didn't before. User then went to sleep and explicitly authorized autonomous decision-making for
the rest of this run ("I suggest you to decide things but note down the decision what you are
making, i will review in the morning. make sure you finish the work.") — every non-mechanical
decision from this point on is logged in this section, in order, for morning review.

**Step 0 — resource baseline (real numbers, not estimated)**:
- Host: 4 cores, `.wslconfig` caps memory=8GB/swap=4GB. At the start of this attempt: swap was
  100% full (4096/4096MB) and only ~2GB "available" *before* touching anything — caused not by
  leftover OpenInline build processes but by (a) 6 unrelated docker containers (three
  postgres+redis pairs for other numbered projects, one nginx actively crash-looping) and (b) two
  OTHER concurrent Claude Code CLI sessions plus VSCode's `cpptools` C++ IntelliSense indexer
  (2.49GB RSS, persistent, this is the editor's own background process).
- **Decision 1**: asked the user before stopping the unrelated docker containers (not mine to
  judge as safe to kill) — user confirmed safe, stopped them. Effect: negligible (~50MB) — they
  were not the real consumer, contrary to my first assumption.
- **Decision 2**: did NOT touch `cpptools` or the other Claude sessions myself — flagged them and
  asked. User closed the other two sessions manually over several checks. Final state before
  proceeding: swap still shows 4095/4096MB "used" (likely stale/inactive pages, not necessarily
  live pressure — could not fully verify from inside the guest), but **available memory recovered
  from ~2GB to ~3.5GB**, and only this one Claude process remains. `cpptools` still holds ~2.49GB
  and was left alone throughout (not something to kill without breaking the user's editor).
- **Decision 3 (this entry)**: proceeding with the local attempt at ~3.5GB available, per the
  user's explicit go-ahead. This is real headroom, not the ~2GB it was at the start, but still
  materially less than the GCP VM's 16 vCPU / more generous RAM that this rig has historically
  needed real debugging to run acceptably on. Treating the Step 2 escalation criteria (TX stall,
  nonzero socket_drops, OOM/restarts, >1hr projected for 1000 slots) as strict and non-negotiable
  given this tighter margin — escalating to the GCP VM rather than pushing through any of them.

**Decision 4 (real discrepancy from this session's own briefed assumption)**: the briefing for this
attempt assumed "all four rig images already exist" locally (docker-5gc, ocudu/gnb, oi/gpu-phy:dev,
oi/oracle:dev). Checked for real: `docker images` returns **zero images** on this host — nothing
has ever been built here. `third_party/ocudu` is correctly pinned at `release_26_04` and pristine
(`git status --short` empty, 0 changes), and disk has 878GB free, so there's no structural blocker
— this is just materially more local build work than assumed (p0's `oi/gpu-phy:dev`/`oi/oracle:dev`
base images, then the oracle-injection-patched `ocudu/gnb` on top, then whatever p1 needs for its
own gNB/5GC images) before Step 1's patched-image build can even start. Proceeding anyway per the
user's "make sure you finish the work" — this is foundational, non-destructive, necessary work, not
a genuine fork requiring a judgment call, just more of it than expected. Watching `free -m` /
`docker stats` through each build stage given the tighter memory margin than the GCP VM had.

**Decision 5 — WSL crashed mid-build, user restarted it (2026-07-27 ~01:14 local)**: the host WSL2
VM crashed while p1's `bring_up.sh` was building the unpatched `gnb` image (was at 98% of the main
OCUDU compile when it went down). User restarted WSL and told me. Checked real post-crash state
before resuming: docker daemon up fresh, and — real, unexpected good news — **built images survive
a WSL crash/restart** (they're on the WSL2 virtual disk, not just in-memory): `ocudu/gnb:oracle-
injection`, `oi/gpu-phy:dev`, `oi/oracle:dev`, and even `oi_p1-5gc:latest` (5gc had already finished
building before the crash) were all still present. Only `gnb` needs rebuilding — the one build that
was mid-flight when the crash hit. All containers are gone (expected, they don't persist across a
VM restart) — will need `compose up` again once `gnb` is rebuilt. Also notable: **swap is now
completely clear (0/4096MB used) and available memory is 6187MB**, both far better than the ~1-3.5GB
ceiling this session was managing around all night — the reboot cleared whatever had swap pinned
(likely `cpptools`'s accumulated footprint and any other stale process state), so resuming from here
is actually on materially better footing than before the crash, not worse. Resuming
`bring_up.sh` — it's idempotent against existing images (compose only (re)builds an image if the
tag doesn't already exist), so it will skip rebuilding `oi_p1-5gc` and just rebuild `gnb`.

**Step 1 result (real)**: built `oi/gpu-phy:dev` (1.09GB) and `oi/oracle:dev` (174MB) via p0's
`build_images.sh` from scratch (no prior local images existed at all), then built
`ocudu/gnb:oracle-injection` (3.25GB) by copying `third_party/ocudu` to `/tmp/ocudu_patched_local`,
applying `patches/0001-oracle-grid-ul-injection.patch` there (`git apply` exit 0), and building from
that scratch copy — never touching the real pinned checkout. Confirmed `git status --short` on the
real `third_party/ocudu` is empty both before and after. Total build time for all three images: a
little over an hour on this 4-core host (watched every ~60s via `free -m`; available memory
oscillated between ~1.4GB and ~3.5GB through the heaviest compile stages — UHD, DPDK, ASN.1
`f1ap.cpp`, the full OCUDU lib+apps build — but never approached the 400MB warning threshold, no
OOM, no thrashing observed). The unpatched `ocudu/gnb` (for the real gNB/DU) and the `5gc` (open5gs)
images are not yet built — `docker compose up` builds them automatically on first bring-up per
their own `build:` blocks in `p0-rig-scaffold/docker/upstream/docker/docker-compose.yml`, so no
separate manual step was needed for those.

**Decision 6 — ESCALATION FIRED at p1's own baseline, before Step 2 (p3's smoke run) was even
reached (2026-07-27, ~02:00 local)**: ran p1's full `bring_up.sh` (baseline images, no oracle
injection, no gpu-phy — the plainest possible version of this rig) to establish a clean starting
point before layering p3's overlay. It built `gnb` (resuming past the WSL crash, using Docker's
on-disk build cache — the apt/UHD/DPDK layers cache-hit instantly, only the interrupted OCUDU
compile layer re-ran), brought up 5gc+gnb+ru-emu, passed the 60s stability hold (0 restarts on all
three), passed the gnb NG-setup-attempt log check, and passed the P1-R8 eCPRI classifier check
(30s capture, 829,232 real frames, 163,366 of them real UL, `vlan_tagged: true`) — all real, all
genuinely passing. **Then the 10-minute soak (P1-R9) failed for real**: final JSON was
`{"check": "soak", "seconds": 600, "restarts": 0, "new_error_lines": 0, "counters_monotonic":
false, "ngap_stable": true}` — `counters_monotonic: false` (not `null`; both start/end snapshots
returned real numeric values, so this isn't a probe failure, it's a real reading) means gnb's own
fronthaul NIC sysfs `tx_bytes`/`rx_bytes` counters did NOT increase over the full 10-minute window.

Root-caused for real, not assumed, before deciding what this meant: manually re-ran the exact same
sysfs read `soak_stability.sh`/`kpi_snapshot.sh` use, twice, 10 seconds apart, while the containers
were still up — **both reads returned byte-for-byte identical tx_bytes/rx_bytes**
(`2558498992`/`681736978`, unchanged). Then checked `ru-emu`'s own internal KPI table directly via
`docker logs`: **`TX_TOTAL` and `RX_TOTAL` both showing `0`** on every printed row, sustained, with
the log simultaneously flooded with `RU emulator timing worker woke up late` warnings at a rate of
several per second, magnitudes ranging **500us to 4000+us late (14 to 113 symbols late)**. This is
a real, live, currently-happening TX/RX stall — not a counter-reading artifact, not a transient
blip: `ru-emu`'s own real-time timing thread is being scheduled so late and so often that it isn't
managing to transmit or receive any fronthaul symbols at all, even though the container itself
never restarted and `docker stats` showed no single container anywhere near its (generous,
unconstrained) memory limit.

**This is an exact, real match for Step 2's own named escalation criterion ("TX stall / ru-emu
frame production stops or collapses")** — it just fired one step earlier than expected, at p1's
own baseline, before p3's overlay (which would only add MORE load: gpu-phy, oracle injection
processing) was ever brought up. Per the standing instruction not to thrash trying to force an
escalation-criterion match: **stopped the local rig cleanly** (`docker compose down`, confirmed all
3 containers + all 3 networks removed, memory recovered to 6374MB available) rather than retrying
or attempting to tune around a host-level real-time scheduling limit. Working hypothesis (not
proven, flagged as a hypothesis): WSL2's virtualized scheduler doesn't provide the sub-millisecond,
low-jitter wakeup guarantees `ru-emu`'s real-time timing thread needs, in a way a dedicated cloud
VM's scheduler apparently does (this exact rig, patched the same way, ran real UL traffic
successfully multiple times on the GCP VM earlier this same date) — this may be inherent to WSL2
regardless of available CPU/RAM headroom, not something more free memory would fix. Not fully
confirmed (didn't have time/access to test with e.g. a real-time-prioritized process or
Windows-side WSL2 scheduler tuning) — flagging as the leading explanation, not a certainty.

**Per Step 2's own instruction, the next move on a firing criterion is to escalate to the GCP
VM — checked, and this is ALSO currently blocked**: `gcloud auth list` still returns "No
credentialed accounts" in this environment (same blocker hit and worked around via direct SSH
earlier tonight's live-debugging session), and — this time — direct SSH to the VM's static IP
(`34.131.123.68`) **times out** (not "connection refused," a real difference: the VM is not
listening at all, consistent with it being fully powered off, which it is — I stopped it myself via
guest-triggered shutdown at the end of the earlier session tonight, per the standing VM-cost
discipline). Starting a stopped GCE instance requires either `gcloud compute instances start` (no
auth available) or the GCP web console (not accessible from this environment) — there is no way
for me to power it back on from here. **This is a genuine, real blocker, not a judgment call**:
both the local path (real TX stall, likely a WSL2 scheduling limitation) and the escalation path
(no way to reach or start the VM) are closed to me right now. Not thrashing against either.
**Action needed from the user in the morning**: start `oi-p1-rig` (`gcloud compute instances start
oi-p1-rig --zone=asia-south2-a --project=cxl-systems-lab-26`, or via the console) if the live gates
(P3-I1, P4-G4, the GCP half of P5-G2) should proceed on the VM as originally planned. The WSL2
attempt's own real findings above (patched images all built and verified real, p1 baseline
otherwise fully green apart from the TX stall) are not wasted — they're a real, disclosed data
point on this host's suitability, and the built images/patch work carry over to nothing (VM has its
own separate image builds) but the diagnostic finding itself is durable and worth keeping in mind
for future attempts at running this rig on WSL2.

**Decision 7 — what I did next, given both live-gate paths were blocked**: did not leave the host
idle or fabricate a live-gate result. Per Step 5's own scoping ("P5-G2 cannot fully close locally
... produce and archive the WSL ledger"), attempted the WSL-local-only-achievable portion of P5-G2
next (`make simtest`), since it does not require the live rig to produce a real, honest signal —
local suites (p2a-p2f, lint checks, etc.) can genuinely pass or fail independent of the live-rig
gates, and the live-rig-dependent gates failing/being marked incomplete here is itself the correct,
honest outcome given the finding above, not something to route around. See the P5-G2 entry below
for the real result.

*(Continued below as each step completes — Result subsections appended in place, this intro not
edited.)*

---

## 2026-07-26 GCP session log — real progress made, real bug found+fixed, session paused (not failed)

A real live session ran on `oi-p1-rig` this date. Summary, so the next session picks up correctly
rather than re-deriving all of this:

**Done, verified real, still true after VM stop/start (all baked into the VM's disk):**
- p1's full bring-up + 10-min soak: fully green for real (`run-id=20260726T034952Z`).
- `ocudu/gnb:oracle-injection` image: built for real, tagged, on the VM (`docker images` confirms
  it). One real bug found+fixed in the patch itself during this build — see the P3-U1 entry below.
- `oi/gpu-phy:dev` and `oi/oracle:dev`: rebuilt for real on the VM (`p0-rig-scaffold/helpers/
  build_images.sh`), including `libyaml-cpp0.8`/`zlib1g` added to gpu-phy's runtime image (a real,
  necessary fix — `bit_exact_harness`/`gpu_phy_seam_bridge` are dynamically linked against them
  and the base gpu-phy image never needed them before p3/p4's bind-mounted binaries existed).
- OCUDU bootstrap (the libs p3/p4's own tools link against) built for real on the VM.
- p3's and p4's own tool binaries (`osg_gen`, `bit_exact_harness`, `gpu_phy_seam_bridge`, etc.)
  built for real on the VM against those libs — all of p3's and p4's own local test suites
  (145 + 81 assertions) re-run and green ON THE VM, not just locally.
- **A real, live throughput bug found and root-caused, not just worked around**: the `ageing_time=
  0` hub-mode fix needed to make UL traffic visible to gpu-phy (a 3rd promiscuous bridge listener,
  not UL's real destination) also flooded it with ~4x irrelevant DL traffic, costing a measured
  2:1 system/user CPU-time penalty and making a ≥1000-slot run impractical (~2.4 min/slot instead
  of anything close to real-time). **Fixed**: `oi_ingest_af_packet.cpp`'s BPF filter now also
  checks `src_mac == ru_mac`, dropping the DL flood in-kernel before any syscall. Proven locally,
  for real, against the full 840,783-frame archived corpus (both directions, known ground truth):
  exactly 163,268 delivered, 0 socket_drops, plus a dedicated regression case (a real captured DL
  frame, right ethertype, DU MAC as source — confirmed dropped). Full account: p3's own
  `VERIFICATION.md`, "src-MAC BPF filter fix" section.

**What's still open, unaffected by the above (still real gaps, not resolved by this session):**
- Full P3-U1 (≥1000-slot capture+compare), P3-U2 (regression), P3-I1 (full integration), P4-G4,
  P5-G2 — all still need the live rig re-run with the fix applied. Session was paused here
  (VM stopped to save cost) rather than continued, once the throughput root cause was found and
  fixed and re-verified locally — resuming is a fresh live session, not a restart from scratch.

**Exact resume runbook for the next session** (do this once, in order, before any individual
gate's own steps below):
```bash
# 1. Start the VM (same static IP as before, 34.131.123.68 unless re-checked)
gcloud compute instances start oi-p1-rig --zone asia-south2-a --project cxl-systems-lab-26

# 2. Rsync the fix (and anything else changed since) — the VM's tree predates tonight's
#    src-MAC filter fix entirely
rsync -az --delete --exclude='.build/' --exclude='build/' --exclude='__pycache__/' \
  -e "ssh -i ~/.ssh/id_ed25519 -o StrictHostKeyChecking=accept-new" \
  /root/linux_env/cxl/open_inline/ claude@<VM_IP>:~/oi-rig/open_inline/

# 3. Re-apply hub-mode on the fronthaul bridge (real kernel runtime state, NOT expected to survive
#    a VM stop/start -- re-check and re-apply, don't assume):
ssh claude@<VM_IP> '
  cd ~/oi-rig/open_inline/features/p1-ran-baseline/helpers && bash bring_up.sh --hold-seconds 60 --soak-seconds 60
  BRIDGE=$(docker network inspect oi_p1_fronthaul --format "{{.Id}}" | cut -c1-12)
  sudo ip link set br-${BRIDGE} type bridge ageing_time 0
'

# 4. Rebuild p3/p4 tool binaries with the fix (OCUDU libs + gpu-phy/oracle images already on disk,
#    should NOT need rebuilding unless this rsync also touched their own Dockerfiles/patches):
ssh claude@<VM_IP> '
  cd ~/oi-rig/open_inline/features/p3-live-tap-ul-inject && make build/bit_exact_harness build/osg_gen
  cd ~/oi-rig/open_inline/features/p4-phy-l2-seam && make build/gpu_phy_seam_bridge
'

# 5. Regenerate the oracle grid set if /tmp/p3_osg didn't survive the reboot (verify first --
#    /tmp is sometimes tmpfs and does NOT survive a stop/start; check before assuming):
ssh claude@<VM_IP> 'ls /tmp/p3_osg/*.osg 2>&1 | wc -l'   # expect 20; if not, regenerate:
ssh claude@<VM_IP> '
  cd ~/oi-rig/open_inline/features/p3-live-tap-ul-inject
  mkdir -p /tmp/p3_osg && ./build/osg_gen 4 1 0x4601 0 /tmp/p3_osg
'

# 6. Bring up the oracle-injection overlay (compose.p3.yml's command now includes the ru_mac arg;
#    the fix is already baked into the tool binaries themselves, no env var needed) and run P3-I1
#    at the real, full ≥1000-slot target this time (P3_MIN_SLOTS unset = defaults to 1000):
ssh claude@<VM_IP> '
  cd ~/oi-rig/open_inline
  export P3_OSG_DIR=/tmp/p3_osg P3_FEATURE_ROOT=$(pwd)/features/p3-live-tap-ul-inject \
         P3_RU_EMU_ORACLE_CONFIG_PATH=$(pwd)/features/p3-live-tap-ul-inject/docker/configs/ru_emu_oracle_injection.yml \
         GNB_CONFIG_PATH=$(pwd)/features/p1-ran-baseline/docker/configs/gnb_ofh_testmode.yml \
         P1_RU_EMU_CONFIG_PATH=$(pwd)/features/p1-ran-baseline/docker/configs/ru_emu.yml
  COMPOSE_PROJECT_NAME=oi_p1 docker compose \
    -f features/p0-rig-scaffold/docker/upstream/docker/docker-compose.yml \
    -f features/p0-rig-scaffold/docker/compose.sim.yml \
    -f features/p1-ran-baseline/docker/compose.p1.yml \
    -f features/p3-live-tap-ul-inject/docker/compose.p3.yml \
    up -d --force-recreate ru-emu gpu-phy
'
# Then poll: docker logs oi_p1-gpu-phy-1 -- expect the one JSON verdict line within a few minutes
# (not ~40+ min) now that the DL flood is filtered in-kernel. If it's still slow after this fix,
# that falsifies the diagnosis -- the next suspect is bit_exact_harness's own per-slot behavior
# (drain polling), not the filter; the 2:1 system/user CPU signature measured tonight says this
# fix should land it, but verify, don't assume.
```

---

## 2026-07-26 second GCP session log — two more real bugs found+fixed, one of them cross-feature

Continuation of the same date's session above. Resumed the VM, redeployed the busy-loop-backoff
fix and the `emu_cfg.oracle_injection` conversion-line fix (both already committed to the patch
before this log entry — see p3's own `VERIFICATION.md` for those two accounts), then chased the
calibration failure through to its actual root cause. Two more real, distinct bugs found — full
account below; short version: **injection was already correct; two separate comparator/parser bugs
were comparing the wrong bytes.**

**Bug 3 — `pcap_comparator` calibrating against the wrong-direction frame.** Captured live wire
traffic with `tcpdump` after redeploying `ru-emu` with both fixes above. Direct raw-byte search
(Python, independent of any project code) proved the exact 2448-byte oracle payload for
slot 8/symbol 0 was present, byte-for-byte, 1495 times in the capture — injection was genuinely
correct at the source. Yet `pcap_comparator` still reported `"calibration failed on first U-plane
frame"`. Root cause: the bridge tap runs in hub mode (`ageing_time=0`, needed for UL visibility),
so the capture contains BOTH directions of fronthaul traffic, and `pcap_comparator` had no
direction filter — it calibrated against whichever eCPRI frame it saw first in capture order,
which (confirmed directly: the first `ethertype=0xAEFE` frame in the capture had `src=DU_MAC`) was
a downlink frame no oracle file could ever match. **Fixed**: `pcap_comparator.cpp` now requires a
`<ru_mac>` CLI arg and skips any frame whose Ethernet source address isn't the RU emulator's own
MAC before it ever reaches preparse/calibration. Regression test added
(`pcap_comparator_test.cpp`'s DL-noise case): a stream with a leading DU-sourced frame whose payload
matches no oracle file must still calibrate correctly once the direction filter is in place.

**Bug 4 — the real bug underneath Bug 3, and the reason fixing Bug 3 alone still didn't pass:**
after fixing the direction filter, `pcap_comparator` STILL failed calibration, now on the correct
(RU-sourced) first frame. Direct byte-offset investigation (hex-dumping the real captured frame at
the position the tool assumed payload started) found the real IQ payload begins 2 bytes later than
`OI_WIRE_TOTAL_HEADER_BYTES(eth_hdr_len)` computes — bytes `[34]=0x00 [35]=0x00` sit between the
O-RAN section header and the real IQ data on every real RU-sourced frame checked. Root-caused by
reading the real OCUDU source (not guessed): OCUDU has TWO U-plane builder classes —
`ofh_uplane_message_builder_static_compression_impl` (0 bytes, the layout this project's shared
`oi_oran_wire_layout.h` assumed) and `..._dynamic_compression_impl` (2 bytes: 1-byte udCompHdr
`data_width<<4|type` + 1 reserved byte). `apps/examples/ofh/ru_emulator.cpp`'s own hand-rolled
frame construction (upstream OCUDU example code, not this project's patch) unconditionally uses the
dynamic layout regardless of `ul_compr_method` config. Confirmed byte-for-byte against **two**
independent real corpora: this session's oracle-injection capture (none/16 config, gap bytes
`0x00 0x00`) and `artifacts/p1/pcaps/20260725T180323Z` (bfp/9 config, 163,268/163,268 real UL
frames matched, gap bytes `0x91 0x00` = `(9<<4)|1` exactly as predicted).

This is a **shared, cross-feature bug**, not a p3-only one: `oi_oran_preparse_frame()` (p2a) and
`k1_depacketizer.cl` (p2c) both silently assumed the static (0-byte) layout, meaning K1's real
GPU-side depacketizer — not just this debug comparator — has been extracting IQ samples from the
wrong offset for any real dynamic-layout frame. **Fixed at the correct layer** (root cause, not a
p3-local workaround): added `oi_frame_desc::payload_byte_off` (the fully-resolved absolute payload
offset, folding in both `eth_hdr_len` and the new `udcomphdr_bytes` fact) and a new required
`udcomphdr_bytes` parameter to `oi_oran_preparse_frame()`, sourced explicitly by every caller (never
sniffed from content — `0x00 0x00` is indistinguishable from "absent" by content alone). K1's kernel
now reads `desc.payload_byte_off` directly instead of re-deriving it. Every caller updated:
`pcap_comparator.cpp`/`bit_exact_harness.cpp` (p3, new required CLI arg), `gpu_phy_seam_bridge.c`
(p4, same), `pipeline_runner.cpp` (p2f, new required CLI arg — `pipeline_test.py` passes 0 for
class-b/oracle-packed, 2 for class-a/P1-captured). Full citation trail and design rationale:
`oi_oran_wire_layout.h`'s header comment. Full test account: p2a/p2c/p3's own `VERIFICATION.md`.

**Regression coverage added**: `preparse_test.cpp` (+2 cases, `udcomphdr_bytes` ABSENT/PRESENT,
including one matching the exact real 36-byte offset found on the wire), `k1_test.cpp` (+1 case
using the REAL OCUDU dynamic builder/decoder pair, not hand-inserted padding — bit-exact K1-kernel
verification, plus a negative control proving the wrong `udcomphdr_bytes` value would have caught
this), `pcap_comparator_test.cpp` (+2 cases, positive + negative control). Full local sweep after
both fixes: see STATUS.md for exact counts (all green, 0 FAIL).

**Still open**: the actual P3-I1 ≥1000-slot live re-run (with all four fixes now applied — busy-loop
backoff, conversion-line, direction-filter, udCompHdr-offset) has NOT yet been executed to
completion this session; VM was used for capture+diagnosis, not a full timed run. This is the
direct next step for the next live session — see the resume runbook above (still valid, add
`udcomphdr_bytes=2` to the manual `pcap_comparator`/`bit_exact_harness` invocations per P3-U1/P3-I1's
own updated command snippets below).

---

## p3-live-tap-ul-inject

### P3-U1 (live-capture half) — "injected frames byte-identical to oracle grids", live session

**What's already verified locally (not deferred):** M4's comparator logic itself — byte
extraction via the shared `oi_oran_preparse_frame`, phase-offset calibration, mismatch detection
— against a synthetic hand-built pcap with real oracle-grid content
(`tests/pcap_comparator_test.cpp`, 8/8 PASS, corruption-detection confirmed). What's deferred is
only running it against a pcap of **real injected traffic from the actual patched ru_emulator**,
which needs the live rig.

**Build the patched image first** (also satisfies P3-R1's binary-level counterpart to the
apply-check already done locally):
```bash
cd ~/oi-rig  # or wherever deploy_and_bring_up.sh rsync'd the repo to
rm -rf /tmp/ocudu_patched && cp -r third_party/ocudu /tmp/ocudu_patched
cd /tmp/ocudu_patched
git apply ~/oi-rig/open_inline/features/p3-live-tap-ul-inject/patches/0001-oracle-grid-ul-injection.patch
docker build -t ocudu/gnb:oracle-injection -f docker/Dockerfile .
```
Expected: patch applies cleanly (already confirmed via `--check` locally, re-confirm for real
here since this is a fresh clone on a different host), image builds without error (the patch adds
`ru_emulator_oracle_grid.cpp` to `apps/examples/ofh/CMakeLists.txt`'s `SOURCES` list, so no
Dockerfile change is needed).

**Generate the oracle grid set** (real OCUDU-linked generator, already built+tested locally):
```bash
cd ~/oi-rig/open_inline/features/p3-live-tap-ul-inject
make bootstrap-ocudu   # reuses p2f's OCUDU_BUILD, real cmake targets, ~minutes not hours
make build/osg_gen
mkdir -p /tmp/p3_osg && ./build/osg_gen 4 1 0x4601 0 /tmp/p3_osg
```
Expected: `osg_gen: OK — MCS 4, wrote 20 files to /tmp/p3_osg (nslot 0..19), self-check passed`
(byte-for-byte the same message this produced locally on the dev host).

**Bring the rig up with p3's compose overlay:**
```bash
export P3_OSG_DIR=/tmp/p3_osg
export P3_FEATURE_ROOT=~/oi-rig/open_inline/features/p3-live-tap-ul-inject
export P3_RU_EMU_ORACLE_CONFIG_PATH=~/oi-rig/open_inline/features/p3-live-tap-ul-inject/docker/configs/ru_emu_oracle_injection.yml
export GNB_CONFIG_PATH=~/oi-rig/open_inline/features/p1-ran-baseline/docker/configs/gnb_ofh_testmode.yml
export P1_RU_EMU_CONFIG_PATH=~/oi-rig/open_inline/features/p1-ran-baseline/docker/configs/ru_emu.yml
COMPOSE_PROJECT_NAME=oi_p3 docker compose \
  -f features/p0-rig-scaffold/docker/upstream/docker/docker-compose.yml \
  -f features/p0-rig-scaffold/docker/compose.sim.yml \
  -f features/p1-ran-baseline/docker/compose.p1.yml \
  -f features/p3-live-tap-ul-inject/docker/compose.p3.yml \
  up -d --force-recreate 5gc gnb ru-emu gpu-phy
```
(This exact 4-file layering + all required env vars was verified to render correctly via
`docker compose config` on the dev host — see `p3-live-tap-ul-inject/VERIFICATION.md`. A real
bug was found and fixed there: `ru-emu`'s `command` needed an explicit override to point at the
new oracle-injection config, since compose's `configs:` list merges additively but `command`
does not.)

**Capture + compare:**
```bash
BRIDGE=$(docker network inspect docker_fronthaul --format '{{.Id}}' | cut -c1-12)
sudo timeout 30 tcpdump -i "br-${BRIDGE}" -w /tmp/p3_injection.pcap 'ether proto 0xaefe or (vlan and ether proto 0xaefe)'
cd ~/oi-rig/open_inline/features/p3-live-tap-ul-inject
make build/pcap_comparator
./build/pcap_comparator /tmp/p3_injection.pcap /tmp/p3_osg 20 02:6f:69:00:01:01 2
# ^ ru_mac + udcomphdr_bytes args REQUIRED as of the 2026-07-26 direction-filter fix and the
#   udCompHdr offset fix (see this file's own "2026-07-26 second session log" section below and
#   oi_oran_wire_layout.h's header comment). udcomphdr_bytes=2 because the real ru_emulator binary
#   always uses the dynamic-compression wire layout, confirmed byte-for-byte against two
#   independent real corpora.
```
**Expected pass criteria:** exit 0, JSON output with `"mismatches":0` and `"phase_offset"` some
value in `[0,20)` (the calibrated offset — do not expect it to be 0; that's only guaranteed if
ru_emu happened to start at a slot-0 boundary, see `oi_harness_calibrate.h`'s own real
reconciliation finding). Nonzero `mismatches` or a calibration failure (nonzero exit before any
JSON line) is a real P3-R3 failure, not a tooling issue — the comparator itself is already proven
correct locally.

### P3-U2 (live-rig regression half) — injection-disabled binary equals upstream behavior

**What's already verified locally (not deferred):** the schema half — the real patched
`ru_emulator_cli11_schema.cpp`/`ru_emulator_appconfig.h` accept both an absent and a present
`oracle_injection` block correctly (`tests/patch_schema_regression_test.cpp`, 13/13 PASS). What's
deferred is running the **actual patched binary** with injection disabled through P1's own full
gate suite and diffing the outcome against the unpatched binary's already-recorded P1 results.

```bash
cd ~/oi-rig/open_inline
# Point P1's bring-up at the PATCHED image (built above) with p1's OWN unmodified config (no
# oracle_injection block at all -- P3-R4's exact claim: absence = identical to upstream).
GNB_IMG_OVERRIDE=ocudu/gnb:oracle-injection \
  features/p1-ran-baseline/helpers/deploy_and_bring_up.sh <same target used before> \
  --identity ~/.ssh/id_ed25519 --soak-seconds 600
```
(`GNB_IMG_OVERRIDE` doesn't exist yet as a real env var in `deploy_and_bring_up.sh` — add a small,
additive `RU_EMU_IMG` override next to its existing `GNB_IMG`/`GC_IMG` variables, defaulting to
today's `ocudu/gnb:latest`, before running this. A real, small, disclosed gap: this override
doesn't exist in the script as committed.)

**Expected pass criteria:** byte-for-byte the same result already recorded in
`p1-ran-baseline/VERIFICATION.md`'s fully-green run (`run-id=20260725T180323Z`):
`{"restarts": 0, "new_error_lines": 0, "counters_monotonic": true, "ngap_stable": true}`,
`bring_up: PASS`. Any difference (a new restart, a new error line, a stability regression) means
the patch changed upstream behavior with injection disabled — a real P3-R4/R5 failure.

### P3-I1 — full live integration: P3-R9 + P3-R11 + P3-R12 + P3-R13 in one session

**What's already verified locally (not deferred):** every individual piece M5's driver calls —
`oi_ingest_af_packet` (19/19 real veth+corpus+pipeline assertions), the calibration algorithm
(43/43), the real `oi_p2_pipeline` setup/feed/launch_slot/drain chain (already proven throughout
p2f-integration). `tools/bit_exact_harness.cpp` itself is compile-verified against the real
pipeline + ingest module but has never been run end-to-end against a live patched ru_emulator +
gnb — that full orchestration is what's deferred.

```bash
cd ~/oi-rig/open_inline/features/p3-live-tap-ul-inject
# (rig already up from P3-U1's steps, oracle injection enabled)
make build/bit_exact_harness
docker cp build/. gpu-phy:/oi/p3-build/ 2>/dev/null || true  # if not already bind-mounted per compose.p3.yml
docker exec ocudu_gpu_phy /oi/p3-build/bit_exact_harness \
  /oi/p2a-scaffold/tests/fixtures/mvp_config.yaml /oracle eth0 20 1000 4 02:6f:69:00:01:01 2 &
  # ^ ru_mac arg REQUIRED as of the 2026-07-26 src-MAC filter fix; udcomphdr_bytes=2 arg REQUIRED
  #   as of the 2026-07-26 udCompHdr offset fix -- see this file's own "2026-07-26 GCP session log"
  #   and "2026-07-26 second session log" sections above; the actual session used compose.p3.yml's
  #   own command array instead of this manual docker-exec form, which already includes both args.
HARNESS_PID=$!
# Concurrently, P3-R12's DU-undisturbed check (M6, reuses P1's own script unmodified):
./helpers/run_du_undisturbed_check.sh --seconds 600 &
wait $HARNESS_PID
```
**Expected pass criteria:** `bit_exact_harness` exits 0 with JSON
`{"slots_completed": >=1000, "tb_mismatches": 0, "crc_mismatches": 0, "socket_drops": 0,
"feed_backpressure": 0, "parse_failed": <tolerated, real C-plane frames in the mix>}`; the
concurrent `run_du_undisturbed_check.sh` (= p1's `soak_stability.sh`) reports
`{"restarts": 0, "new_error_lines": 0, "ngap_stable": true, "counters_monotonic": true}`. Also
separately verify P3-R9 by comparing `ru-emu`'s own `tx_total_counter` (upstream KPI, printed
periodically to its stdout) against gpu-phy's `ethertype_matched` counter over the same window —
must be equal (frames reaching the tap == frames ru-emu actually sent).

**If this fails:** do not weaken any of the four conditions to get a "pass" — each has its own
already-proven-correct local test, so a live failure here means a real integration-level gap
(most likely candidate, based on this session's own findings: the phase-offset calibration
assumption, or DU-bound frame visibility needing the `tc mirred` fallback instead of bridge
hub-mode if P1's own Q4/D5 fallback ever triggers — check `ageing_time 0` took effect on
`docker_fronthaul` first).

---

## p4-phy-l2-seam

### P4-G4 — full integration: injected UL → p2 pipeline → ring → l2-stub

**What's already verified locally (not deferred):** the ring library itself (`oi_seam.c`), the
producer's field-mapping (`oi_seam_producer.c`), and the L2 stub's validation core
(`oi_l2_validate.c`) — 80/80 real assertions across `struct_layout_test`/`producer_test`/
`ordering_test` (P4-G1)/`wrap_test` (P4-G2)/`restart_test` (P4-G3), all run against a real mmap'd
ring file and, for the concurrency gates, real threads. What's deferred is only the actual
end-to-end wiring: p3's real injected UL → the real p2 pipeline → this feature's producer called
at the real drain call site → the ring → a real running `l2-stub` container.

**This also depends on p3's own P3-I1 rig being up** (see this file's p3 section above) — bring
that up first.

```bash
cd ~/oi-rig/open_inline
export OI_SEAM_RING_PATH=/oi/seam/ring.bin OI_SEAM_RING_CAPACITY=64 OI_SEAM_TB_MAX_BYTES=3457 \
       OI_SEAM_FORMAT_VERSION=1 OI_SEAM_CONSUMER_STATE_PATH=/oi/seam/consumer_state.json
COMPOSE_PROJECT_NAME=oi_p4 docker compose \
  -f features/p0-rig-scaffold/docker/upstream/docker/docker-compose.yml \
  -f features/p0-rig-scaffold/docker/compose.sim.yml \
  -f features/p1-ran-baseline/docker/compose.p1.yml \
  -f features/p3-live-tap-ul-inject/docker/compose.p3.yml \
  -f features/p4-phy-l2-seam/docker/compose.p4.yml \
  up -d --force-recreate 5gc gnb ru-emu gpu-phy l2-stub
```

**Real, disclosed gap this run will surface (not silently papered over):** `gpu-phy`'s own event
loop does not yet actually call `oi_seam_producer_fill_slot`/`oi_seam_reserve`/`oi_seam_publish`
anywhere — this feature's LLD Q1 leaves "which process/thread owns the drain-and-publish call
site" open pending p2/p3 implementation details, and no session so far has wired gpu-phy's own
main loop to call p2's `oi_p2_drain` AND this feature's producer together. Before this gate can
produce real slots, that wiring needs to be added to gpu-phy's own image/entrypoint (a real, small,
additive integration step — not a design change to anything already built) as the actual first
step of this gate, not assumed already done.

**Expected pass criteria once that wiring exists:** run
`helpers/gate_p4_integration.sh` (invokes `l2_stub_main` directly, same binary the container
runs) with `OI_P4_MIN_SLOTS=1000` (mirrors P3-R11's own default) — exit 0, JSON
`{"processed": >=1000, "crc_ok": <matches p3's own known-good CRC rate for this run>,
"crc_fail": 0 or matching p3's own captured CRC-fail count if any, "order_violations": 0,
"epoch_resets": 0}` (a nonzero `epoch_resets` mid-run would mean gpu-phy restarted unexpectedly
during the gate — a real failure, not a benign event, unless gpu-phy was DELIBERATELY restarted as
part of this same session's P4-G3 verification).

---

## p5-one-command-rig

### P5-G2 — full real run (real p1–p4 suites) on WSL2 **and** GCP `n2-standard-16`, cross-compared

**What's already verified locally (not deferred):** the entire runner — discovery, schema
validation, overlay merge, `docker compose up`/`down` exactly once, per-gate timeout+capture,
PASS/FAIL/ERROR/BLOCKED/TIMEOUT classification, the unconditional rollup precedence, the
Markdown render, the rollup no-perf lint, and `compare_ledgers.sh` — all proven against
`tests/mock_suites/` with **real** `docker compose` (busybox services), real timeouts, real
teardown (P5-G1, 22/22 PASS, `tests/test_p5_g1.py`). All 9 real `pX-*/gates/suite.yml` manifests
(p1, p2a–p2f, p3, p4) were also written for real this session and validate cleanly via
`discover_suites.py` against the actual repo root (schema-valid, every declared script exists and
is executable, every declared `compose_overlays` path exists) — that is as far as local validation
can go without bringing the live rig up, which is explicitly out of scope here.

Every individual gate script referenced by those 9 manifests was also smoke-tested directly
(bypassing `docker compose` entirely) against this host's real state:
- `p1-check-sctp`, `p1-rigcfg-crosscheck`, `p1-assert-ecpri` (against the real archived corpus at
  `artifacts/p1/pcaps/20260725T180323Z/fronthaul.pcap`) all **PASS** for real, right now.
- `p1-soak-stability` correctly returns ERROR (`container ocudu_gnb not found — rig must be up
  first`) since no live rig is up — honest, not faked.
- Every p2a–p2f gate (`run-host-api`, `run-preparse`, `run-k1`..`run-k6`, `run-cb-segment`,
  `run-ldpc-decode`, and p2f's `pipeline_test.py`) **PASSes** for real (these need no live rig at
  all — pure host-side compute against real OCUDU libraries).
- p3's `p3-patch-apply-check`, `p3-osg-format-test`, `p3-osg-loader-crosscheck-test`,
  `p3-harness-calibrate-test`, `p3-pcap-comparator-test`, `p3-patch-schema-regression-test` all
  **PASS** for real (fully local per p3's own VERIFICATION.md); `p3-ingest-af-packet-test` also
  PASSes for real (real veth pair + real p1 corpus, no live rig needed).
- Every p4 gate (`run-struct-layout-test`, `run-producer-test`, `run-ordering-test`,
  `run-wrap-test`, `run-restart-test`) **PASSes** for real (pure local, no live rig needed).

**What's deferred is only actually invoking `make simtest` for real against these manifests**,
which brings the union of `compose.p1.yml` + `compose.p3.yml` + `compose.p4.yml` up atop the p0
base — the full 5GC+gNB+ru_emu+gpu-phy+l2-stub stack — and running `p1-soak-stability` (needs the
rig genuinely up and stable for 60+s) and any gate that needs real injected traffic flowing (p3's
live-tap gates already individually deferred in that feature's own section above; p4's P4-G4
likewise). Running `make simtest` locally today would correctly produce `overall: BLOCKED` or
`FAIL` for exactly those reasons — a real, honest result, not a tooling gap — but is not the DoD
gate (P5-G2 needs a real **PASS** with the rig genuinely exercised, on two hosts).

```bash
cd ~/oi-rig/open_inline  # WSL2 side: this repo, unchanged
cd features/p5-one-command-rig
make simtest OI_P5_RUN_ID=wsl2-$(date -u +%Y%m%dT%H%M%SZ)
# -> artifacts/p5/<run-id>/ledger.json + ledger.md

# GCP side (after starting oi-p1-rig and rsyncing this tree per this file's header prereqs):
cd ~/oi-rig/open_inline/features/p5-one-command-rig
make simtest OI_P5_RUN_ID=gcp-$(date -u +%Y%m%dT%H%M%SZ)

# Cross-host comparison (run wherever both ledger.json files are reachable):
bash helpers/compare_ledgers.sh \
  artifacts/p5/wsl2-<ts>/ledger.json artifacts/p5/gcp-<ts>/ledger.json
```

**Expected pass criteria:** both runs produce `ledger.overall: PASS` (every discovered gate
`PASS`, no phase `NOT_DISCOVERED` since all 9 manifests already exist), and
`compare_ledgers.sh` exits 0 (`PASS: compare_ledgers — ledgers agree on overall, pins/rigcfg
digests, and every gate status`). A `FAIL`/`BLOCKED` `overall` on either host, or any gate-status
disagreement between hosts, is a real P5-R9/DoD failure — not something to paper over by loosening
`compare_ledgers.sh`'s field-ignore list (host/timestamps/run_id only, by design).

**Known real gap this run will hit before it can pass, carried over from this file's own p3/p4
sections above:** `gpu-phy`'s event loop still doesn't call p4's producer at p2's drain call site
(p4-phy-l2-seam's LLD Q1, still open) and the ru_emulator's oracle-injection patch has never been
built into a real image and run (p3-live-tap-ul-inject's P3-U1/U2/I1). Both must be resolved first
(see this file's own p3/p4 sections for the exact steps) before `p1-soak-stability`,
`p3-ingest-af-packet-test`'s live-rig counterpart, and any p3/p4 integration-type gate can produce
anything but the honest ERROR/BLOCKED this session already recorded.

### Result (2026-07-27, WSL2 half) — real, better than the prediction above, GCP half still blocked

The paragraph above (written before this date) predicted `make simtest` would produce a real,
honest `BLOCKED`/`FAIL` locally. Run for real tonight, after the p3 oracle-injection patch WAS
built into a real image (see the "third session log" above) — **the actual result was
`overall: PASS`, genuinely, across all 29 real gates spanning all 9 phases**, including
`p1-soak-stability` (passes at its own suite-specced 60-second window — see
`p1-ran-baseline/VERIFICATION.md`'s new WSL2 finding for why this is real and not a fluke: a
LONGER, separately-run 600s soak the same night did fail with a real TX stall, so 60s genuinely
sits before whatever develops over the following minutes). p4-phy-l2-seam's LLD Q1 gap (noted
above) turned out not to block this run at all — `p4`'s own `gates/suite.yml` only exercises its
local unit-test suite (`struct-layout`/`producer`/`ordering`/`wrap`/`restart`), none of which need
`gpu-phy`'s drain call site wired up.

Getting to that real `PASS` required finding and fixing **three real, previously-latent bugs in
`simtest_runner.py`/`discover_suites.py` itself** — none of the mock-suite tests this feature's own
`test_p5_g1.py` already had could have caught them, because the mocks never needed real env vars, a
real base compose file, or a real teardown to succeed. Full account, citations, and the exact fix:
`p5-one-command-rig/VERIFICATION.md`'s own new section (same date). Short version: (1) no suite
manifest had any way to declare the real, hard-required compose env vars its own overlays need —
fixed by extending the schema to `oi-p5-suite/2` with an optional, generically-merged `compose_env`
map, not by hardcoding var names into the runner; (2) the runner's own "p0 base overlay" was
missing the upstream `docker-compose.yml` that actually defines `gnb`/`5gc`'s image/build blocks;
(3) `tear_down()` silently swallowed its own failure, so a run that printed `overall: PASS` still
left every container running afterward until caught by manually checking `docker ps -a`.

Real archived ledgers from this session, in order (first two hit the bugs above, in sequence; last
two are clean, both `overall: PASS`, confirming the fixes and the teardown fix respectively):
- `artifacts/p5/20260727T015521Z-f1315b6/` — hit bug (1), `compose up FAILED`.
- `artifacts/p5/20260727T020249Z-1f50768/` — bug (1) fixed, hit bug (2), `compose up FAILED`
  (different error: `service "gnb" has neither an image nor a build context specified`).
- `artifacts/p5/20260727T020450Z-11f5494/` — bugs (1)+(2) fixed (plus `oi/l2-stub:dev` built
  manually, plus `/tmp/p3_osg`'s 20 real oracle files generated), `overall: PASS`, all 29 gates —
  but bug (3) (silent teardown failure) meant containers were still up afterward; cleaned up
  manually, bug (3) then fixed in the runner itself.
- `artifacts/p5/20260727T021153Z-8a0b784/` — final, clean run: `overall: PASS`, all 29 gates,
  AND `docker ps -a` confirmed empty afterward (teardown fix verified end-to-end for real). **This
  is the WSL2-half ledger for the cross-comparison step below**, once a GCP-half ledger exists.

**GCP half + cross-comparison: still genuinely deferred, for a different reason than before** — not
because the rig isn't ready (it now genuinely is, on both the runner-correctness and p3-patch
fronts), but because the GCP VM (`oi-p1-rig`) was stopped at the end of the prior session tonight
(standard cost discipline) and this environment has no `gcloud` authentication to start it back up,
and direct SSH to its static IP timed out (consistent with it being fully powered off) — see the
"Decision 6" entry in the third session log above for the full account. **Action needed from the
user**: start `oi-p1-rig`, then run the exact GCP-side command block above
(`make simtest OI_P5_RUN_ID=gcp-<ts>`), then `compare_ledgers.sh` against
`artifacts/p5/20260727T021153Z-8a0b784/ledger.json`.
