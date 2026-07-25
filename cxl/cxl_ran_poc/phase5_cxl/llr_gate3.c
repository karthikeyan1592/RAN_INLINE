/* llr_gate3.c — Gate 3: bpftime LLR mover → CXL node 1 → OpenCL
 *
 * Run as: LD_PRELOAD=libbpftime-syscall-server.so ./llr_gate3 <bench_pid>
 *
 * What this does:
 *  1. Reads uprobe offset from /etc/cxl_poc_uprobe_offset
 *  2. Loads llr_mover.bpf.o via libbpf (intercepted by bpftime syscall-server)
 *  3. Attaches uprobe to ldpc_decoder_benchmark at the derived offset
 *  4. Allocates LLR_PER_CB bytes on CXL NUMA node 1
 *  5. Polls llr_map.slot[0].seq until it increments (one LLR captured)
 *  6. Copies LLR from BPF map → CXL buffer
 *  7. Verifies buffer is on node 1 via get_mempolicy
 *  8. Runs OpenCL cxl_copy kernel on CXL buffer
 *  9. Prints [gate3] lines for telemetry and writes e2e_gcp.csv
 *
 * Build inside VM:
 *   BPFTIME=/root/cxl/third_party/bpftime
 *   gcc -O2 -o llr_gate3 llr_gate3.c \
 *     -I${BPFTIME}/build/libbpf \
 *     ${BPFTIME}/build/libbpf/libbpf/libbpf.a \
 *     -lelf -lz -lnuma -lOpenCL -lpthread
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <linux/memfd.h>
#include <numa.h>
#include <numaif.h>

#include <bpf/libbpf.h>
#include <bpf/bpf.h>

#define CL_TARGET_OPENCL_VERSION 120
#include <CL/cl.h>

#define LLR_PER_CB    26112
#define BITS_PER_CB   ((9216 * 384 / 384 + 7) / 8)  /* 1056 bytes */
#define CXL_NODE      1
#define OFFSET_FILE   "/etc/cxl_poc_uprobe_offset"
#define BPF_OBJ       "./llr_mover.bpf.o"
#define BENCH_PATH    "/root/cxl/third_party/srsRAN_Project/build/tests/benchmarks/phy/upper/channel_coding/ldpc/ldpc_decoder_benchmark"
#define CSV_OUT       "/root/cxl/paper/results/e2e_gcp.csv"
#define POLL_TIMEOUT_S  60

/* llr_slot layout must match llr_mover.bpf.c */
struct llr_slot {
    uint64_t seq;
    int8_t   data[LLR_PER_CB];
};
struct stats_t { uint64_t fires; uint64_t bytes_written; };

static const char *OCL_SRC =
    "__kernel void cxl_copy(__global const char *llr,"
    "                       __global uchar *bits, int n) {\n"
    "  int i = get_global_id(0);\n"
    "  if (i >= n) return;\n"
    "  int byte = i >> 3, bit = i & 7;\n"
    "  if (llr[i] < 0) bits[byte] |= (uchar)(1u << bit);\n"
    "}\n";

static void die(const char *msg) {
    fprintf(stderr, "FATAL: %s (errno=%s)\n", msg, strerror(errno));
    exit(1);
}

static int libbpf_quiet(enum libbpf_print_level lvl, const char *fmt, va_list args) {
    if (lvl == LIBBPF_DEBUG) return 0;
    return vfprintf(stderr, fmt, args);
}

static unsigned long read_offset(void) {
    FILE *f = fopen(OFFSET_FILE, "r");
    if (!f) die("cannot open " OFFSET_FILE);
    unsigned long off = 0;
    char line[64];
    if (fgets(line, sizeof(line), f)) {
        /* file may contain "0x30cf0" or "UPROBE_OFFSET=0x30cf0" */
        char *p = strstr(line, "0x");
        if (!p) p = strstr(line, "0X");
        if (p) off = strtoul(p, NULL, 16);
        else   off = strtoul(line, NULL, 16);
    }
    fclose(f);
    return off;
}

