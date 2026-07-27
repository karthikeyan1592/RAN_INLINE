/* probe03_rawqp_hostmem.c -- P6-R3: raw-packet QP + flow-steering + same-function self-loopback
 * smoke test, host-RAM MR only (no dmabuf yet -- that's probe02/M1 step 3, not this probe).
 *
 * PHYSICAL tier, build-verified-only in this dev environment (no mlx5 RDMA hardware here to run
 * against -- see VERIFICATION.md). Written against the REAL, installed libibverbs/mlx5dv headers
 * (infiniband/verbs.h, infiniband/mlx5dv.h both confirmed present on this host via direct grep of
 * the installed headers before writing this file), using real struct layouts and real symbol
 * names (struct ibv_flow_spec_eth, struct mlx5dv_qp_init_attr,
 * MLX5DV_QP_CREATE_TIR_ALLOW_SELF_LOOPBACK_UC/_MC, mlx5dv_create_qp) -- not hand-declared shims.
 *
 * Contract (LLD "probe CLI" table):
 *   probe03_rawqp_hostmem --nic mlx5_0 --ethertype 0xAEFE
 *     -> RESULT: QP_CREATE=ok|fail
 *     -> RESULT: FLOW_RULE=ok|fail
 *     -> RESULT: LOOPBACK_RX=ok|fail
 *   exit 0 iff all three are ok (SPEC P6-R3's PASS criterion).
 *
 * On this dev host (no RDMA hardware), ibv_open_device / device lookup by name will fail --
 * expected, honestly reported as fail (not faked as ok), matching this feature's own
 * "compile-clean, not run" scope for this environment.
 */
#include <infiniband/verbs.h>
#include <infiniband/mlx5dv.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

static struct ibv_context *open_device_by_name(const char *name) {
    int num_devices = 0;
    struct ibv_device **dev_list = ibv_get_device_list(&num_devices);
    if (!dev_list) return NULL;

    struct ibv_context *ctx = NULL;
    for (int i = 0; i < num_devices; i++) {
        const char *dname = ibv_get_device_name(dev_list[i]);
        if (dname && strcmp(dname, name) == 0) {
            ctx = ibv_open_device(dev_list[i]);
            break;
        }
    }
    ibv_free_device_list(dev_list);
    return ctx;
}

