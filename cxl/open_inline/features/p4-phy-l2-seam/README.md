# p4-phy-l2-seam

The PHY↔L2 seam: a byte-precise shared-memory ring carrying decoded TB+CRC records from
`gpu-phy` (the GPU-resident PHY, `p2-phy-kernels`) to a minimal CPU **L2 stub** consumer, in a
separate container, over a wire format that's already compatible with the future Phase-2 CXL swap
without building Phase-2 now.

See `spec/{SPEC,HLD,LLD}.md` for the full design and `VERIFICATION.md` for what was actually built
and tested, including two real, disclosed findings: the LLD's own cited CXL PoC precedent
(`e2e_slot_t`) doesn't actually exist in the codebase, and dynamic race detectors (TSan, Helgrind)
can't verify this ring's release/acquire discipline on x86_64 (a real, well-understood tooling
limitation, not a bug — see that file for the full explanation and the static argument used
instead).

## Scope

- **`src/oi_seam_ring.h`** — the wire format itself: header, per-slot layout, atomic `status`
  field, epoch/sequence fields. Byte-precise, `_Static_assert`-checked at every compile.
- **`src/oi_seam.{h,c}`** — `oi_seam_open`/`reserve`/`publish`/`wait_status`/`release`/`epoch`,
  naming and shape deliberately parallel to the CXL PoC's own `desc_ring_t` API (a Phase-2 port is
  meant to be a rename-and-add-`mbind`, not a redesign).
- **`src/oi_seam_producer.{h,c}`** — the field-mapping producer helper (P4-R12): p2's opaque
  `oi_p2_tb_record_c` → this feature's `oi_seam_slot_t`, 1:1, no renaming.
- **`src/oi_l2_validate.{h,c}` + `src/l2_stub_main.c`** — the L2 stub: per-key `(sfn,slot)`
  ordering validation + CRC verdict counting. No SCF FAPI / packed-FAPI encoding (verified absent
  by grep).
- **`docker/compose.p4.yml` + `docker/Dockerfile.l2stub`** — additive `gpu-phy` volume mount, new
  `l2-stub` service, named/volume-persisted ring segment (not tmpfs, not `ipc: container:X`).
- **`helpers/gate_p4_{ordering,wrap,restart,integration}.sh`** — the four acceptance gates.

## Gates this slice owns

Traceable to `spec/SPEC.md`'s Acceptance gates (P4-G1..G4). P4-G1/G2/G3 run fully locally (no
OCUDU/OpenCL dependency at all — pure C11 + pthreads). P4-G4 needs the live rig; see
`../../DEFERRED_LIVE_GATES.md`'s p4 section for the exact command and pass criteria.

## Build

```bash
make all
make test
```
