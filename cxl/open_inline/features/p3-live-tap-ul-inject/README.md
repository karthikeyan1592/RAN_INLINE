# p3-live-tap-ul-inject

Turns P1's fronthaul from "real protocol, dummy data" into "real protocol, **known** data":
patches OCUDU's `ru_emulator` to inject externally-supplied oracle RE grids as the UL U-plane IQ
payload (byte-identical to the oracle files, verified on the wire), and gives `gpu-phy` a passive
af_packet tap that decodes the live eCPRI stream bit-exact against the oracle's known TBs, while
the unmodified DU (`gnb`) keeps consuming the same stream undisturbed.

See `spec/{SPEC,HLD,LLD}.md` for the full design and `VERIFICATION.md` for what was actually built
and tested, including every real bug found along the way.

## Scope

- **M1/M2 — `ru_emulator` oracle-injection patch** (`patches/0001-oracle-grid-ul-injection.patch`,
  `patches/files/ru_emulator_oracle_grid.{h,cpp}`): config-gated, default-off (absence = identical
  to upstream, P3-R4). Patch point: `generate_ul_uplane_messages()`, replacing the fixed
  random-IQ payload copy with a per-invocation copy selected by the deterministic
  `file_idx = (sfn*slots_per_frame + slot) mod N` schedule (P3-R6).
- **Shared packer** (`../p2f-integration/src/host/oi_oracle_pack.{h,cpp}`) — LLD Q1's resolution:
  one real-OCUDU-grounded "TB → RE grid → wire IQ bytes" library, two front ends (p2f's
  `oracle_tx_gen.cpp` for pcap frames, this feature's `osg_gen.cpp` for `.osg` files).
- **`.osg` file format** (`src/host/oi_osg_format.{h,cpp}`) — byte-precise per LLD §3.1, real
  zlib CRC32 trailer.
- **`osg_gen`** (`tools/osg_gen.cpp`) — generates exactly `slots_per_frame` (20) oracle files per
  MCS point, one per within-frame slot position — not an arbitrary count; see the tool's own
  header comment for the real DMRS-safety reason this constraint exists.
- **M3 — `gpu-phy` ingest_backend** (`src/host/oi_ingest_af_packet.{h,cpp}`): af_packet tap,
  two-branch VLAN-aware classic BPF filter, `PACKET_AUXDATA` handling, real counters
  (`frames_seen`/`ethertype_matched`/`parse_failed`/`delivered`/`feed_backpressure`/
  `socket_drops`), continuous multi-slot arena ring-buffer. Purely passive (P3-R8).
- **M4 — pcap comparator** (`tools/pcap_comparator.cpp`), **M5 — live bit-exact harness**
  (`tools/bit_exact_harness.cpp`), **M6 — DU-undisturbed checker**
  (`helpers/run_du_undisturbed_check.sh`, a thin pass-through to p1's own `soak_stability.sh`).
- **`docker/compose.p3.yml`** — additive compose layer: `ru-emu` switches to the patched image +
  oracle volume mount, `gpu-phy` joins `fronthaul` with `NET_RAW`.

## Gates this slice owns

Traceable to `spec/SPEC.md`'s Acceptance gates (P3-U1/U2/U3, P3-I1). Local-testing status and the
exact assertion counts are in `VERIFICATION.md`. Everything needing the live rig (no SCTP on this
host) is in `../../DEFERRED_LIVE_GATES.md`'s p3 section, with exact commands and pass criteria.

## Build

```bash
make bootstrap-ocudu   # once; reuses p2f-integration's shared OCUDU_BUILD
make all
make test
```
