# p1-ran-baseline — Verification Status

See [`spec/`](spec/) for the requirement/gate IDs referenced below. This file records what was
actually built and run (2026-07-23/24), including several real corrections to this feature's own
LLD that surfaced only once its assumptions were checked against the actual OCUDU source rather
than trusted as written — the same discipline this project has applied throughout p0/p2.

## What's implemented and verified for real

| Component | Verified how | Result |
|---|---|---|
| `docker/compose.p1.yml` (P1-R1/R2/R3) | Real `docker compose config` render, 3-layer stack ({p0 upstream}+{compose.sim.yml}+{this}), diffed programmatically against the base-alone render | `5gc` byte-identical; `gnb` unchanged outside its `networks` key, which gained exactly `{fronthaul}`; `ru-emu`/`5gc` membership correct — see `tests/ci_p1_static.sh` |
| `docker/configs/{gnb_ofh_testmode,ru_emu}.yml` (P1-R4/R5) | `rigcfg_crosscheck.sh` against the real files (MAC pairwise-swap, VLAN, bandwidth, eAxC ports, UL compression) + a negative test (perturbed field -> exit 1) | PASS + negative test correctly fails |
| `helpers/check_sctp.sh` (P1-R6) | Run for real on this host | Exits 3 with the exact documented `CONFIG_IP_SCTP` message (this host has no SCTP, same as p0's own finding) |
| `helpers/assert_ecpri.sh` classifier (P1-R8) | `tests/classifier_test.sh` against `tools/synth_ecpri_gen.cpp`'s real, self-consistent eCPRI+O-RAN CUS fixture (2 C-plane DL, 3 U-plane DL [1 VLAN-tagged], 2 U-plane UL, 0 C-plane UL) + a negative test (RU-only pcap) | **7/7 assertions pass** |
| `helpers/rigcfg_crosscheck.sh`, `helpers/lint_no_perf.sh`, `tests/ci_p1_static.sh` | Run for real on this host | All PASS (see below for the full sweep) |
| `helpers/kpi_snapshot.sh`, `helpers/soak_stability.sh`, `helpers/archive_pcap.sh` | Structurally exercised (container-not-found path, fixture-pcap archival) — NOT exercised against a live rig | See "Known-open items" — the parts that need a live SCTP-capable rig are explicitly flagged, not silently assumed working |
| `helpers/bring_up.sh` | Run for real on this host | Correctly exits 3 at the SCTP precondition, identical in shape to p0's `smoke_up.sh` |

## Real corrections found vs. this feature's own LLD

The LLD's normative shapes for the two rig config files and the `ru-emu` build were written before
being checked against the actual OCUDU source; three real inaccuracies surfaced during
implementation (same category of finding as p2's IQ-conversion-constant and K3-MMSE-formula
corrections — an LLD's best-guess normative shape isn't ground truth until verified):

1. **No `COMPONENT=ru-emulator` build arg exists.** The LLD assumed `ru-emu` needed a separate
   image built from `docker/Dockerfile` with `COMPONENT=ru-emulator`. Reading the actual
   Dockerfile shows ONE unconditional build stage that compiles and installs
   `ocu/odu/odu_split_8/odu_split_7_2/gnb/gnb_split_8/gnb_split_7_2/ru_emulator` into
   `/opt/ocudu/bin/` of the SAME image already built for `gnb` — there is no component-selection
   build arg anywhere in this file (the monolithic one D1 selects; a component-selecting Dockerfile
   may exist for the split-compose variant, irrelevant here). Fixed: `ru-emu` reuses
   `image: ocudu/gnb` with `command: ["/opt/ocudu/bin/ru_emulator", "-c", "/ru_emu_config.yml"]` —
   simpler and more correct than a redundant second build.
2. **`ru_emu.yml`'s real top-level shape is `log:` / `ru_emu: {cells: [...]}`, not a flat
   `ru_emu:` list.** Verified by reading `apps/examples/ofh/ru_emulator_appconfig.h` (the real
   `ru_emulator_appconfig` struct: `log_cfg`/`ru_cfg`(vector)/`dpdk_config`(optional)) and
   `ru_emulator_cli11_schema.cpp` (the real CLI11/YAML subcommand names: `log`, `ru_emu`, `dpdk`,
   with `ru_emu`'s per-instance config passed through a `--cells` option of repeated CLI11
   sub-blocks, each parsed via `create_yaml_config_parser()`). The real, working
   nested-list-of-maps YAML syntax for this exact mechanism is confirmed by
   `third_party/ocudu/configs/gnb_ru_picocom_scb_tdd_n78_20mhz.yml`, a genuine upstream example
   that exercises the SAME `--cells` pattern on the gNB side
   (`ru_ofh_config_cli11_schema.cpp`) and is presumably tested by upstream. Field names (`t2a_*`
   not `t1a_*`; `compr_method_ul`/`compr_bitwidth_ul` as the CLI/YAML keys, not
   `ul_compr_method`; `enable_promiscuous`; `prach_format`) are read directly off
   `ru_emulator_cli11_schema.cpp`'s `configure_cli11_ru_emu_args`, not approximated.
3. **Upstream's `gnb` build does not actually parameterize `OS_VERSION` via an env var default.**
   The LLD's Q5 claimed upstream's compose defaults `OS_VERSION` to `26.04` for `gnb`'s build and
   that this project pins it to `24.04` via `.env`. Reading the real
   `docker/docker-compose.yml` shows `gnb`'s build args hardcode `OS_VERSION: "24.04"` literally —
   there is no `${OS_VERSION}` template anywhere in this file for `gnb` to override. This
   coincidentally matches what the LLD wanted, but for the wrong reason (nothing to actually pin);
   the LLD's Q5 "upstream-inconsistency note" doesn't apply as stated. Not acted on further (no
   override needed where there is nothing to override); flagged here so a future reader doesn't
   go looking for a real `${OS_VERSION}` hook on the `gnb` service that doesn't exist.
4. **Real gNB OFH+TDD config needed more than the LLD's abbreviated `cell_cfg`.** The LLD's
   normative `cell_cfg` omitted `pci`, the full `prach` block (`prach_config_index`,
   `prach_root_sequence_index`, `zero_correlation_zone`, `prach_frequency_start`), and
   `tdd_ul_dl_cfg` — all of which a real TDD n78 cell needs and which the real upstream
   `gnb_ru_picocom_scb_tdd_n78_20mhz.yml` example (the closest real analogue: same band, same
   20MHz/30kHz shape, same split-7.2x OFH mechanism) already provides correct values for. Copied
   from that real, working reference rather than inventing plausible-looking values.

## Full re-verification sweep (2026-07-23/24, this host)

`tests/ci_p1_static.sh`: P1-R1 (rendered-config diff) PASS, P1-R2 (fronthaul network driver+MTU)
PASS, P1-R3 (network membership) PASS, P1-R5 (rigcfg_crosscheck + negative test) PASS, P1-R6
(real exit-3 with the exact message) PASS, P1-R8 (classifier vs. real fixture, 7/7) PASS, P1-R11
(zero project code) PASS, P1-R12 (lint_no_perf) PASS. All static/CI-buildable requirements this
host can exercise are green.

## First real local bring-up (2026-07-25) — P1-R8 achieved for real, 4 real bugs found+fixed

Before committing to a paid GCP VM, tried an actual `docker compose up` locally on this
(non-SCTP) host, bypassing only the SCTP precondition gate manually (not by editing
`bring_up.sh` — its refusal-without-SCTP behavior is correct and untouched). Every image
(`ocudu/gnb:latest`, `docker-5gc:latest`, `oi/gpu-phy:dev`, `oi/oracle:dev`) already existed from
an earlier session, so this cost no compute beyond the bring-up itself. Four real, previously
unexercised bugs surfaced, in order:

1. **Relative config path resolved against the wrong compose file's directory.**
   `configs.ru_emu_config.yml.file: ./configs/ru_emu.yml` in `compose.p1.yml` failed at `docker
   compose up` with "bind source path does not exist" — Compose resolves a layered override
   file's relative paths against the FIRST `-f` file's directory (the upstream compose file's),
   not the override file's own directory. Same class of bug as K1's kernel CWD-relative source
   paths (p2 VERIFICATION.md). Fixed: required an absolute path via `P1_RU_EMU_CONFIG_PATH`
   (`:?` required, no relative default — a default here would hit the identical problem);
   `bring_up.sh`/`ci_p1_static.sh` set it explicitly, mirroring how `GNB_CONFIG_PATH` already
   had to be absolute for the same reason.
2. **`ru_emulator` isn't at `/opt/ocudu/bin/` in the actually-built image.** The current
   `docker/Dockerfile` (read in the earlier grounding pass) explicitly `mv`s it there, but the
   already-built `ocudu/gnb:latest` image has it at `/usr/local/bin/ru_emulator` instead (image
   predates that reading, or was built via a different path) — confirmed via `docker run ...
   which gnb` also resolving to `/usr/local/bin/gnb`. Fixed: `ru-emu`'s `command` uses the bare
   name (`ru_emulator`, PATH-relative), matching how upstream's own `gnb` command already works.
3. **`ru-emu` needs `CAP_NET_ADMIN`, contradicting HLD D2's "no privileged, no cap_add" claim.**
   `ru_emulator` crashed on start: `Unable to set MTU size to '9000' bytes... current MTU size
   set to '9000' bytes` (i.e. it was already correct, but the tool tried to set it anyway).
   Traced to `lib/ofh/ethernet/ethernet_transmitter_impl.cpp`: the constructor unconditionally
   calls `ioctl(socket_fd, SIOCSIFMTU, ...)`, which always needs `CAP_NET_ADMIN` on Linux, and
   treats any failure as fatal. `gnb`'s upstream service never hit this because it's
   `privileged: true` (for USB/SDR access, unrelated) — `ru-emu` deliberately isn't. Fixed:
   added `cap_add: [NET_ADMIN]` to `ru-emu` only — still not `--privileged`, still no
   hugepages/DPDK, a minimal targeted grant.
4. **Q6 (container interface naming) is worse than "unconfirmed" — it's genuinely
   non-deterministic across recreations of the identical stack.** With `network_interface: eth1`
   (the LLD's original guess), zero eCPRI packets ever reached the fronthaul bridge despite gnb's
   MAC/PHY test-mode counters looking fully "live" (`docker exec ocudu_gnb ip -o link show`
   showed `eth0` actually carried the pinned fronthaul MAC, not `eth1` — gnb's OFH transmitter
   was binding to the *backhaul* network the whole time, silently). Fixing the static value to
   `eth0` and then simply recreating the SAME container (`--force-recreate`, no config change)
   flipped the mapping back to `eth1=fronthaul` — proving no static value is ever safe. Fixed for
   real: `gnb`'s `command` is now a shell wrapper that resolves the fronthaul interface by MAC
   address (`ip -o link show | grep <pinned MAC>`) at container start, patches a writable copy of
   the mounted (read-only) config, and execs the real `gnb` binary against it — see
   `compose.p1.yml`'s own comments for the full mechanism, including the further gotcha that
   Compose's own `${...}` interpolation will silently mangle a literal shell script unless every
   `$` is escaped as `$$` (found by the wrapper's first version being silently rewritten to use
   an empty `IFACE`, logged only as an easy-to-miss Compose warning).
   **P1-R1's own gate was extended to allow this**: `gnb`'s `command` key is now also an
   additive, documented override (SPEC.md P1-R1 amended with the rationale; `ci_p1_static.sh`
   updated to check for its presence, not treat it as an unexpected diff).

**Result after all four fixes**: brought up `5gc` (healthy), `gnb`, and `ru-emu` for real (0
restarts, all running). MAC/PHY test-mode traffic confirmed genuinely live (RNTI `0x44`,
continuously-updating PUSCH/PDSCH block counts) — though this is a du_high-internal MAC test-mode
PDU-injection stream (`lib/du/du_high/test_mode/mac_test_mode_helpers.cpp`), not by itself proof
anything reached the wire. Captured **real eCPRI traffic on the fronthaul bridge** (on the
individual veth, not the bridge master device — a Linux bridge doesn't reliably surface
port-to-port-forwarded traffic on its own master interface, a real capture-methodology finding
worth keeping for next time) and ran it through the **unmodified `assert_ecpri.sh`** (the exact
script `bring_up.sh`/P1-G2 would use):
```json
{"check": "ecpri", "frames": 495, "c_dl": 37, "u_dl": 373, "u_ul": 85, "c_ul": 0, "vlan_tagged": true}
```
**Exit 0 — all three required classes (c_dl, u_dl, u_ul) nonzero. P1-R8 achieved for real**, on
this host, before spending anything on GCP.

**Q1 (VLAN tagging) is now answered, not just handled:** the real captured frames ARE 802.1Q
VLAN-tagged (`vlan 1`, matching the fronthaul plan's `vlan_tag_cp`/`vlan_tag_up`/`vlan_tag: 1`),
confirming the ru_emulator `--vlan_tag` `CLI::Range(1,65536)` finding's prediction that the real
wire always carries a tag. The p2a/p2c VLAN-handling fix (this same day, see those features'
VERIFICATION.md) turned out to be validated against real captured traffic, not just synthetic
fixtures, within hours of landing.

**One genuinely new, real limitation found (not previously anticipated by the LLD's Honesty
notes at this severity): sustained transmission stalls under CPU contention on this shared
4-core host.** `gnb`'s OFH transmit thread bursts a large volume of real traffic immediately at
startup, then **fully stops** (not just "runs late" like `ru_emulator`'s own RX-side timing
warnings, which the LLD already anticipated) once real host CPU contention catches up with it —
confirmed by watching `/sys/class/net/eth0/statistics/tx_packets` freeze for 6+ consecutive
seconds. `gnb` alone measured 137% CPU on a 4-core host already shared with unrelated tenants.
This makes the *sustained* 30-second capture (P1-R8's literal window) and the 10-minute soak
(P1-R9) unachievable on THIS specific host as currently loaded — not a protocol/config problem,
a scheduling one. **This is now the one thing actually worth confirming on a dedicated GCP VM**:
whether `n2-standard-16`'s larger, non-shared CPU budget keeps the OFH TX thread running
continuously instead of stalling after the initial burst. Everything else this session
found/fixed transfers directly; only this specific question needs a bigger box to answer.

## GCP `n2-standard-16` deployment (2026-07-25) — P1-R7 and P1-R8 proven for real; ~10 more bugs

Deployed to a dedicated GCP VM (`n2-standard-16`, Ubuntu 24.04, `asia-south2-a`) via
`helpers/deploy_and_bring_up.sh`. This was the first time this project's actual deployment
tooling (not a manual bypass) ran end to end, and — consistent with every other "first real run"
in this project's history — it surfaced a long tail of real, previously unexercised bugs, each
fixed as found rather than worked around. In rough chronological order:

1. **`deploy_and_bring_up.sh` transferred no pre-built images**, so the first attempt silently
   triggered a full from-scratch OCUDU+UHD+DPDK+ROHC compile on the remote host even though
   already-built, already-verified images (`ocudu/gnb:latest`, `oi_p1-5gc:latest`) existed right
   here. Fixed: the script now checks (via a real `docker image inspect` probe on both ends, not
   assumed) whether each needed image already exists locally and on the remote, and streams
   exactly what's missing (`docker save | gzip | ssh | gunzip | docker load`) rather than ever
   silently falling back to a source rebuild for an image that's already built.
2. **`check_sctp.sh` had two real, independent bugs**, both invisible on every host this project
   had used before (all root shells) until a real non-root sudo-capable GCP user hit them: (a)
   plain `modprobe sctp` needs `CAP_SYS_MODULE`, which a non-root user doesn't have — the failure
   was silently swallowed by the script's own `2>/dev/null` and mis-reported as "module absent"
   rather than "needs privilege"; (b) `/proc/net/protocols` reports the protocol name as uppercase
   `SCTP`/`SCTPv6`, but the detection grep was `^sctp` (lowercase, case-sensitive) — a second,
   independent bug that also happened to be invisible everywhere else, since WSL2 genuinely lacks
   the module so both bugs failed the same way there. Fixed: retry via `sudo -n modprobe` when not
   root, and match case-insensitively. **This GCP VM's kernel has real SCTP support
   (`CONFIG_IP_SCTP=m`) — the earlier assumption that WSL2 was the only obstacle was itself
   wrong**; without both fixes, this VM would have looked just as SCTP-blocked as WSL2 despite
   genuinely having the module.
3. **gnb's real application log doesn't reach `docker logs` at all with the original
   `filename: /tmp/gnb.log` config** — copied from the upstream b200/picocom reference configs,
   which have no reason to care about container log visibility. `docker logs` only ever showed a
   completely unrelated periodic stdout-metrics table (from `metrics.autostart_stdout_metrics`).
   Fixed: `filename: stdout` (standard container practice) — the only way this project's
   log-based P1-R7 verification method can work at all.
4. **`all_level: warning` (also copied from the reference configs) filters out the NGAP
   connection-establishment lines P1-R7's whole check depends on**, at every severity below
   warning, everywhere, not just on one output stream. Fixed narrowly, not with a blanket
   `all_level: info`: `ocudulog` exposes NGAP as its own independently-configurable category
   (`cu_cp_unit_logger_config::ngap_level`, YAML key `ngap_level`, verified in
   `apps/units/o_cu_cp/cu_cp/cu_cp_unit_config_yaml_writer.cpp`) — setting only this one category
   to `info` gets exactly the needed evidence. (A first attempt DID set `all_level: info` globally
   and it worked once, manually — but at real traffic rates the OFH layer's own per-frame info
   logging produced such volume that `docker logs` became too slow to retrieve over SSH within a
   normal timeout, and risked the entries actually being dropped by ocudulog's own bounded async
   queue under load. Reverted to the narrow fix.)
5. **`ofh_level: warning` (the default) floods the log with "missed incoming User-Plane
   uplink/PRACH messages" — 32,648 of 32,753 total lines (99.7%) within the first ~60-90s of a
   fresh bring-up.** This reflects gnb's own OFH RX not seeing an incoming U-plane frame at the
   exact slot/symbol it expected — very likely the same class of independently-clocked-emulated-
   components timing mismatch this project's Honesty notes already anticipate for non-realtime
   SIM-tier hosts, NOT evidence that eCPRI traffic isn't reaching the wire (a real, filtered
   tcpdump capture — see below — proves real UL frames genuinely arrive, regardless of whether
   gnb's own slot counter considers them on-time). Fixed: `ofh_level: error`, documented as a
   scoped, explained suppression, not a silent one; the WHY of the underlying mismatch is
   flagged as still open, not chased further.
6. **`bring_up.sh` never recreated containers**, so a `docker compose up` against
   already-running containers (left over from manual debugging earlier in the same session) did
   nothing — meaning checks ran against an old, already-hours-old, warning-flooded log instead of
   a fresh startup. Fixed: `up -d --force-recreate` — also just the more correct semantics for
   "bring up the rig" (a known-clean start every time, not "whatever happens to be running").
7. **tcpdump on this image has no `cap_net_raw` capability set** — `getcap` on the binary
   returned nothing, and a bare non-root capture failed with `Operation not permitted`, contrary
   to the (wrong) assumption that Ubuntu's tcpdump package sets this automatically. Fixed:
   `remote_provision.sh` now `setcap cap_net_raw,cap_net_admin+eip` on the tcpdump binary
   explicitly (narrower than routing captures through sudo; works for any user).
8. **`soak_stability.sh` was missing its executable bit in the repo** — a genuine, pre-existing
   local bug (not an rsync artifact — `ls -la` confirmed `-rw-r--r--` locally too), invisible
   until now because this was the literal first time any run of this project's tooling had ever
   reached the point of actually invoking it (every earlier attempt failed earlier in the chain,
   on SCTP or one of the bugs above). Fixed: `chmod +x`.
9. **CORRECTED (see item 13 below): what looked like a "`docker logs` visibility race," first
   worked around with bounded retries, turned out to have a fully understood, deterministic root
   cause — a `pipefail`/`grep -q` SIGPIPE interaction, not buffering. See item 13 for the real fix;
   the retry loops added here were a real improvement at the time (correctly widened coverage to
   `soak_stability.sh`'s own independent check, not just `bring_up.sh`'s) but did not, on their
   own, fix the underlying bug — `soak_stability.sh`'s post-soak check kept failing on every one
   of 15 retries once gnb's log grew large enough, which is what led to finding the real cause.

10. **`ru_emu.yml`'s `log.level: info` flooded its own container log to 5.09GB in 26 minutes**
   (~3.3MB/sec at ~28K frames/sec sustained), vs gnb's 425KB in the same window — found when a
   plain `docker logs ocudu_ru_emu | wc -l` didn't return within 120s. Unlike gnb, `ru_emulator`
   has a single flat log level (no per-category `ngap_level`/`ofh_level` split), so `info` means
   per-frame OFH logging. Fixed: `level: warning`, matching the sane default already used
   elsewhere in this project.
11. **`kpi_snapshot.sh` had a dead, wasteful line**: `RU_LOG="$(docker logs "$RU_CONTAINER" ...)"`
   unconditionally read ru_emu's *entire* log — but the variable was never actually used
   (`ru_emu.eaxc` is hardcoded to `[]`, Q4 parsing isn't implemented). Combined with finding 10,
   this made every `kpi_snapshot.sh` call during a long soak take minutes for no reason. Removed.
12. **The REAL root cause of the earlier "`docker logs` visibility race"** (item 9, corrected):
   under this project's `set -uo pipefail`, `docker logs C | grep -q PATTERN` can return **141
   (SIGPIPE)** even when grep genuinely matches — `grep -q` exits the instant it finds a match,
   closing its read end; `docker logs` gets SIGPIPE'd before finishing writing; `pipefail` then
   reports that 141 as the pipeline's exit status instead of grep's real 0. Confirmed by direct
   reproduction on the GCP VM: `docker logs ocudu_gnb | grep -qiE '...'` → `pipeline_exit=141`
   while the match line was genuinely present. This is **deterministic once the log is large
   enough that the match line isn't near the tail** — not a timing flake, which is exactly why
   `soak_stability.sh`'s post-soak `NGAP_UP_N` check kept failing on every one of its 15 retries
   (gnb's log grows over a 10-minute soak; `NGAP_UP_0` at t=0, log still small, mostly got lucky).
   Fixed for real in `bring_up.sh` and `soak_stability.sh` (both `NGAP_UP_0` and `NGAP_UP_N`) and
   proactively in `p0-rig-scaffold/helpers/smoke_up.sh` (same pattern, never yet executed there
   since p0 has always been SCTP-blocked on every host used so far): capture the log into a
   variable via command substitution first (which fully drains its child before returning — no
   live pipe for grep to short-circuit), then grep the captured text with a here-string.
13. **Confirmation re-run after fixes 10-12 — CONFIRMED.** Real result:
   `{"check": "soak", "seconds": 600, "restarts": 0, "new_error_lines": 0,
   "counters_monotonic": null, "ngap_stable": true}`. `ngap_stable` flipped from `false` (every
   prior attempt) to `true` — direct confirmation the SIGPIPE fix in item 12 was the real cause,
   not a coincidence or a further retry-count effect.

14. **`counters_monotonic` root-caused for real (2026-07-25, same night, VM still warm) — chased
   immediately rather than deferred, since this investigation needed a live rig and this host was
   about to go cold.** Traced end-to-end through the real, pinned OCUDU source on the VM
   (`third_party/ocudu`, not guesswork):
   - `apps/gnb/gnb.cpp` creates a `remote_server` (`apps/services/remote_control/remote_server.cpp`)
     only if `remote_control.enabled` — a **WebSocket server** (built on uWebSockets), not a file,
     not stdout, not UDP. `metrics.enable_json: true` in YAML is bound to THREE different struct
     fields simultaneously (gnb's own top-level metrics subcommand, `du_high`'s
     `common_metrics_cfg.enable_json_metrics`, and `remote_control`'s own
     `enable_metrics_subscription`) — OCUDU's CLI11 helper layer merges multiple modules'
     options into one shared `metrics:` YAML section, confirmed by reading
     `du_high_config_cli11_schema.cpp:2579` and `remote_control_appconfig_cli11_schema.cpp`
     side by side.
   - Even with all of that enabled, a client must actively connect and send a
     `{"cmd": "metrics_subscribe"}` text frame before the server pushes anything
     (`remote_server_impl::subscribe_metrics_client()`) — this fully explains why the JSON metrics
     never appeared anywhere passive (stdout, log file, `docker logs`), regardless of `filename`
     or log-level settings: nothing was ever supposed to reach a passive sink.
   - **The real payload has no byte/throughput counters at all.** Read
     `apps/helpers/metrics/json_generators/du_high/mac.cpp` and
     `include/ocudu/mac/mac_metrics.h` directly: `mac_dl_cell_metric_report` carries only
     `average/min/max_latency_us` and `cpu_usage_percent` — no `ul_bytes`/`dl_bytes` field exists
     anywhere in the struct. RLC/PDCP (which normally carry SDU/PDU byte counts in a real stack)
     aren't even active in this MAC-test-mode config (traffic is injected directly at the MAC
     layer). So there was never a cumulative counter to extract from gnb's self-reported metrics,
     by ANY route — not a mapping gap, a real, confirmed absence.
15. **Real fix, not the originally-planned one**: switched `counters_monotonic`'s data source from
   gnb's (nonexistent) JSON byte counters to the fronthaul NIC's own kernel-level counters —
   `/sys/class/net/<iface>/statistics/{tx,rx}_bytes` inside the gnb container, read via `docker
   exec` using the exact same MAC-based interface resolution `compose.p1.yml`'s own command
   wrapper already uses (Q6). Genuinely monotonic (verified live: 50.4GB→53.8GB tx_bytes,
   13.0GB→13.9GB rx_bytes climbing during manual checks), zero new dependencies, and a more
   direct measure of "is the fronthaul link still moving data" than an internal latency stat ever
   was. `kpi_snapshot.sh`'s `mac_ul_bytes`/`mac_dl_bytes` fields are kept but explicitly `null`
   with a comment stating WHY (confirmed nonexistent, not unmapped) rather than removed outright,
   so the historical record of what was checked stays visible.
16. **Two more real bugs found while archiving a pcap manually** (per this investigation's own
   guardrail: capture one regardless of how the metrics hunt went, to decouple p3/p2f from it):
   - `deploy_and_bring_up.sh`'s pull-back step rsynced from `${TARGET}:~/oi-rig/artifacts/p1/` —
     but `archive_pcap.sh`'s real default `OUT_ROOT` is one directory deeper,
     `~/oi-rig/open_inline/artifacts/p1/`. The wrong path silently matched nothing on every prior
     run tonight; rsync's own "not fatal" fallback message (`no artifacts/p1/ produced on remote
     yet...`) masked it as a normal "soak didn't complete" case rather than a real bug. **P1-R10's
     pcap corpus was never actually pulled back into this repo by this script, on any run, until
     this fix.**
   - `archive_pcap.sh`'s size-based rotation used `tcpdump -w fronthaul_%03d.pcap -C "$MAX_MB"` —
     but tcpdump's `-C` rotation does not do printf-style substitution; it just appends a raw,
     non-zero-padded integer suffix to the exact filename given. The old code produced files
     literally named `fronthaul_%03d.pcap`, `fronthaul_%03d.pcap1`, `fronthaul_%03d.pcap2` (valid
     pcaps, confusingly named). Fixed to a plain `fronthaul.pcap` base name.
   - A pcap was manually archived under this run's fixes (`run_id=20260725T173051Z_manual`,
     852,702 frames) as an immediate, decoupled deliverable before either bug above was found or
     fixed — genuinely valid (verified: correct pcap magic number on all 3 fragments, manifest.json
     complete with real digests/counts/cell config), sitting at
     `artifacts/p1/pcaps/20260725T173051Z_manual/` if needed, though item 17's official run below
     supersedes it.
17. **Full re-confirmation after fixes 14-16 — FULLY GREEN, all four P1-R9 conditions real.**
   `{"check": "soak", "seconds": 600, "restarts": 0, "new_error_lines": 0,
   "counters_monotonic": true, "ngap_stable": true}`. `bring_up: PASS (P1-G1 + P1-G2 green,
   run-id=20260725T180323Z)`. P1-R10 pcap corpus (840,783 frames, 3 files, correctly named)
   archived on the remote AND correctly pulled back into this repo at
   `artifacts/p1/pcaps/20260725T180323Z/` — the pull-back path fix (item 16) verified working on
   this exact run.

**Final results, P1-G1 + P1-G2 both green:**
- **P1-R7 (NG Setup): PROVEN.** Real log evidence: `[NGAP    ] [I] Tx PDU: NGSetupRequest` /
  `[NGAP    ] [I] Rx PDU: NGSetupResponse` / CU-CP connected to AMF, PLMN 00101.
- **P1-R8 (eCPRI on the wire): PROVEN at real scale, repeatedly.** 30-second captures via the
  unmodified `assert_ecpri.sh` across five separate runs: 848,485 / 851,004 / 860,401 / 852,702 /
  840,783 frames (~28,000-29,000/sec sustained), `c_ul: 0` every time, no TX stall at all
  (confirming the earlier WSL2 stall was purely shared-host CPU contention, not a protocol issue —
  a dedicated VM resolves it completely).
- **P1-R9 (10-minute soak): ALL FOUR conditions PROVEN for real** — `restarts: 0`,
  `new_error_lines: 0`, `ngap_stable: true`, `counters_monotonic: true`, from the confirmation run
  in item 17. No condition weakened or worked around — the real data source was fixed (item 15),
  per this investigation's own explicit guardrail against silently accepting a `null`.
- **P1-R10 (pcap archival): PROVEN for real, end to end** — official run's corpus is in this repo
  at `artifacts/p1/pcaps/20260725T180323Z/`, correctly named, correctly counted, manifest complete.
- **P1-G1 + P1-G2: BOTH GREEN.** `bring_up.sh` and `deploy_and_bring_up.sh` both report `PASS`.

## WSL2 local attempt (2026-07-27) — real, more precise confirmation of the earlier WSL2-stall finding

An earlier session (item 17 above, 2026-07-25) already noted "the earlier WSL2 stall was purely
shared-host CPU contention, not a protocol issue — a dedicated VM resolves it completely," but
without much diagnostic detail (that finding predates the src-MAC BPF fix that later cut ingest CPU
load ~3x, which is why a fresh local attempt seemed worth trying again). Real GCP live-debugging
work later the same date (see `p3-live-tap-ul-inject/VERIFICATION.md`'s "Real bugs found and fixed"
items 11-12) fixed several real bugs unrelated to this host-level issue; a subsequent session
attempted the SIM-tier rig on this WSL2 host again, now with all of that fixed, to see if it fit.

**Real, more precise result this time**: `bring_up.sh`'s own full run (baseline images, no oracle
injection, no gpu-phy — the plainest version of this rig) passed P1-R7 (NG setup), P1-R8 (eCPRI,
30s capture, 829,232 real frames including 163,366 real UL), and the 60-second stability hold
cleanly — but the **10-minute soak (P1-R9) failed for real**: `counters_monotonic: false` (a real
reading, not a probe failure — both snapshots returned real numeric values that were simply
unchanged 10 minutes apart). Root-caused directly, not assumed: `ru-emu`'s own internal KPI table
(via `docker logs`) showed `TX_TOTAL`/`RX_TOTAL` stuck at `0`, sustained, with its timing worker
logging frequent "woke up late" warnings (500-4000+us late, 14-113 symbols late per event, several
per second). Manually re-read gnb's fronthaul sysfs counters twice, 10 seconds apart, while the
containers were still up: byte-for-byte identical both times, confirming this wasn't a stale-cache
or probe artifact — traffic had genuinely, currently stopped flowing.

**New, real, useful data point**: run the SAME rig's `p1-soak-stability` gate at a SHORTER 60-second
window (this feature's own `gates/suite.yml` spec, `--seconds 60`) instead of the standalone
script's 600-second default, and it **passes cleanly** — `counters_monotonic: true`, for real,
verified via the actual JSON output, not assumed from a green exit code. Run twice, both clean. This
means the stall isn't present from the very start of a run; it develops (or is triggered) sometime
after roughly a minute, consistent with a resource/scheduling-drift explanation rather than an
immediate hard failure — WSL2's virtualized scheduler apparently cannot sustain the sub-millisecond,
low-jitter wakeup guarantees `ru-emu`'s real-time timing thread needs for more than about a minute
under this host's real (4-core, ~8GB) constraints, even with the CPU-load-reducing fixes already
applied. Not fully proven (didn't have the time/access to test e.g. real-time process priority or
Windows-side WSL2 CPU pinning as a possible mitigation) — flagged as the leading, consistent-with-
prior-session explanation, not a certainty.

**Practical implication, not yet acted on**: this host CAN produce a real, honest, useful P5-G2
WSL2-half ledger for gates that don't need sustained multi-minute live traffic (see
`p5-one-command-rig/VERIFICATION.md` — every local unit-test gate across p1-p4 passed for real, and
even `p1-soak-stability` passed at its own suite-specced 60s window) — but the FULL, ≥1000-slot
P3-I1 gate specifically (which needs the rig to sustain real traffic far longer than a minute) is
not achievable on this host and needs the GCP VM, exactly as the earlier 2026-07-25 finding already
concluded. Full account, exact numbers, and the escalation-blocker note (GCP VM unreachable that
same night): `DEFERRED_LIVE_GATES.md`'s "second GCP session log" / third WSL2 session log.

## Known-open items (updated 2026-07-25, end of session)

- None remaining for P1-G1/P1-G2 — both fully green as of item 17 above. `kpi_snapshot.sh`'s
  `ru_emu.eaxc` KPI-table parse (the OTHER half of the original Q4) is still unimplemented (an
  honest `[]`, never fabricated) but is not gated by any P1 requirement — it was never blocking.
- **Q2 (compose per-endpoint `mac_address`)**: CONFIRMED working for real on both hosts (WSL2
  local bring-up and this GCP VM) — no `enable_promiscuous` fallback ever needed.
- **Q3 (UL C-plane presence)**: CONFIRMED on both hosts — `c_ul: 0` consistently, matching the
  LLD's own "observed-and-recorded only" framing.
- **Q6 (container interface naming)**: CONFIRMED and RESOLVED for real — fixed with MAC-based
  runtime resolution (finding from the WSL2 local bring-up), and the GCP run independently
  re-confirmed the underlying non-determinism (`eth0` this time, `eth1`/`eth2` on other runs of
  the identical stack) exists on GCP's networking stack too, not just WSL2's.
- **Q1 (VLAN tagging)**: now confirmed to genuinely vary by host/environment — WSL2's captured
  frames were 802.1Q-tagged, this GCP VM's are untagged. The dual-branch fix (p2a/p2c) already
  handles both; no further action needed, this is now a closed, understood question rather than
  a guess either way.
- **Q7 (gnb ERROR-log baseline)**: still open — `soak_stability.sh` conservatively treats ANY
  `ERROR` line as new (empty baseline); `new_error_lines: 0` held on the GCP run's full 10-minute
  soak regardless, so this hasn't yet been the blocking factor in practice, but the baseline
  capture itself still hasn't been done.
- **TX-stall-under-CPU-contention (WSL2 finding)**: CONFIRMED RESOLVED by a dedicated VM — no
  stall observed at all on `n2-standard-16` across a 30-second capture at ~28K frames/sec
  sustained. Was a real, shared-host scheduling artifact, not a protocol or code defect.
