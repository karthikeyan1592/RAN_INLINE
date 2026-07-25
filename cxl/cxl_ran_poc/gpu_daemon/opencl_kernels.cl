/* OpenCL kernels for CXL RAN PoC — PoCL CPU backend.
 * Swap PoCL ICD for AMD ROCm / NVIDIA OpenCL for real GPU — zero code changes.
 *
 * NOTE: Avoids OpenCL reserved identifiers: half, sign, complex, double.
 */

/* ── LDPC min-sum check-node update ──────────────────────────────────────────
 * Simplified 5G NR BG1 check-node step.
 * One work item per check node, processes a 4-variable-node window.
 * Demonstrates memory access pattern over CXL-backed LLR buffers.
 */
__kernel void ldpc_check_node_update(
    __global const float *llr_in,
    __global float       *llr_out,
    const int             n_var)
{
    int cn    = get_global_id(0);
    int istart = cn * 4;
    int iend   = istart + 4;

    if (istart >= n_var) return;
    if (iend   >  n_var) iend = n_var;

    float mmin1 = 1e9f, mmin2 = 1e9f;
    int   prod  = 1;    /* product of signs */

    int i;
    for (i = istart; i < iend; i++) {
        float val = llr_in[i];
        float mag = fabs(val);
        prod *= (val >= 0.0f) ? 1 : -1;
        if      (mag < mmin1) { mmin2 = mmin1; mmin1 = mag; }
        else if (mag < mmin2) { mmin2 = mag; }
    }

    for (i = istart; i < iend; i++) {
        float val  = llr_in[i];
        float mag  = (fabs(val) == mmin1) ? mmin2 : mmin1;
        int   sbit = prod * ((val >= 0.0f) ? 1 : -1);
        llr_out[i] = (float)sbit * mag * 0.75f;
    }
}

/* ── LDPC variable-node update ───────────────────────────────────────────────
 * Blends extrinsic CN messages into channel LLRs.
 */
__kernel void ldpc_variable_node_update(
    __global       float *llr,
    __global const float *channel_llr,
    const int             n_var)
{
    int vn = get_global_id(0);
    if (vn >= n_var) return;
    llr[vn] = llr[vn] * 0.5f + channel_llr[vn % n_var];
}

/* ── FFT butterfly (radix-2 Cooley-Tukey, in-place) ─────────────────────────
 * One stage of the DIT FFT per kernel invocation.
 * Demonstrates OFDM FFT memory access over CXL-backed complex float buffers.
 * NOTE: avoids 'half' (OpenCL built-in type) as variable name.
 */
__kernel void fft_butterfly(
    __global float2 *data,
    const int        N,
    const int        stage)
{
    int tid   = get_global_id(0);
    int hsize = N >> (stage + 1);   /* renamed from 'half' */

    if (tid >= hsize) return;

    int i0 = tid * 2;
    int i1 = i0 + 1;

    float ang = -2.0f * 3.14159265358979f * (float)(tid % hsize)
                / (float)(hsize * 2);
    float2 w  = (float2)(cos(ang), sin(ang));
    float2 a  = data[i0];
    float2 b  = data[i1];
    float2 wb = (float2)(w.x * b.x - w.y * b.y,
                          w.x * b.y + w.y * b.x);

    data[i0] = a + wb;
    data[i1] = a - wb;
}