int main(int argc, char **argv) {
    const char *nic = "mlx5_0";
    uint16_t ethertype = 0xAEFE;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--nic") == 0 && i + 1 < argc) {
            nic = argv[++i];
        } else if (strcmp(argv[i], "--ethertype") == 0 && i + 1 < argc) {
            ethertype = (uint16_t)strtoul(argv[++i], NULL, 0);
        }
    }

    int qp_create_ok = 0;
    int flow_rule_ok = 0;
    int loopback_rx_ok = 0;

    struct ibv_context *ctx = open_device_by_name(nic);
    if (!ctx) {
        fprintf(stderr, "probe03_rawqp_hostmem: could not open device '%s': %s\n",
                nic, strerror(errno));
        goto report;
    }

    struct ibv_pd *pd = ibv_alloc_pd(ctx);
    if (!pd) {
        fprintf(stderr, "probe03_rawqp_hostmem: ibv_alloc_pd failed: %s\n", strerror(errno));
        goto report;
    }

    struct ibv_cq *send_cq = ibv_create_cq(ctx, 16, NULL, NULL, 0);
    struct ibv_cq *recv_cq = ibv_create_cq(ctx, 16, NULL, NULL, 0);
    if (!send_cq || !recv_cq) {
        fprintf(stderr, "probe03_rawqp_hostmem: ibv_create_cq failed: %s\n", strerror(errno));
        goto report;
    }

    /* Host-RAM MR for the RX buffer (dmabuf is probe02/M1's concern, not this probe). */
    size_t rx_buf_len = 4096;
    void *rx_buf = malloc(rx_buf_len);
    struct ibv_mr *rx_mr = rx_buf ? ibv_reg_mr(pd, rx_buf, rx_buf_len,
                                               IBV_ACCESS_LOCAL_WRITE) : NULL;
    if (!rx_mr) {
        fprintf(stderr, "probe03_rawqp_hostmem: ibv_reg_mr (host RAM) failed: %s\n",
                strerror(errno));
        goto report;
    }

    /* Raw-packet QP with the real MLX5DV_QP_CREATE_TIR_ALLOW_SELF_LOOPBACK_UC/_MC creation
     * flags -- this is the actual mechanism under test (P6-R22's "no cable, no VF" claim rests
     * on this working, not on external wiring). */
    struct ibv_qp_init_attr_ex qp_attr_ex;
    memset(&qp_attr_ex, 0, sizeof(qp_attr_ex));
    qp_attr_ex.send_cq = send_cq;
    qp_attr_ex.recv_cq = recv_cq;
    qp_attr_ex.qp_type = IBV_QPT_RAW_PACKET;
    qp_attr_ex.cap.max_send_wr = 16;
    qp_attr_ex.cap.max_recv_wr = 16;
    qp_attr_ex.cap.max_send_sge = 1;
    qp_attr_ex.cap.max_recv_sge = 1;
    qp_attr_ex.comp_mask = IBV_QP_INIT_ATTR_PD | IBV_QP_INIT_ATTR_CREATE_FLAGS;
    qp_attr_ex.pd = pd;

    struct mlx5dv_qp_init_attr mlx5_qp_attr;
    memset(&mlx5_qp_attr, 0, sizeof(mlx5_qp_attr));
    mlx5_qp_attr.comp_mask = MLX5DV_QP_INIT_ATTR_MASK_QP_CREATE_FLAGS;
    mlx5_qp_attr.create_flags = MLX5DV_QP_CREATE_TIR_ALLOW_SELF_LOOPBACK_UC |
                                MLX5DV_QP_CREATE_TIR_ALLOW_SELF_LOOPBACK_MC;

    struct ibv_qp *qp = mlx5dv_create_qp(ctx, &qp_attr_ex, &mlx5_qp_attr);
    if (!qp) {
        fprintf(stderr, "probe03_rawqp_hostmem: mlx5dv_create_qp failed: %s\n", strerror(errno));
        goto report;
    }
    qp_create_ok = 1;

    /* Flow-steering rule: match the configured ethertype, deliver to this QP (P6-R18's config
     * surface, exercised here at the probe level). */
    struct {
        struct ibv_flow_attr attr;
        struct ibv_flow_spec_eth eth;
    } flow_rule;
    memset(&flow_rule, 0, sizeof(flow_rule));
    flow_rule.attr.type = IBV_FLOW_ATTR_NORMAL;
    flow_rule.attr.size = sizeof(flow_rule);
    flow_rule.attr.priority = 0;
    flow_rule.attr.num_of_specs = 1;
    flow_rule.attr.port = 1;
    flow_rule.eth.type = IBV_FLOW_SPEC_ETH;
    flow_rule.eth.size = sizeof(flow_rule.eth);
    flow_rule.eth.val.ether_type = htons(ethertype);
    flow_rule.eth.mask.ether_type = 0xFFFF;

    struct ibv_flow *flow = ibv_create_flow(qp, &flow_rule.attr);
    if (!flow) {
        fprintf(stderr, "probe03_rawqp_hostmem: ibv_create_flow failed: %s\n", strerror(errno));
        goto report;
    }
    flow_rule_ok = 1;

    /* Single self-loopback smoke frame: post one RX WQE, transition the QP through
     * RESET->INIT->RTR->RTS, post one SEND, poll for both completions. This probe only smoke-
     * tests the mechanism at n=1 (P6-R10/R12 do the real 1000-frame sustained version). */
    struct ibv_qp_attr mod_attr;
    memset(&mod_attr, 0, sizeof(mod_attr));
    mod_attr.qp_state = IBV_QPS_INIT;
    mod_attr.port_num = 1;
    if (ibv_modify_qp(qp, &mod_attr, IBV_QP_STATE | IBV_QP_PORT) != 0) {
        fprintf(stderr, "probe03_rawqp_hostmem: modify to INIT failed: %s\n", strerror(errno));
        goto report;
    }
    memset(&mod_attr, 0, sizeof(mod_attr));
    mod_attr.qp_state = IBV_QPS_RTR;
    if (ibv_modify_qp(qp, &mod_attr, IBV_QP_STATE) != 0) {
        fprintf(stderr, "probe03_rawqp_hostmem: modify to RTR failed: %s\n", strerror(errno));
        goto report;
    }
    memset(&mod_attr, 0, sizeof(mod_attr));
    mod_attr.qp_state = IBV_QPS_RTS;
    if (ibv_modify_qp(qp, &mod_attr, IBV_QP_STATE) != 0) {
        fprintf(stderr, "probe03_rawqp_hostmem: modify to RTS failed: %s\n", strerror(errno));
        goto report;
    }

    struct ibv_sge recv_sge = { .addr = (uintptr_t)rx_buf, .length = rx_buf_len, .lkey = rx_mr->lkey };
    struct ibv_recv_wr recv_wr = { .wr_id = 1, .sg_list = &recv_sge, .num_sge = 1 };
    struct ibv_recv_wr *bad_recv_wr = NULL;
    if (ibv_post_recv(qp, &recv_wr, &bad_recv_wr) != 0) {
        fprintf(stderr, "probe03_rawqp_hostmem: ibv_post_recv failed: %s\n", strerror(errno));
        goto report;
    }

    /* Minimal Ethernet + eCPRI-ethertype frame to self-send. */
    static uint8_t frame[64];
    memset(frame, 0, sizeof(frame));
    frame[12] = (uint8_t)(ethertype >> 8);
    frame[13] = (uint8_t)(ethertype & 0xFF);

    struct ibv_mr *tx_mr = ibv_reg_mr(pd, frame, sizeof(frame), IBV_ACCESS_LOCAL_WRITE);
    if (!tx_mr) {
        fprintf(stderr, "probe03_rawqp_hostmem: ibv_reg_mr (tx) failed: %s\n", strerror(errno));
        goto report;
    }
    struct ibv_sge send_sge = { .addr = (uintptr_t)frame, .length = sizeof(frame), .lkey = tx_mr->lkey };
    struct ibv_send_wr send_wr;
    memset(&send_wr, 0, sizeof(send_wr));
    send_wr.wr_id = 2;
    send_wr.sg_list = &send_sge;
    send_wr.num_sge = 1;
    send_wr.opcode = IBV_WR_SEND;
    send_wr.send_flags = IBV_SEND_SIGNALED;
    struct ibv_send_wr *bad_send_wr = NULL;
    if (ibv_post_send(qp, &send_wr, &bad_send_wr) != 0) {
        fprintf(stderr, "probe03_rawqp_hostmem: ibv_post_send failed: %s\n", strerror(errno));
        goto report;
    }

    struct ibv_wc wc;
    int recv_completed = 0;
    for (int poll_i = 0; poll_i < 1000000 && !recv_completed; poll_i++) {
        int n = ibv_poll_cq(recv_cq, 1, &wc);
        if (n > 0 && wc.wr_id == 1 && wc.status == IBV_WC_SUCCESS) {
            recv_completed = 1;
        }
    }
    loopback_rx_ok = recv_completed;

report:
    printf("RESULT: QP_CREATE=%s\n", qp_create_ok ? "ok" : "fail");
    printf("RESULT: FLOW_RULE=%s\n", flow_rule_ok ? "ok" : "fail");
    printf("RESULT: LOOPBACK_RX=%s\n", loopback_rx_ok ? "ok" : "fail");

    return (qp_create_ok && flow_rule_ok && loopback_rx_ok) ? 0 : 1;
}
