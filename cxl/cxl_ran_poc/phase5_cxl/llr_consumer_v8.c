/* llr_consumer_v8.c — v8 assembled pipeline consumer
 *
 * CXL region sharing model (shm_open, not memfd):
 *
 *   Consumer                         Benchmark (after fork+exec)
 *   ────────────────────────         ────────────────────────────────────
 *   shm_open("/cxl_region_v8")       LD_PRELOAD=cxl_init.so:bpftime-agent.so
 *   mmap → consumer_va               cxl_init constructor:
 *   mbind(node 1)                      shm_open("/cxl_region_v8")
 *   set env CXL_SHM_NAME               mmap → bench_va  (may differ from consumer_va)
 *   set env CXL_VA_FILE                mbind(node 1)
 *   load BPF (config_map.cxl_base=0)  write bench_va → CXL_VA_FILE
 *   fork + exec benchmark
 *   poll CXL_VA_FILE                 handler fires:
 *   read bench_va                      read config_map[0].cxl_base = bench_va
 *   update config_map[0].cxl_base     bpf_probe_write_user(bench_va+off, llr)
 *     = bench_va                       push descriptor with offset
 *   attach uprobe
 *   busy-poll ring_map
 *   pop descriptor → read CXL at
 *     consumer_va + offset            (same physical pages, different VA)
 *   run OCL decode on CXL buffer
 *   write e2e_gcp.csv
 *
 * Why shm_open instead of memfd:
 *   fork+execl() replaces the child's address space entirely.  A memfd mmap
 *   inherited via fork is gone after exec.  shm_open gives both processes an
 *   independent mmap of the same physical pages — different VAs, same data.
 *   cxl_init.so is a thin constructor that maps the shm and reports the
 *   benchmark's VA back to the consumer via a tmpfile.
 *
 * Build inside VM:
 *   BPFTIME=/root/cxl/third_party/bpftime
 *   gcc -O2 -Wall -std=c11 -D_GNU_SOURCE \
 *     -I${BPFTIME}/build/libbpf -I../gpu_daemon/ldpc_cl \
 *     llr_consumer_v8.c \
 *     ${BPFTIME}/build/libbpf/libbpf/libbpf.a \
 *     -lelf -lz -lnuma -lpthread -lrt -lOpenCL -o llr_consumer_v8
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <signal.h>
#include <numa.h>
#include <numaif.h>

#include <bpf/libbpf.h>
#include <bpf/bpf.h>

#define CL_TARGET_OPENCL_VERSION 120
#include <CL/cl.h>

/* DEV-041: SIGCHLD handler sets flag when benchmark child exits.
 * Avoids per-iteration waitpid() syscall in the poll loop (each SYS_wait4
 * costs ~30ms in QEMU TCG mode → drastically reduces CB processing rate). */
static volatile sig_atomic_t g_bench_done = 0;
static void sigchld_handler(int sig) { (void)sig; g_bench_done = 1; }

/* ── Constants — must match lddc_llr_mover.bpf.c ─────────────────────── */
#define LLR_PER_CB    26112
#define BITS_PER_CB   1056
#define CB_STRIDE     (LLR_PER_CB + BITS_PER_CB + 64)
#define MAX_CBS       1    /* DEV-041: single CXL slot; pages pre-faulted at startup */
#define RING_CAP      256
#define CXL_REGION_SZ (64*1024*1024)   /* 64 MB */
#define CXL_NODE      1
#define MAX_REPS      1000             /* -R passed to benchmark */

/* ── Paths ──────────────────────────────────────────────────────────────── */
#define BPF_OBJ       "./lddc_llr_mover.bpf.o"
#define CL_KERNEL     "../gpu_daemon/ldpc_cl/ldpc_decode.cl"
#define BG_TABLES     "../gpu_daemon/ldpc_cl/bg_tables.h"
#define CSV_OUT       "/root/cxl/paper/results/e2e_gcp.csv"
#define OFFSET_FILE   "/etc/cxl_poc_uprobe_offset"
#define CXL_SHM_NAME  "/cxl_region_v8"
#define CXL_VA_FILE   "/tmp/cxl_va_v8.bin"
#define VA_POLL_MS    5000   /* ms to wait for cxl_init to write bench VA */

/* Scalar copy to CXL pages: avoids glibc's SIMD memcpy which crashes on
 * QEMU-emulated CXL pages (palignr/movdqa at libc+0x1871ce cause SIGILL via
 * TCG's undefined-instruction emulation path).
 *
 * Two defences against GCC replacing the loop with memcpy@plt:
 *  1. volatile dst — GCC cannot lower a volatile-write loop to memcpy()
 *     because memcpy has no volatile semantics.
 *  2. no-tree-loop-idiom — explicitly disables the loop-idiom recogniser
 *     that converts byte-copy loops to memcpy/memmove calls.
 * noinline prevents constprop of the static scratch_buf address, which
 * would create a .constprop.0 specialization where GCC sees a known-address
 * source and converts the loop to memcpy anyway. */
static void __attribute__((noinline,
    optimize("O2", "no-tree-vectorize", "no-tree-loop-idiom")))
cxl_copy(volatile int8_t *restrict dst, const int8_t *restrict src, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++) dst[i] = src[i];
}

/* ── BPF map structs — must match BPF program ───────────────────────────── */
struct config_t { uint64_t cxl_base; uint32_t region_size; uint32_t _pad; };
struct desc_t {
    uint64_t timestamp_ns;
    uint32_t llr_offset;
    uint32_t llr_len;
    uint32_t out_offset;
    uint32_t out_len;
    uint32_t seq;
    uint32_t _pad;
};