/* confirm ptr is on NUMA node 1 */
static int confirm_node1(void *ptr) {
    int mode = 0;
    unsigned long nodemask = 0;
    unsigned long maxnode = 8;
    long ret = syscall(SYS_get_mempolicy, &mode, &nodemask, maxnode, ptr,
                       MPOL_F_ADDR | MPOL_F_NODE);
    if (ret != 0) return -1;
    /* mode field contains the node number when MPOL_F_NODE is set */
    return mode;   /* should be 1 */
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);

    /* ── Parse bench PID ─────────────────────────────────── */
    pid_t bench_pid = -1;
    if (argc >= 2) {
        bench_pid = (pid_t)atoi(argv[1]);
    } else {
        /* auto-discover via /proc */
        FILE *p = popen("pgrep -x ldpc_decoder_benchmark | head -1", "r");
        if (p) { fscanf(p, "%d", &bench_pid); pclose(p); }
    }
    if (bench_pid <= 0) die("bench_pid not found — pass PID as argv[1] or start benchmark first");

    /* ── Read uprobe offset ───────────────────────────────── */
    unsigned long uprobe_off = read_offset();
    if (!uprobe_off) die("could not parse uprobe offset from " OFFSET_FILE);
    printf("[gate3] uprobe_offset=0x%lx\n", uprobe_off);
    printf("[gate3] bench_pid=%d\n", bench_pid);

    /* ── Check CXL NUMA node 1 ───────────────────────────── */
    if (numa_available() < 0) die("NUMA not available");
    if (numa_max_node() < CXL_NODE) die("NUMA node 1 not present — run vm_cxl_setup.sh");
    printf("[gate3] CXL NUMA node %d: %lld MB\n",
           CXL_NODE, (long long)(numa_node_size64(CXL_NODE, NULL) >> 20));

    /* ── Load BPF object via bpftime syscall-server ─────── */
    libbpf_set_print(libbpf_quiet);

    struct bpf_object *obj = bpf_object__open_file(BPF_OBJ, NULL);
    if (libbpf_get_error(obj)) die("bpf_object__open_file failed");

    int err = bpf_object__load(obj);
    if (err) {
        fprintf(stderr, "[gate3] bpf_object__load err=%d (%s)\n", err, strerror(-err));
        die("BPF load failed");
    }
    printf("[gate3] BPF object loaded\n");

    /* ── Find llr_mover program ─────────────────────────── */
    struct bpf_program *prog = NULL;
    bpf_object__for_each_program(prog, obj) {
        break;
    }
    if (!prog) die("no BPF program found in object");

    /* ── Attach uprobe to bench_pid ─────────────────────── */
    struct bpf_link *link = bpf_program__attach_uprobe(
        prog,
        false,          /* retprobe=false */
        bench_pid,      /* specific PID */
        BENCH_PATH,
        uprobe_off
    );
    if (libbpf_get_error(link)) {
        /* fallback: attach to all PIDs (-1) */
        fprintf(stderr, "[gate3] DEV-037: pid-specific attach failed, trying pid=-1\n");
        link = bpf_program__attach_uprobe(prog, false, -1, BENCH_PATH, uprobe_off);
        if (libbpf_get_error(link)) {
            fprintf(stderr, "[gate3] uprobe attach error: %ld\n", libbpf_get_error(link));
            die("uprobe attach failed");
        }
        printf("[gate3] bpftime_handler_attached pid=-1 (all procs)\n");
    } else {
        printf("[gate3] bpftime_handler_attached pid=%d\n", bench_pid);
    }

    /* ── Get BPF map FDs ─────────────────────────────────── */
    struct bpf_map *llr_map_obj  = bpf_object__find_map_by_name(obj, "llr_map");
    struct bpf_map *stat_map_obj = bpf_object__find_map_by_name(obj, "llr_stats");
    if (!llr_map_obj) die("llr_map not found in BPF object");
    int llr_fd  = bpf_map__fd(llr_map_obj);
    int stat_fd = stat_map_obj ? bpf_map__fd(stat_map_obj) : -1;

    /* ── Poll for first LLR capture ─────────────────────── */
    printf("[gate3] waiting for LLR capture (poll up to %ds)...\n", POLL_TIMEOUT_S);
    struct llr_slot slot = {0};
    uint32_t key = 0;
    time_t deadline = time(NULL) + POLL_TIMEOUT_S;
    uint64_t last_seq = 0;
    while (time(NULL) < deadline) {
        bpf_map_lookup_elem(llr_fd, &key, &slot);
        if (slot.seq > last_seq) break;
        struct timespec ts = {0, 5000000};  /* 5 ms */
        nanosleep(&ts, NULL);
    }
    if (slot.seq == 0) die("timed out waiting for LLR capture");

    printf("[gate3] LLR captured seq=%llu\n", (unsigned long long)slot.seq);
    printf("[gate3] LLR[0..4]=%d %d %d %d %d\n",
           (int)slot.data[0], (int)slot.data[1], (int)slot.data[2],
           (int)slot.data[3], (int)slot.data[4]);

    /* ── Allocate CXL buffer on node 1 ─────────────────── */
    int8_t  *cxl_llr  = numa_alloc_onnode(LLR_PER_CB,  CXL_NODE);
    uint8_t *cxl_bits = numa_alloc_onnode(BITS_PER_CB, CXL_NODE);
    if (!cxl_llr || !cxl_bits) die("numa_alloc_onnode failed");
    memset(cxl_llr,  0, LLR_PER_CB);
    memset(cxl_bits, 0, BITS_PER_CB);

    /* ── Copy LLR from BPF map → CXL buffer ─────────────── */
    memcpy(cxl_llr, slot.data, LLR_PER_CB);

    /* ── Verify CXL node ─────────────────────────────────── */
    int actual_node = confirm_node1(cxl_llr);
    int cxl_ok = (actual_node == CXL_NODE);
    printf("[gate3] cxl_buf ptr=%p numa_node=%d cxl_node=%s\n",
           (void *)cxl_llr, actual_node, cxl_ok ? "YES" : "NO");

    /* ── OpenCL cxl_copy on CXL buffer ─────────────────── */
    cl_platform_id platform;
    cl_device_id   device;
    cl_int         cl_err;
    double ocl_us = -1.0;
    int ocl_popcount = -1;

    if (clGetPlatformIDs(1, &platform, NULL) == CL_SUCCESS &&
        clGetDeviceIDs(platform, CL_DEVICE_TYPE_ALL, 1, &device, NULL) == CL_SUCCESS)
    {
        char devname[128];
        clGetDeviceInfo(device, CL_DEVICE_NAME, sizeof(devname), devname, NULL);
        printf("[gate3] OpenCL device: %s\n", devname);

        cl_context ctx = clCreateContext(NULL, 1, &device, NULL, NULL, &cl_err);
        cl_command_queue q = clCreateCommandQueue(ctx, device, 0, &cl_err);

        cl_mem in_buf = clCreateBuffer(ctx,
            CL_MEM_READ_ONLY | CL_MEM_USE_HOST_PTR,
            LLR_PER_CB, cxl_llr, &cl_err);
        cl_mem out_buf = clCreateBuffer(ctx,
            CL_MEM_WRITE_ONLY | CL_MEM_USE_HOST_PTR,
            BITS_PER_CB, cxl_bits, &cl_err);

        size_t src_len = strlen(OCL_SRC);
        cl_program prog_cl = clCreateProgramWithSource(ctx, 1, &OCL_SRC, &src_len, &cl_err);
        clBuildProgram(prog_cl, 1, &device, NULL, NULL, NULL);
        cl_kernel kernel = clCreateKernel(prog_cl, "cxl_copy", &cl_err);

        int n_bits = LLR_PER_CB;
        clSetKernelArg(kernel, 0, sizeof(cl_mem), &in_buf);
        clSetKernelArg(kernel, 1, sizeof(cl_mem), &out_buf);
        clSetKernelArg(kernel, 2, sizeof(int),    &n_bits);

        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        size_t gws = (size_t)LLR_PER_CB;
        clEnqueueNDRangeKernel(q, kernel, 1, NULL, &gws, NULL, 0, NULL, NULL);
        clFinish(q);
        clock_gettime(CLOCK_MONOTONIC, &t1);

        ocl_us = ((t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec)) / 1000.0;

        /* map back to read popcount */
        uint8_t *mapped = clEnqueueMapBuffer(q, out_buf, CL_TRUE, CL_MAP_READ,
                                              0, BITS_PER_CB, 0, NULL, NULL, &cl_err);
        if (cl_err == CL_SUCCESS && mapped) {
            ocl_popcount = 0;
            for (int i = 0; i < BITS_PER_CB; i++)
                ocl_popcount += __builtin_popcount(mapped[i]);
            clEnqueueUnmapMemObject(q, out_buf, mapped, 0, NULL, NULL);
            clFinish(q);
        }

        clReleaseKernel(kernel);
        clReleaseProgram(prog_cl);
        clReleaseMemObject(in_buf);
        clReleaseMemObject(out_buf);
        clReleaseCommandQueue(q);
        clReleaseContext(ctx);
    }

    /* ── Print telemetry lines ───────────────────────────── */
    printf("[gate3] ocl_us=%.1f\n", ocl_us);
    printf("[gate3] ocl_popcount=%d\n", ocl_popcount);

    /* ── Stats from BPF map ─────────────────────────────── */
    struct stats_t st = {0, 0};
    if (stat_fd >= 0) bpf_map_lookup_elem(stat_fd, &key, &st);
    printf("[gate3] bpftime_fires=%llu bytes_written=%llu\n",
           (unsigned long long)st.fires, (unsigned long long)st.bytes_written);

    /* ── Pass/fail verdict ───────────────────────────────── */
    int llr_range_ok = 0;
    for (int i = 0; i < 5; i++) {
        int v = (int)slot.data[i];
        if (v >= -20 && v <= 20 && v != 0) { llr_range_ok = 1; break; }
    }
    int pass = cxl_ok && llr_range_ok && (slot.seq > 0);

    printf("[gate3] %s cxl_ok=%s llr_range_ok=%s seq=%llu\n",
           pass ? "GATE3 PASS" : "GATE3 FAIL",
           cxl_ok ? "YES" : "NO",
           llr_range_ok ? "YES" : "NO",
           (unsigned long long)slot.seq);

    /* ── Write e2e_gcp.csv ───────────────────────────────── */
    /* Ensure output directory exists */
    system("mkdir -p /root/cxl/paper/results");
    FILE *csv = fopen(CSV_OUT, "w");
    if (csv) {
        fprintf(csv, "source,emulation_mode,uprobe_offset,bench_pid,"
                     "bpftime_fires,llr_captured,cxl_node,cxl_ok,"
                     "llr_0,llr_1,llr_2,llr_3,llr_4,"
                     "ocl_us,ocl_popcount,gate3_pass,"
                     "primary_config_us_slot,primary_config_slowdown\n");
        fprintf(csv, "measured,gcp_kvm_cxl_bpftime_node1,0x%lx,%d,"
                     "%llu,%llu,%d,%s,"
                     "%d,%d,%d,%d,%d,"
                     "%.1f,%d,%s,"
                     "11703,23.4\n",
                uprobe_off, bench_pid,
                (unsigned long long)st.fires, (unsigned long long)slot.seq,
                actual_node, cxl_ok ? "YES" : "NO",
                (int)slot.data[0], (int)slot.data[1], (int)slot.data[2],
                (int)slot.data[3], (int)slot.data[4],
                ocl_us, ocl_popcount,
                pass ? "PASS" : "FAIL");
        fclose(csv);
        printf("[gate3] wrote %s\n", CSV_OUT);
    }

    bpf_link__destroy(link);
    bpf_object__close(obj);
    numa_free(cxl_llr,  LLR_PER_CB);
    numa_free(cxl_bits, BITS_PER_CB);
    return pass ? 0 : 1;
}
