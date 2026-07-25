/* ldpc_decode.cl — Layered min-sum LDPC decoder matching srsRAN's ldpc_decoder_generic.cpp
 *
 * Algorithm: layered (one CN row per "layer"), delta soft-bit update.
 * Mirrors srsRAN's update_variable_to_check / update_check_to_variable / update_soft_bits.
 *
 * Convention (3GPP TS 38.212 / srsRAN):
 *   CN[cn][r] connects to VN[vn][(r + shift) % Z]
 *   => v2c from VN[vn][j] to CN[cn][r]  where j = (r+shift)%Z
 *   => c2v from CN[cn][r] to VN[vn][j]  stored at c2v_buf[cn][vn][j] (VN-indexed)
 *
 * Soft update (delta): soft[vn][j] += c2v_new[cn][vn][j] - c2v_old[cn][vn][j]
 *   equivalent to srsRAN's  soft[j] = promotion_sum(c2v_new[j], v2c[j])
 *                          = c2v_new[j] + (soft_old[j] - c2v_old[j])
 *
 * Extra kernel argument: c2v_buf — __global char [n_cn * n_vn_full * Z], pre-zeroed by host.
 * Indexed: c2v_buf[(cn * n_vn_full + vn) * Z + j]  where j is the VN replica index.
 *
 * LLR representation: int8 in [-120, +120], ±127 = infinity (srsRAN convention).
 * Scale factor: 0.8 = 4/5 integer approximation (normalized min-sum).
 *
 * PoCL note: runs serial (local_size=1, get_local_id(0)=0 always).
 */

/* Compile-time constants injected via -D flags:
 *   -D NO_EDGE=0xffff  -D MAX_LS=384
 */

#define LLR_MAX   120
#define LLR_INF   127
#define SCALE_NUM 4
#define SCALE_DEN 5

/* Saturating add (srsRAN::log_likelihood_ratio::promotion_sum) */
inline char llr_add(char a, char b)
{
    if (a == (char)LLR_INF  || b == (char)LLR_INF)  return  (char)LLR_INF;
    if (a == (char)-LLR_INF || b == (char)-LLR_INF) return (char)-LLR_INF;
    short s = (short)a + (short)b;
    if (s >  LLR_MAX) return  (char)LLR_INF;   /* promote to infinity — matches srsRAN promotion_sum */
    if (s < -LLR_MAX) return (char)-LLR_INF;
    return (char)s;
}

/* Saturating subtract (srsRAN operator-) — plain arithmetic, saturate to ±LLR_MAX.
 * Matches srsRAN: no special-case for ±LLR_INF, so v2c = soft - c2v ≤ LLR_MAX=120
 * even when soft=LLR_INF.  This keeps v2c_abs < min1_init(=LLR_INF) in analysis. */
inline char llr_sub(char a, char b)
{
    short d = (short)a - (short)b;
    if (d >  LLR_MAX) return  (char)LLR_MAX;
    if (d < -LLR_MAX) return (char)-LLR_MAX;
    return (char)d;
}

inline char llr_abs(char a)    { return a < 0 ? -a : a; }
inline int  llr_isinf(char a)  { return llr_abs(a) == (char)LLR_INF; }

/* Scale LLR magnitude by 4/5, round half-up, preserve sign (normalized min-sum). */
inline char llr_scale(char a)
{
    if (llr_isinf(a)) return a;
    char mag    = llr_abs(a);
    char scaled = (char)(((short)mag * SCALE_NUM + SCALE_DEN / 2) / SCALE_DEN);
    return a < 0 ? -scaled : scaled;
}