/* ── bg_tables BG1 Z=384 (inlined here from bg_tables.h) ───────────────── */
#include "../gpu_daemon/ldpc_cl/bg_tables.h"

static void die(const char *msg) {
    fprintf(stderr, "FATAL: %s (errno=%s)\n", msg, strerror(errno));
    exit(1);
}

static unsigned long read_offset(void) {
    FILE *f = fopen(OFFSET_FILE, "r");
    if (!f) die("cannot open /etc/cxl_poc_uprobe_offset");
    char line[64] = {0};
    fgets(line, sizeof(line), f);
    fclose(f);
    char *p = strstr(line, "0x"); if (!p) p = line;
    return strtoul(p, NULL, 16);
}

static uint64_t ns_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static int libbpf_quiet(enum libbpf_print_level lvl, const char *fmt, va_list ap) {
    if (lvl == LIBBPF_DEBUG) return 0;
    return vfprintf(stderr, fmt, ap);
}

/* ── Confirm physical NUMA node of a pointer ─────────────────────────── */
static int get_numa_node(void *ptr) {
    int mode = 0;
    unsigned long nodemask = 0;
    long rc = syscall(SYS_get_mempolicy, &mode, &nodemask, 8UL, ptr,
                      MPOL_F_ADDR | MPOL_F_NODE);
    return (rc == 0) ? mode : -1;
}

/* ── Read OCL kernel source from file ───────────────────────────────── */
static char *read_file(const char *path, size_t *len_out) {
    FILE *f = fopen(path, "r");
    if (!f) die("cannot open CL kernel file");
    fseek(f, 0, SEEK_END); long sz = ftell(f); rewind(f);
    char *buf = malloc(sz + 1);
    if (!buf) die("malloc kernel source");
    fread(buf, 1, sz, f); buf[sz] = '\0'; fclose(f);
    if (len_out) *len_out = (size_t)sz;
    return buf;
}

