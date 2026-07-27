/* probe01_nic_ident.c -- P6-R1: day-1 NIC silicon + RDMA device identification.
 *
 * PHYSICAL tier, build-verified-only in this dev environment (no GPU/RDMA hardware here to run
 * against -- see VERIFICATION.md). Compiles against the REAL, installed libibverbs headers
 * (this host has libibverbs-dev + infiniband/verbs.h for real, confirmed via direct grep of the
 * installed header before writing this).
 *
 * Contract (LLD "Test plan" + probe CLI table):
 *   probe01_nic_ident [--pci-lines FILE]
 *     -> stdout: human-readable device list
 *     -> RESULT: MLX5_DEVICES=<N>
 *     -> RESULT: BROADCOM_ONLY=yes|no
 *   exit 0 iff >=1 mlx5 RDMA device found (P6-R1's PASS criterion); exit 1 otherwise.
 *
 * lspci itself isn't invoked from C (that half of R1 is meant to be an operator-run shell command
 * per the SPEC's literal `lspci | grep -iE 'mellanox|broadcom'` -- this program covers the
 * `ibv_devinfo`-equivalent half via libibverbs' real device-enumeration API, which is the part
 * that needs a compiled probe rather than a one-line shell pipeline).
 */
#include <infiniband/verbs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    int num_devices = 0;
    struct ibv_device **dev_list = ibv_get_device_list(&num_devices);

    if (!dev_list) {
        printf("ibv_get_device_list failed (errno set) -- no RDMA devices visible to userspace\n");
        printf("RESULT: MLX5_DEVICES=0\n");
        printf("RESULT: BROADCOM_ONLY=no\n");
        return 1;
    }

    int mlx5_count = 0;
    int other_count = 0;

    for (int i = 0; i < num_devices; i++) {
        const char *name = ibv_get_device_name(dev_list[i]);
        printf("device[%d]: name=%s\n", i, name ? name : "(null)");
        if (name && strncmp(name, "mlx5", 4) == 0) {
            mlx5_count++;
        } else {
            other_count++;
        }
    }

    printf("RESULT: MLX5_DEVICES=%d\n", mlx5_count);
    printf("RESULT: BROADCOM_ONLY=%s\n",
           (mlx5_count == 0 && other_count > 0) ? "yes" : "no");

    ibv_free_device_list(dev_list);

    /* PASS = >=1 mlx5 RDMA device found (SPEC P6-R1). */
    return (mlx5_count >= 1) ? 0 : 1;
}
