# p2e-k4

Implementation slice of [`p2-phy-kernels`](../p2-phy-kernels/spec/) — K4, the soft demapper. Its
own slice because its gate shape is unusual: float input, but a **bit-exact** (not tolerance-gated)
int8 output — the boundary where the pipeline's floating-point half becomes an integer-exact half.

## Scope

- **K4 — soft demapper → int8 LLR** (`src/kernels/k4_demapper.cl`), port of
  `demodulation_mapper_impl.{h,cpp}` dispatch + `demodulation_mapper_{qpsk,qam16,qam64}.cpp`
  **scalar paths only**, `log_likelihood_ratio::quantize()`. Per-RE LLR (Gauss approx), quantized
  to int8 with OCUDU's exact constants: `LLR_MAX=120`, `RANGE_LIMIT_FLOAT` = 24 (QPSK) / 20
  (16QAM/64QAM). **P2-R6**.
- `tests/k4_test.cpp`: the actual kernel (via PoCL) vs the real linked OCUDU `demodulation_mapper`
  — **17/17 real assertions pass, all bit-exact**, all three MCS (Qm 2/4/6), plus
  saturation-boundary, near-zero-symbol, and degenerate-noise_var vectors. One real portability
  bug found and fixed (OpenCL C rejects `__constant` arrays declared inside a non-kernel
  function — see `VERIFICATION.md`).

## Gates this slice owns

P2.4={R6} from the parent gate-mapping table — **and P2-R14/R14a for K4**, claimed explicitly:
both srsRAN AGPL vectors (CI-only per **R13**) and Sionna vectors (shippable) must be wired and
both must show bit-exact int8 output (K4 admits no tolerance column — LLD §7). **R14a**: apply the
same-lineage "verified applicable" check (DEV-043/HLD D8) to the srsRAN vector set before treating
any mismatch as a port bug; unset flag ⇒ WARN not CI-red. Landing K4 with only one oracle wired
counts as this slice incomplete.

## Depends on

`p2a-scaffold` (host API) only. Does **not** depend on `p2d-k2-k3` — K4's unit gate takes
oracle-supplied float inputs directly, per LLD §7's test-plan row for P2-R6.

## Note

K4's *pre-quantization float* LLR is explicitly not gated on its own (parent LLD §7: "not gated —
K4's gate is the bit-exact int8 output") — don't add a tolerance check for the intermediate float
value; only the final int8 output is asserted.