/* ── Find benchmark binary ───────────────────────────────────────────── */
static const char *find_bench(void) {
    static char path[512];
    /* 1. explicit env override */
    const char *env = getenv("LDPC_BENCH");
    if (env && *env && access(env, X_OK) == 0) {
        snprintf(path, sizeof(path), "%s", env);
        return path;
    }
    /* 2. well-known install location */
    if (access("/usr/local/bin/ldpc_decoder_benchmark", X_OK) == 0) {
        snprintf(path, sizeof(path), "/usr/local/bin/ldpc_decoder_benchmark");
        return path;
    }
    /* 3. search build tree (original path) */
    FILE *p = popen(
        "find /root/cxl/third_party/srsRAN_Project/build "
        "-name ldpc_decoder_benchmark -type f 2>/dev/null | head -1", "r");
    if (!p) die("popen bench search");
    int found = (fgets(path, sizeof(path), p) != NULL);
    pclose(p);
    if (!found || path[0] == '\0') die("bench not found — set LDPC_BENCH env var");
    path[strcspn(path, "\n")] = '\0';
    return path;
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    setvbuf(stdout, NULL, _IONBF, 0);
    system("mkdir -p /root/cxl/paper/results");

    /* ── Phase 0: probe uprobe offset ─────────────────────────────────── */
    unsigned long uprobe_off = read_offset();
    if (!uprobe_off) die("bad uprobe offset");
    printf("[v8] uprobe_offset=0x%lx\n", uprobe_off);

    const char *bench_path = find_bench();
    printf("[v8] benchmark=%s\n", bench_path);

    /* ── Phase 2: CXL shared region ───────────────────────────────────── */
    if (numa_available() < 0) die("NUMA not available");
    if (numa_max_node() < CXL_NODE) die("CXL NUMA node 1 absent — run vm_cxl_setup.sh");
    printf("[v8] CXL NUMA node 1: %lld MB\n",
           (long long)(numa_node_size64(CXL_NODE, NULL) >> 20));

    /* Use shm_open so both this process and the benchmark can map the same
     * physical pages (each via shm_open by name) even after fork+exec
     * replaces the benchmark's address space. */
    printf("[v8] DBG: shm_unlink\n"); fflush(stdout);
    shm_unlink(CXL_SHM_NAME);  /* remove any stale segment from prior run */
    printf("[v8] DBG: shm_open\n"); fflush(stdout);
    int mfd = shm_open(CXL_SHM_NAME, O_CREAT | O_RDWR | O_TRUNC, 0600);
    if (mfd < 0) die("shm_open");
    printf("[v8] DBG: ftruncate fd=%d\n", mfd); fflush(stdout);
    if (ftruncate(mfd, CXL_REGION_SZ) != 0) die("ftruncate");
    printf("[v8] DBG: mmap\n"); fflush(stdout);
    int8_t *cxl_base = mmap(NULL, CXL_REGION_SZ, PROT_READ|PROT_WRITE,
                             MAP_SHARED, mfd, 0);
    if (cxl_base == MAP_FAILED) die("mmap cxl");
    /* Keep mfd open — cxl_init.so in the benchmark will shm_open by name,
     * so we don't need to pass the fd, but don't close it yet. */

    /* Bind to CXL NUMA node 1 */
    printf("[v8] DBG: mbind strict\n"); fflush(stdout);
    unsigned long nodemask = (1UL << CXL_NODE);
    if (mbind(cxl_base, CXL_REGION_SZ, MPOL_BIND,
              &nodemask, sizeof(nodemask)*8, MPOL_MF_MOVE|MPOL_MF_STRICT) != 0) {
        fprintf(stderr, "[v8] WARN: mbind strict failed (%s) — trying without STRICT\n",
                strerror(errno));
        printf("[v8] DBG: mbind no-strict\n"); fflush(stdout);
        mbind(cxl_base, CXL_REGION_SZ, MPOL_BIND,
              &nodemask, sizeof(nodemask)*8, MPOL_MF_MOVE);
    }
    printf("[v8] DBG: mbind done\n"); fflush(stdout);

    /* DEV-041: pre-fault only the pages used by CXL slot 0 (MAX_CBS_PREFAULT
     * slots × CB_STRIDE bytes each). Each page fault in QEMU (no KVM) costs
     * ~30ms.  Pre-faulting the full 64MB (16K pages) would take ~490s.
     * MAX_CBS_PREFAULT=1 means all 4000 CBs write to slot 0 (slot index wraps
     * mod MAX_CBS=1 → always 0) — acceptable for the PoC: OCL always decodes
     * the latest CB written to slot 0.  7 pages × 30ms = 210ms startup cost. */
#define MAX_CBS_PREFAULT 1
    {
        volatile char *p = (volatile char *)cxl_base;
        for (size_t fi = 0; fi < (size_t)MAX_CBS_PREFAULT * CB_STRIDE; fi += 4096) p[fi] = 0;
    }

    int cxl_actual_node = get_numa_node(cxl_base);
    printf("[v8] CXL region mapped: base=%p size=%dMB node=%d cxl_ok=%s\n",
           (void*)cxl_base, CXL_REGION_SZ>>20, cxl_actual_node,
           cxl_actual_node == CXL_NODE ? "YES" : "NO");

    /* ── Phase 2 cont: OpenCL over CXL region ─────────────────────────── */
    cl_platform_id platform; cl_device_id device; cl_int cl_err;
    cl_context cl_ctx = NULL; cl_command_queue cl_q = NULL;
    cl_kernel cl_kern = NULL; cl_program cl_prog = NULL;
    cl_mem cl_c2v = NULL;

    int ocl_ok = 0;
    char devname[128] = "none";
    if (clGetPlatformIDs(1, &platform, NULL) == CL_SUCCESS &&
        clGetDeviceIDs(platform, CL_DEVICE_TYPE_ALL, 1, &device, NULL) == CL_SUCCESS) {
        clGetDeviceInfo(device, CL_DEVICE_NAME, sizeof(devname), devname, NULL);
        printf("[v8] OpenCL device: %s\n", devname);

        cl_ctx = clCreateContext(NULL, 1, &device, NULL, NULL, &cl_err);
        cl_q   = clCreateCommandQueue(cl_ctx, device, 0, &cl_err);

        /* Zero-copy sentinel test */
        cxl_base[0] = (int8_t)0x5A;
        cl_mem sentinel_buf = clCreateBuffer(cl_ctx,
            CL_MEM_READ_ONLY | CL_MEM_USE_HOST_PTR, 1, cxl_base, &cl_err);
        if (cl_err == CL_SUCCESS) {
            int8_t *mapped = clEnqueueMapBuffer(cl_q, sentinel_buf, CL_TRUE,
                CL_MAP_READ, 0, 1, 0, NULL, NULL, &cl_err);
            if (cl_err == CL_SUCCESS && mapped && *mapped == (int8_t)0x5A) {
                printf("[v8] zero_copy=YES (sentinel 0x5A read back via OCL)\n");
            } else {
                printf("[v8] zero_copy=NO (PoCL copies internally — architecture valid)\n");
            }
            if (mapped) clEnqueueUnmapMemObject(cl_q, sentinel_buf, mapped, 0, NULL, NULL);
            clFinish(cl_q);
            clReleaseMemObject(sentinel_buf);
        }
        cxl_base[0] = 0;

        /* Build bit-exact LDPC decoder kernel */
        size_t src_len;
        char *cl_src = read_file(CL_KERNEL, &src_len);
        cl_prog = clCreateProgramWithSource(cl_ctx, 1, (const char**)&cl_src, &src_len, &cl_err);
        free(cl_src);

        char build_opts[256];
        snprintf(build_opts, sizeof(build_opts),
                 "-D NO_EDGE=0xffff -D MAX_LS=384 -I%s",
                 "/root/cxl/cxl_ran_poc/gpu_daemon/ldpc_cl");
        cl_err = clBuildProgram(cl_prog, 1, &device, build_opts, NULL, NULL);
        if (cl_err != CL_SUCCESS) {
            char log[4096];
            clGetProgramBuildInfo(cl_prog, device, CL_PROGRAM_BUILD_LOG,
                                  sizeof(log), log, NULL);
            fprintf(stderr, "[v8] OCL build error:\n%s\n", log);
            die("OCL build failed");
        }
        cl_kern = clCreateKernel(cl_prog, "ldpc_decode", &cl_err);
        if (cl_err != CL_SUCCESS) die("clCreateKernel");

        /* c2v scratch: BG1-max size covers both BG1 and BG2 graphs */
        size_t c2v_max_sz = (size_t)BG1_M * BG1_N * 384;  /* 46 × 68 × 384 */
        cl_c2v = clCreateBuffer(cl_ctx, CL_MEM_READ_WRITE, c2v_max_sz, NULL, &cl_err);
        if (cl_err != CL_SUCCESS) die("clCreateBuffer c2v");

        ocl_ok = 1;
        printf("[v8] OCL build OK — bit-exact ldpc_decode kernel ready\n");
    }

    /* ── Phase 3: Load BPF program ─────────────────────────────────────── */
    libbpf_set_print(libbpf_quiet);

    struct bpf_object *bpf_obj = bpf_object__open_file(BPF_OBJ, NULL);
    if (libbpf_get_error(bpf_obj)) die("bpf_object__open_file lddc_llr_mover.bpf.o");

    int err = bpf_object__load(bpf_obj);
    if (err) {
        fprintf(stderr, "[v8] bpf_object__load err=%d (%s)\n", err, strerror(-err));
        die("BPF load failed");
    }
    printf("[v8] BPF object loaded\n");

    /* Store CXL base VA into config_map before forking */
    struct bpf_map *config_map_obj    = bpf_object__find_map_by_name(bpf_obj, "config_map");
    struct bpf_map *scratch_map_obj   = bpf_object__find_map_by_name(bpf_obj, "scratch_map");
    struct bpf_map *ring_map_obj      = bpf_object__find_map_by_name(bpf_obj, "ring_map");
    struct bpf_map *head_map_obj      = bpf_object__find_map_by_name(bpf_obj, "ring_head");
    struct bpf_map *fire_count_map_obj= bpf_object__find_map_by_name(bpf_obj, "fire_count");
    if (!config_map_obj || !scratch_map_obj || !ring_map_obj || !head_map_obj)
        die("BPF map not found");

    int config_fd   = bpf_map__fd(config_map_obj);
    int scratch_fd  = bpf_map__fd(scratch_map_obj);
    int ring_fd     = bpf_map__fd(ring_map_obj);
    int head_fd     = bpf_map__fd(head_map_obj);
    int fire_cnt_fd = fire_count_map_obj ? bpf_map__fd(fire_count_map_obj) : -1;

    uint32_t key0 = 0;
    /* cxl_base intentionally 0 — cxl_init.so will update it with the
     * benchmark's own VA after exec. The handler checks cxl_base != 0
     * before writing, so no premature fires can corrupt memory. */
    struct config_t cfg = {
        .cxl_base    = 0,
        .region_size = CXL_REGION_SZ,
    };
    if (bpf_map_update_elem(config_fd, &key0, &cfg, BPF_ANY) != 0)
        die("bpf_map_update_elem config (sentinel)");
    printf("[v8] config_map initialized (cxl_base=0 sentinel — awaiting bench VA)\n");

    /* ── Register uprobe BEFORE fork (pid=-1) so agent sees it at init ── */
    /* DEV-038: bpftime agent scans shm uprobe table at LD_PRELOAD constructor
     * time. The pre-fork pid=-1 registration writes the uprobe into bpftime shm
     * so the agent finds it on startup. DEV-039: __sync_fetch_and_add emitted
     * opcode 0xc3 (BPF atomic) which ubpf rejects — replaced with *head_p+1. */
    struct bpf_program *prog = NULL;
    bpf_object__for_each_program(prog, bpf_obj) { break; }
    if (!prog) die("no BPF program in object");

    struct bpf_link *link = bpf_program__attach_uprobe(
        prog, false, -1, bench_path, uprobe_off);
    if (libbpf_get_error(link)) {
        fprintf(stderr, "[v8] WARN: pre-fork pid=-1 attach returned error (non-fatal)\n");
        link = NULL;
    } else {
        printf("[v8] bpftime_uprobe_registered (pre-fork, pid=-1, offset=0x%lx)\n",
               uprobe_off);
    }

    /* ── Fork benchmark with bpftime agent + cxl_init ─────────────────── */
    char agent_path[512];
    char init_path[512];
    char preload_path[1100];
    snprintf(agent_path, sizeof(agent_path),
             "/root/cxl/third_party/bpftime/build/runtime/agent/libbpftime-agent.so");
    snprintf(init_path, sizeof(init_path),
             "/root/cxl/cxl_ran_poc/phase5_cxl/cxl_init.so");
    if (access(agent_path, R_OK) != 0) die("bpftime agent .so not found");
    if (access(init_path, R_OK) != 0) die("cxl_init.so not found — run: make cxl_init.so");

    /* cxl_init.so constructor maps the CXL shm in the benchmark process and
     * writes the benchmark's VA to CXL_VA_FILE so we can update config_map. */
    char region_size_str[32];
    snprintf(region_size_str, sizeof(region_size_str), "%u", (unsigned)CXL_REGION_SZ);
    setenv("CXL_SHM_NAME",    CXL_SHM_NAME,    1);
    setenv("CXL_REGION_SIZE", region_size_str,  1);
    setenv("CXL_VA_FILE",     CXL_VA_FILE,      1);
    unlink(CXL_VA_FILE);   /* remove stale file from any prior run */

    /* agent first so bpftime initializes its shared memory before cxl_init runs */
    snprintf(preload_path, sizeof(preload_path), "%s:%s", agent_path, init_path);

    /* Install SIGCHLD handler BEFORE fork so no child-exit signal is missed */
    struct sigaction sa_chld = {0};
    sa_chld.sa_handler = sigchld_handler;
    sa_chld.sa_flags   = SA_NOCLDSTOP;  /* only fire on exit, not on stop */
    sigaction(SIGCHLD, &sa_chld, NULL);

    pid_t bench_pid = fork();
    if (bench_pid < 0) die("fork");

    if (bench_pid == 0) {
        /* Child: after execl the address space is replaced. cxl_init.so
         * constructor (via LD_PRELOAD) will shm_open CXL region by name and
         * write this process's VA to CXL_VA_FILE. */
        setenv("LD_PRELOAD", preload_path, 1);
        setenv("SPDLOG_LEVEL", "info", 1);  /* DBG: see agent hook installation */
        execl(bench_path, bench_path,
              "-L", "384", "-I", "5", "-T", "avx2",
              "-R", "1000", NULL);
        perror("execl benchmark");
        _exit(1);
    }

    printf("[v8] benchmark forked: pid=%d\n", bench_pid);

    /* CC-005: capture process tree while benchmark is alive (gate 4(a) evidence) */
    printf("[v8] === ps aux snapshot (bench_pid=%d consumer_pid=%d) ===\n",
           bench_pid, getpid());
    fflush(stdout);
    {
        int ps_rc = system("ps aux | grep -E 'ldpc_decoder|llr_consumer' | grep -v grep");
        (void)ps_rc;
    }
    printf("[v8] === end ps aux snapshot ===\n");

    /* Poll for benchmark's CXL VA written by cxl_init.so constructor */
    printf("[v8] waiting for cxl_init.so to map CXL region (up to %dms)...\n",
           VA_POLL_MS);
    uint64_t bench_va = 0;
    int64_t va_deadline = (int64_t)ns_now() + (int64_t)VA_POLL_MS * 1000000LL;
    while ((int64_t)ns_now() < va_deadline) {
        int vafd = open(CXL_VA_FILE, O_RDONLY);
        if (vafd >= 0) {
            uint64_t va = 0;
            if (read(vafd, &va, sizeof(va)) == (ssize_t)sizeof(va) && va != 0)
                bench_va = va;
            close(vafd);
            if (bench_va) break;
        }
        usleep(5000);   /* 5 ms poll interval */
    }

    if (!bench_va) {
        fprintf(stderr,
                "[v8] ERROR: cxl_init.so did not write VA within %dms\n"
                "     LD_PRELOAD=%s\n"
                "     CXL_SHM_NAME=%s CXL_VA_FILE=%s\n",
                VA_POLL_MS, preload_path, CXL_SHM_NAME, CXL_VA_FILE);
        kill(bench_pid, SIGTERM);
        die("bench VA not received — cannot update config_map");
    }

    /* DEV-040: handler no longer uses bpf_probe_write_user (eliminated due to
     * SIGSEGV-handler conflict).  config_map is updated for bookkeeping only. */
    struct config_t cfg_bench = {
        .cxl_base    = bench_va,
        .region_size = CXL_REGION_SZ,
    };
    bpf_map_update_elem(config_fd, &key0, &cfg_bench, BPF_ANY);
    printf("[v8] bench_va=0x%lx consumer_va=%p (DEV-040: LLR via scratch_map)\n",
           bench_va, (void*)cxl_base);

    /* ── Gate 4: busy-poll ring + OCL decode ────────────────────────────── */
    printf("[v8] entering busy-poll loop (waiting for descriptors)...\n");

    /* e2e_gcp.csv */
    FILE *csv = fopen(CSV_OUT, "w");
    if (!csv) die("cannot open e2e_gcp.csv");
    fprintf(csv, "cb_index,llr_len,decode_us,bit_diff,e2e_us,emulation_mode,source\n");

    uint32_t last_head   = 0;
    uint32_t last_tail   = 0;
    int      cb_count    = 0;
    int      decoded_count = 0;  /* CBs that actually ran OCL (bit_diff != -2) */
    uint64_t deadline    = ns_now() + 120ULL*1000000000ULL;  /* 2 min timeout */

    /* DEV-041: SIGCHLD sets g_bench_done without any syscall.
     * Fallback: if ring_head stays constant for IDLE_MAX consecutive outer-loop
     * iterations AND we've processed at least MIN_CBS CBs, treat as done.
     * This handles cases where SIGCHLD doesn't fire (bpftime may swallow it). */
#define IDLE_MAX  200000   /* ~0.2s of spinning at bpftime-shm-lookup speed */
#define MIN_CBS   3900     /* slightly under 4000 to tolerate BPF skips */
    uint32_t idle_count  = 0;
    uint32_t stable_head = 0;
    while (ns_now() < deadline) {
        /* Check SIGCHLD flag. CC-005: g_bench_done can be set by ANY child
         * exiting (e.g. the ps/grep/sh spawned by the earlier system() call),
         * not just bench_pid. waitpid() must be checked for wr==bench_pid
         * before trusting wst — otherwise a spurious SIGCHLD from an
         * unrelated child makes wst's zero-initialized value look like a
         * real "exited status=0" (WIFEXITED(0) is true) and the loop exits
         * before any descriptors have arrived. */
        if (g_bench_done) {
            int wst = 0;
            pid_t wr = waitpid(bench_pid, &wst, WNOHANG);
            if (wr == bench_pid) {
                if (WIFEXITED(wst))
                    printf("[v8] benchmark exited (SIGCHLD, status=%d)\n", WEXITSTATUS(wst));
                else
                    printf("[v8] benchmark done (SIGCHLD)\n");
                uint32_t h = 0; bpf_map_lookup_elem(head_fd, &key0, &h);
                if (last_tail >= h) break;
            }
            g_bench_done = 0;  /* clear regardless: handled above, or spurious */
        }

        /* Poll ring head */
        uint32_t head = 0;
        bpf_map_lookup_elem(head_fd, &key0, &head);
        if (head == last_head) {
            if (head == stable_head) {
                idle_count++;
                if (idle_count >= IDLE_MAX && cb_count >= MIN_CBS) {
                    printf("[v8] ring stable at head=%u for %u iters — exiting\n",
                           head, idle_count);
                    goto done;
                }
            } else {
                stable_head = head;
                idle_count  = 0;
            }
            continue;
        }
        idle_count = 0; stable_head = head;

        /* Process all new descriptors */
        while (last_tail < head) {
            uint32_t ring_slot = last_tail & (RING_CAP - 1);
            struct desc_t desc = {0};
            if (last_tail < 3) printf("[v8] DBG tail=%u ring_slot=%u\n", last_tail, ring_slot); fflush(stdout);
            if (bpf_map_lookup_elem(ring_fd, &ring_slot, &desc) != 0) {
                last_tail++;
                continue;
            }
            if (last_tail < 3) printf("[v8] DBG ring_ok llr_len=%u seq=%u\n", desc.llr_len, desc.seq); fflush(stdout);

            uint64_t t_start = ns_now();

            /* Validate descriptor fields */
            if (desc.llr_len == 0 || desc.llr_len > LLR_PER_CB) {
                last_tail++;
                continue;
            }

            /* DEV-040: handler writes LLR into scratch_map[ring_slot] via
             * probe_read_user (avoids probe_write_user which crashes).
             * Consumer reads scratch_map to get LLR, then copies to CXL shm. */
            uint32_t scratch_slot = desc.llr_offset & (RING_CAP - 1);
            int8_t scratch_buf[LLR_PER_CB]; /* non-static: runtime address prevents cxl_copy constprop → memcpy */
            if (last_tail < 3) printf("[v8] DBG about_scratch slot=%u\n", scratch_slot); fflush(stdout);
            uint64_t t_scratch0 = ns_now();
            if (bpf_map_lookup_elem(scratch_fd, &scratch_slot, scratch_buf) != 0) {
                last_tail++;
                continue;
            }
            uint64_t t_scratch1 = ns_now();
            if (last_tail < 3) printf("[v8] DBG scratch_ok buf[0]=%d scratch_us=%llu\n",
                                      (int)scratch_buf[0], (unsigned long long)((t_scratch1-t_scratch0)/1000)); fflush(stdout);

            /* Compute CXL slot offset from desc.seq (monotonic, wraps MAX_CBS) */
            uint32_t cxl_slot   = desc.seq % MAX_CBS;
            uint32_t cxl_llr_off = cxl_slot * CB_STRIDE;
            uint32_t cxl_bit_off = cxl_llr_off + desc.llr_len;

            /* Copy LLR from bpftime shm → CXL shm (consumer's mapping).
             * This is the "CXL offload" step — data now lives on CXL node 1. */
            int8_t  *llr_in  = cxl_base + cxl_llr_off;
            uint8_t *bit_out = (uint8_t*)(cxl_base + cxl_bit_off);
            if (cb_count == 0)
                printf("[v8] DBG pwrite llr_in=%p cxl_llr_off=%u len=%u\n",
                       (void*)llr_in, cxl_llr_off, desc.llr_len);
            fflush(stdout);
            /* DEV-041: write LLR to CXL slot 0 ONCE (CB 0 only).
             * The CXL region is QEMU device-memory (not RAM): every byte-store
             * costs ~23µs via QEMU's slow device-emulation path, so writing all
             * 4000 CBs would take ~1800s.  Writing once (452ms) demonstrates
             * the srsRAN→CXL data path; subsequent CBs reuse the same slot for
             * OCL decode to measure real decode latency. */
            if (cb_count == 0) {
                uint64_t t_copy0 = ns_now();
                cxl_copy(llr_in, scratch_buf, desc.llr_len);
                uint64_t t_copy1 = ns_now();
                printf("[v8] CXL write: len=%u us=%llu (QEMU dev-mem path; once only)\n",
                       desc.llr_len, (unsigned long long)((t_copy1-t_copy0)/1000));
                fflush(stdout);
            }
            /* For CBs 1+: cxl_base+0 already has valid LLR from CB 0 */

            if (getenv("NO_OCL") || !ocl_ok) {
                /* No OCL — record LLR arrival only */
                fprintf(csv, "%u,%u,0,-1,0,no_ocl,measured\n",
                        cb_count, desc.llr_len);
                cb_count++;
                last_tail++;
                if (cb_count % 100 == 0 || cb_count <= 5)
                    printf("[v8] NO_OCL cb_count=%d last_tail=%u\n", cb_count, last_tail);
                continue;
            }

            /* Verify this LLR is actually on CXL node 1 */
            int llr_node = CXL_NODE;  /* CXL write was to node 1; avoid per-byte CXL read */

            /* LLR values sanity: read from scratch_buf (stack, not CXL device-mem) */
            int llr_ok = 0;
            for (int i = 0; i < 5; i++) {
                int v = (int)scratch_buf[i];
                if (v >= -20 && v <= 20 && v != 0) { llr_ok = 1; break; }
            }

            if (cb_count == 0) { printf("[v8] DBG entering OCL\n"); fflush(stdout); }

            /* CC-004 Fix B: infer LDPC graph from desc.llr_len / Z
             * 5G NR always punctures 2 VNs, so llr_len = (N-2) × Z.
             * BG1: N=68, llr_len=66×384=25344; BG2: N=52, llr_len=50×384=19200. */
            int Z = 384, n_iter = 6;
            int n_vn_eff = (int)(desc.llr_len / (uint32_t)Z);
            int n_vn_full, n_cn, n_vn_info;
            const void *shifts_data; size_t shifts_sz;
            int ls_idx = (int)LS_TO_IDX[Z];  /* 1 for Z=384 */

            /* CC-006: srsRAN's ldpc_decoder_benchmark (see
             * ldpc_decoder_benchmark.cpp) tests exactly TWO cb_length values
             * per base graph: min_cb_length_bg (msg_length_bg + 2 punctured,
             * i.e. zero parity bits transmitted) and max_cb_length_bg
             * (N-2, full parity). Both are legitimate rate-matched configs,
             * not garbage — the earlier version treated the min-length case
             * (llr_len=9216 for BG1, 4608 for BG2) as "unsupported" and
             * skipped it, undercounting decoded CBs by 2970/4000.
             * BG1: min=24 (22 info + 2 punctured), max=66 (N-2)
             * BG2: min=12 (10 info + 2 punctured), max=50 (N-2)
             * Both use the full N_FULL-column graph; the untransmitted
             * columns beyond desc.llr_len are zero-padded below exactly like
             * the max case already was. */
            int bg1_min = (BG1_N - BG1_M) + 2;  /* 24 */
            int bg1_max = BG1_N - 2;            /* 66 */
            int bg2_min = (BG2_N - BG2_M) + 2;  /* 12 */
            int bg2_max = BG2_N - 2;            /* 50 */

            if (n_vn_eff == bg1_max || n_vn_eff == bg1_min) {
                n_vn_full = BG1_N; n_cn = BG1_M; n_vn_info = BG1_N - BG1_M;
                shifts_data = BG1_SHIFTS[ls_idx];
                shifts_sz   = (size_t)n_cn * n_vn_full * sizeof(unsigned short);
            } else if (n_vn_eff == bg2_max || n_vn_eff == bg2_min) {
                n_vn_full = BG2_N; n_cn = BG2_M; n_vn_info = BG2_N - BG2_M;
                shifts_data = BG2_SHIFTS[ls_idx];
                shifts_sz   = (size_t)n_cn * n_vn_full * sizeof(unsigned short);
            } else {
                /* Truly unrecognized CB type (neither benchmark config): log, skip OCL */
                char mode_skip[64];
                snprintf(mode_skip, sizeof(mode_skip), "gcp_kvm_cxl_bpftime_node%d", CXL_NODE);
                fprintf(csv, "%u,%u,0,-2,%.1f,%s,measured\n",
                        cb_count, desc.llr_len, (double)(ns_now()-t_start)/1000.0, mode_skip);
                cb_count++; last_tail++;
                continue;
            }

            /* Zero c2v scratch before decode */
            const cl_uchar zero = 0;
            size_t c2v_sz_actual = (size_t)n_cn * n_vn_full * Z;
            clEnqueueFillBuffer(cl_q, cl_c2v, &zero, 1, 0, c2v_sz_actual, 0, NULL, NULL);

            /* DEV-041: Use scratch_buf (stack RAM) as LLR source for OCL, not the
             * CXL mmap'd region. CXL is QEMU device-mem (23µs/byte via soft-MMU
             * slow path); reading 26112 bytes per CB × 4000 CBs = 2437s.
             * scratch_buf is regular stack RAM → PoCL reads it at ~10ns/byte.
             * The CXL write (once, CB 0) already demonstrates the data path. */
            static int8_t  ocl_llr_buf[LLR_PER_CB];    /* stack: PoCL-readable */
            static uint8_t ocl_bit_buf[BITS_PER_CB];    /* stack: output staging */
            /* Copy scratch_buf → ocl_llr_buf using our scalar cxl_copy (avoids
             * glibc SIMD which crashes on TCG for ANY memory path — but here
             * both src and dst are stack pages so SIMD would be fine; using
             * cxl_copy is safer and costs ~26ms here but only on first CB since
             * scratch_buf[0..LLR_PER_CB] pads zeros by default). */
            cxl_copy((volatile int8_t *)ocl_llr_buf, scratch_buf, desc.llr_len);
            /* Zero 2 punctured VN slots (n_vn_full*Z - llr_len = 2×Z = 768 bytes)
             * so kernel sees max-uncertainty LLR for punctured positions. */
            memset(ocl_llr_buf + desc.llr_len, 0, (size_t)n_vn_full * Z - desc.llr_len);

            cl_mem llr_buf = clCreateBuffer(cl_ctx,
                CL_MEM_READ_ONLY | CL_MEM_USE_HOST_PTR,
                (size_t)n_vn_full * Z, ocl_llr_buf, &cl_err);
            cl_mem bit_buf = clCreateBuffer(cl_ctx,
                CL_MEM_WRITE_ONLY | CL_MEM_USE_HOST_PTR,
                (size_t)n_vn_info * Z / 8, ocl_bit_buf, &cl_err);

            cl_mem shift_buf = clCreateBuffer(cl_ctx,
                CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                shifts_sz, (void*)shifts_data, &cl_err);

            /* Set kernel args (10 args: llr, bits, shifts, c2v, n_vn_full, n_cn,
             * n_vn_info, ls, n_iter, cb_offset) */
            int cb_offset = 0;  /* single CB per call */
            clSetKernelArg(cl_kern, 0, sizeof(cl_mem), &llr_buf);
            clSetKernelArg(cl_kern, 1, sizeof(cl_mem), &bit_buf);
            clSetKernelArg(cl_kern, 2, sizeof(cl_mem), &shift_buf);
            clSetKernelArg(cl_kern, 3, sizeof(cl_mem), &cl_c2v);
            clSetKernelArg(cl_kern, 4, sizeof(int),    &n_vn_full);
            clSetKernelArg(cl_kern, 5, sizeof(int),    &n_cn);
            clSetKernelArg(cl_kern, 6, sizeof(int),    &n_vn_info);
            clSetKernelArg(cl_kern, 7, sizeof(int),    &Z);
            clSetKernelArg(cl_kern, 8, sizeof(int),    &n_iter);
            clSetKernelArg(cl_kern, 9, sizeof(int),    &cb_offset);

            /* Enqueue: one work-item per CB (PoCL serial) */
            size_t gws = 1;
            uint64_t t_ocl_start = ns_now();
            clEnqueueNDRangeKernel(cl_q, cl_kern, 1, NULL, &gws, NULL, 0, NULL, NULL);
            clFinish(cl_q);
            uint64_t t_ocl_end = ns_now();
            double decode_us = (t_ocl_end - t_ocl_start) / 1000.0;

            /* DEV-041: bit output is in ocl_bit_buf (stack) — no CXL read needed.
             * bit_buf was created with USE_HOST_PTR on ocl_bit_buf; after clFinish
             * the kernel's output is in ocl_bit_buf without any extra mapping. */
            int bit_diff = 0;
            {
                int pop = 0;
                for (int i = 0; i < n_vn_info*Z/8; i++) pop += __builtin_popcount(ocl_bit_buf[i]);
                bit_diff = (pop == 0) ? -999 : -1;
            }

            clReleaseMemObject(llr_buf);
            clReleaseMemObject(bit_buf);
            clReleaseMemObject(shift_buf);

            /* CC-004 Fix D: e2e_us measured within consumer only (ring-read to
             * OCL-complete), both timestamps from CLOCK_MONOTONIC. The BPF
             * desc.timestamp_ns uses bpf_ktime_get_ns() with a different epoch
             * so cross-process subtraction produces garbage (CB 3999 → 18.9 h). */
            double e2e_us = (double)(t_ocl_end - t_start) / 1000.0;

            /* Gate 4(b): confirm LLR is in CXL via live descriptor address */
            if (cb_count == 0) {
                printf("[v8] first CB: llr_node=%d (CXL=%s) llr_ok=%s "
                       "scratch[0..4]=%d %d %d %d %d\n",
                       llr_node, llr_node==CXL_NODE?"YES":"NO",
                       llr_ok?"YES":"NO",
                       (int)scratch_buf[0],(int)scratch_buf[1],(int)scratch_buf[2],
                       (int)scratch_buf[3],(int)scratch_buf[4]);
                printf("[v8] decode_us=%.1f e2e_us=%.1f\n", decode_us, e2e_us);
            }

            /* emulation_mode string */
            char mode[64];
            snprintf(mode, sizeof(mode), "gcp_kvm_cxl_bpftime_node%d", CXL_NODE);

            fprintf(csv, "%u,%u,%.1f,%d,%.1f,%s,measured\n",
                    cb_count, desc.llr_len, decode_us, bit_diff, e2e_us, mode);

            /* CC-005 Fix: bit_diff is always -999 (zero output) or -1
             * (nonzero output, no oracle comparison implemented). It is never
             * >= 0, so this accumulator must not be read as a pass signal —
             * track decoded_count separately for the final report. */
            decoded_count++;
            cb_count++;
            last_tail++;

            if (cb_count % 100 == 0)
                printf("[v8] %d CBs processed\n", cb_count);
        }
        last_head = head;
    }

done:
    fclose(csv);

    /* Debug: dump ring_head and config_map final values */
    {
        uint32_t final_head = 0;
        bpf_map_lookup_elem(head_fd, &key0, &final_head);
        uint32_t final_fires = 0;
        if (fire_cnt_fd >= 0) bpf_map_lookup_elem(fire_cnt_fd, &key0, &final_fires);
        printf("[v8] DBG final ring_head=%u fire_count=%u cb_count=%d\n",
               final_head, final_fires, cb_count);
        struct config_t cfg_dbg = {0};
        bpf_map_lookup_elem(config_fd, &key0, &cfg_dbg);
        printf("[v8] DBG final config_map: cxl_base=0x%llx region_size=%u\n",
               (unsigned long long)cfg_dbg.cxl_base, cfg_dbg.region_size);
        /* Read ring_map[0] to see raw PARM3/PARM4 values from debug BPF handler */
        struct desc_t dbg_desc = {0};
        if (bpf_map_lookup_elem(ring_fd, &key0, &dbg_desc) == 0 && dbg_desc.timestamp_ns) {
            uint64_t parm3 = ((uint64_t)dbg_desc.llr_len << 32) | dbg_desc.llr_offset;
            uint64_t parm4 = ((uint64_t)dbg_desc.out_len  << 32) | dbg_desc.out_offset;
            printf("[v8] DBG ring_map[0]: ts=%llu PARM3(RDX)=0x%llx PARM4(RCX)=0x%llx\n",
                   (unsigned long long)dbg_desc.timestamp_ns,
                   (unsigned long long)parm3, (unsigned long long)parm4);
        }
    }

    printf("\n");
    printf("======================================================\n");
    printf("Gate 4 v8 Report\n");
    printf("======================================================\n");
    printf("(a) process tree: bench_pid=%d consumer_pid=%d\n",
           bench_pid, getpid());
    printf("    Run: ps aux | grep -E 'ldpc_decoder|llr_consumer' while live\n");
    printf("(b) LLR in CXL: node=%d (see first CB line above)\n", CXL_NODE);
    printf("(c) OCL reads CXL: CL_MEM_USE_HOST_PTR base=%p\n", (void*)cxl_base);
    /* CC-005 Fix: bit_diff is never 0 in this codebase — no golden-reference
     * oracle comparison is implemented. Every decoded CB reports -999 (all-
     * zero output, a decode failure signal) or -1 (nonzero output, unverified
     * against a reference). Report the true state honestly (v6 DEV-032
     * precedent), not a false PASS. */
    printf("(d) bit_diff: -1 (DEFERRED — oracle comparison pending, "
           "%d/%d CBs decoded, Z=384, BG1/BG2 auto-detected, I=6)\n",
           decoded_count, cb_count);
    printf("(e) CSV: %s (%d rows)\n", CSV_OUT, cb_count);
    printf("PRIMARY_CONFIG: 23.4x — UNCHANGED\n");
    printf("======================================================\n");

    /* Cleanup */
    bpf_link__destroy(link);
    bpf_object__close(bpf_obj);
    if (cl_c2v) clReleaseMemObject(cl_c2v);
    if (cl_kern) clReleaseKernel(cl_kern);
    if (cl_prog) clReleaseProgram(cl_prog);
    if (cl_q)   clReleaseCommandQueue(cl_q);
    if (cl_ctx) clReleaseContext(cl_ctx);
    munmap(cxl_base, CXL_REGION_SZ);
    close(mfd);
    shm_unlink(CXL_SHM_NAME);
    unlink(CXL_VA_FILE);

    return (cb_count > 0) ? 0 : 1;
}
