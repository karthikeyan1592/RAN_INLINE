/* _p2a_stub_stage.cl — p2a-scaffold's OWN placeholder kernel. NOT one of the six pipeline kernels
 * (leading underscore + non-"k*" name is deliberate: excludes it from provenance_check.py's and
 * lint_portability.py's k*.cl globs, and from any future "is this a real kernel" assumption).
 *
 * Stands in for K1..K6+LDPC while proving the host orchestration (HLD §6: one in-order queue,
 * explicit cl_event per launch, K1->K2->...->LDPC->readback as one dependency chain). Does the
 * simplest possible thing that still lets a test observe "stage N genuinely ran, in order, before
 * stage N+1 started": stamps every byte of its designated output buffer with stage_id. Content
 * correctness is explicitly out of scope here — that's k1_depacketizer.cl through
 * k6_rate_dematcher.cl (p2b-p2f), not this file.
 */
#include "oi_kernel_compat.h"

__kernel void p2a_stub_stage(__global uchar* out, uint n_bytes, uchar stage_id) {
  uint i = get_global_id(0);
  if (i < n_bytes) {
    out[i] = stage_id;
  }
}