/* ---- Kernel ---- */
__kernel void ldpc_decode(
    __global const char    *llr_input,   /* [n_vn_full * Z]  channel LLRs per CB      */
    __global       uchar   *bit_output,  /* [ceil(n_vn_info*Z/8)] packed bits per CB  */
    __constant const ushort *bg_shifts,  /* [n_cn * n_vn_full]  BG shift matrix       */
    __global       char    *c2v_buf,     /* [n_cn * n_vn_full * Z]  edge msgs, zeroed  */
    int n_vn_full,   /* 68 (BG1)  or 52 (BG2)  */
    int n_cn,        /* 46 (BG1)  or 42 (BG2)  */
    int n_vn_info,   /* 22 (BG1)  or 10 (BG2)  — info VNs written to output */
    int ls,          /* lifting size (e.g. 384)  */
    int n_iter,      /* number of layered iterations */
    int cb_offset    /* codeblock index into llr_input / bit_output / c2v_buf */
)
{
    const int Z = ls;

    /* Per-CB pointers */
    __global const char  *llr = llr_input  + (long)cb_offset * n_vn_full * Z;
    __global       uchar *out = bit_output + (long)cb_offset * ((n_vn_info * Z + 7) / 8);
    __global       char  *c2v = c2v_buf    + (long)cb_offset * n_cn * n_vn_full * Z;

    /* Soft bits in local memory: [n_vn_full][Z], max 68*384=26112 bytes */
    __local char soft[68 * MAX_LS];

    /* Load channel LLRs into soft */
    for (int vn = 0; vn < n_vn_full; vn++)
        for (int zi = 0; zi < Z; zi++)
            soft[vn * Z + zi] = llr[vn * Z + zi];

    /* Per-CN analysis scratch (private memory, stack) */
    char  min1 [MAX_LS];
    char  min2 [MAX_LS];
    int   midx [MAX_LS];
    uchar sprod[MAX_LS];

    /* ---- Layered min-sum iterations ---- */
    for (int iter = 0; iter < n_iter; iter++) {

        for (int cn = 0; cn < n_cn; cn++) {

            /* ---- Step A: Analysis at CN positions r ----
             * For each connected (vn, shift):
             *   rotated v2c at CN position r = soft[vn][(r+shift)%Z] - c2v_old[cn][vn][(r+shift)%Z]
             * min1[r]/min2[r] track the two smallest |rotated_v2c|, midx[r] the var-node index.
             * sprod[r] tracks the XOR of sign bits (0=positive, 1=negative).
             */
            for (int r = 0; r < Z; r++) {
                min1[r]  = (char)LLR_INF;
                min2[r]  = (char)LLR_INF;
                midx[r]  = -1;
                sprod[r] = 0;
            }

            int vni = 0;  /* sequential var-node index within this CN's adjacency list */
            for (int vn = 0; vn < n_vn_full; vn++) {
                ushort shift = bg_shifts[cn * n_vn_full + vn];
                if (shift == (ushort)NO_EDGE) continue;

                int c2v_base = (cn * n_vn_full + vn) * Z;

                for (int r = 0; r < Z; r++) {
                    int j = (r + (int)shift) % Z;          /* VN position from CN position r */
                    char c2v_old_j = c2v[c2v_base + j];
                    char v2c_rj   = llr_sub(soft[vn * Z + j], c2v_old_j);
                    char v2c_abs  = llr_abs(v2c_rj);

                    if (v2c_abs < min1[r]) {
                        min2[r] = min1[r];
                        min1[r] = v2c_abs;
                        midx[r] = vni;
                    } else if (v2c_abs < min2[r]) {
                        min2[r] = v2c_abs;
                    }
                    sprod[r] ^= (v2c_rj < 0) ? 1u : 0u;
                }
                vni++;
            }

            /* ---- Step B: Compute c2v and delta-update soft ----
             * For each connected (vn, shift), for each VN position j:
             *   CN position r = (j + Z - shift) % Z
             *   v2c from VN[vn][j] to CN[cn][r] = soft[vn][j] - c2v_old[cn][vn][j]
             *   c2v_new[cn][vn][j] = scale(min excluding vni) * sign_excluding_vni
             *   soft[vn][j] += c2v_new - c2v_old   (delta update)
             */
            vni = 0;
            for (int vn = 0; vn < n_vn_full; vn++) {
                ushort shift = bg_shifts[cn * n_vn_full + vn];
                if (shift == (ushort)NO_EDGE) continue;

                int c2v_base = (cn * n_vn_full + vn) * Z;

                for (int j = 0; j < Z; j++) {      /* j = VN replica index */
                    int r = (j + Z - (int)shift) % Z;   /* CN position connected to VN[vn][j] */

                    char c2v_old_j = c2v[c2v_base + j];
                    char v2c_j     = llr_sub(soft[vn * Z + j], c2v_old_j);

                    /* Magnitude: min excluding this var-node */
                    char mag = (vni != midx[r]) ? min1[r] : min2[r];
                    mag = llr_scale(mag);

                    /* Sign: product of all signs excluding this var-node */
                    uchar this_sign  = (v2c_j < 0) ? 1u : 0u;
                    uchar final_sign = sprod[r] ^ this_sign;
                    char  c2v_new_j  = final_sign ? -mag : mag;

                    /* Delta update: soft[vn][j] += c2v_new - c2v_old */
                    soft[vn * Z + j] = llr_add(soft[vn * Z + j],
                                               llr_sub(c2v_new_j, c2v_old_j));

                    /* Persist new c2v for next iteration */
                    c2v[c2v_base + j] = c2v_new_j;
                }
                vni++;
            }

        } /* end cn loop */
    } /* end iteration loop */

    /* ---- Hard decisions: first n_vn_info VNs → packed MSB-first bytes ---- */
    for (int vn = 0; vn < n_vn_info; vn++) {
        for (int zi = 0; zi < Z; zi++) {
            int   bit_idx  = vn * Z + zi;
            int   byte_idx = bit_idx / 8;
            int   bit_pos  = 7 - (bit_idx % 8);
            uchar bit      = (soft[vn * Z + zi] < 0) ? 1u : 0u;
            if (bit)
                out[byte_idx] |= (uchar)(1u << bit_pos);
            else
                out[byte_idx] &= ~(uchar)(1u << bit_pos);
        }
    }
}
