/* oi_kernel_compat.h — the single permitted location for any vendor-specific macro (SIM §3).
 *
 * Every kernel in this pipeline (k1..k6) includes this header and nothing else vendor-specific.
 * MVP: empty of vendor branches (HLD §2) — PoCL and every PHYSICAL-tier vendor ICD (ROCm, CUDA,
 * Level Zero) all speak plain OpenCL C 1.2 for the operations this pipeline needs, so there is
 * nothing to abstract yet. If a future kernel genuinely needs a vendor-conditional path, it goes
 * here, guarded by a build-time macro — never a #ifdef __NVPTX__/__AMDGCN__ or a warp/wavefront
 * size query inside a kernel file itself (P2-R2, enforced by helpers/lint_portability.py).
 */
#ifndef OI_KERNEL_COMPAT_H
#define OI_KERNEL_COMPAT_H

/* Intentionally empty. */

#endif /* OI_KERNEL_COMPAT_H */
