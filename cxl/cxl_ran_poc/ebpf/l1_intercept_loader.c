#define _GNU_SOURCE
#include "l1_intercept.h"

#include <bpf/libbpf.h>
#include <bpf/bpf.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <gelf.h>
#include <libelf.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

/*
 * l1_intercept_loader — libbpf uprobe loader for the CXL RAN PoC.
 *
 * Supports two target modes:
 *  a) PoC sim (ran_l1_sim):  C symbols ldpc_decode / fft_process
 *  b) srsRAN benchmark:      C++ mangled symbol, offset from
 *                            paper/results/srsran_probe_symbol.txt
 *
 * Usage:
 *   l1_intercept_loader [options]
 *     -b PATH   path to target binary (default: ./l1_sim/ran_l1_sim)
 *     -s PATH   GPU daemon Unix socket (default: /tmp/gpu_daemon.sock)
 *     -e        enable offload (default: probe only, no offload)
 *     -f FILE   srsRAN probe symbol file (overrides -b symbol lookup)
 */

static volatile int g_running = 1;
static const char  *g_socket_path = "/tmp/gpu_daemon.sock";

/* ── socket forwarding ───────────────────────────────────────────────────── */

static int forward_event(const struct offload_event *event)
{
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	struct sockaddr_un addr;

	if (fd < 0)
		return -1;

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, g_socket_path, sizeof(addr.sun_path) - 1);

	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(fd);
		return -1;
	}

	if (write(fd, event, sizeof(*event)) != (ssize_t)sizeof(*event)) {
		close(fd);
		return -1;
	}
	close(fd);
	return 0;
}

/* ── ring buffer callback ────────────────────────────────────────────────── */

static int handle_ringbuf(void *ctx, void *data, size_t size)
{
	(void)ctx;
	if (size < sizeof(struct offload_event))
		return 0;

	forward_event(data);
	return 0;
}

/* ── ELF symbol lookup ───────────────────────────────────────────────────── */

/*
 * Search both SHT_SYMTAB and SHT_DYNSYM for the first symbol whose
 * name contains substr (case-insensitive match on demangled-style names).
 * Returns the st_value (file offset for ET_EXEC / relative for ET_DYN).
 */
static uint64_t find_symbol_by_substr(const char *binary, const char *substr)
{
	if (!binary || !substr)
		return 0;

	if (elf_version(EV_CURRENT) == EV_NONE)
		return 0;

	int fd = open(binary, O_RDONLY);
	if (fd < 0)
		return 0;

	Elf *elf = elf_begin(fd, ELF_C_READ, NULL);
	if (!elf) {
		close(fd);
		return 0;
	}

	uint64_t result   = 0;
	Elf_Scn *scn      = NULL;
	size_t   shstrndx = 0;

	elf_getshdrstrndx(elf, &shstrndx);

	while ((scn = elf_nextscn(elf, scn)) != NULL && result == 0) {
		GElf_Shdr shdr;

		if (gelf_getshdr(scn, &shdr) != &shdr)
			continue;
		if (shdr.sh_type != SHT_SYMTAB && shdr.sh_type != SHT_DYNSYM)
			continue;

		Elf_Data *data  = elf_getdata(scn, NULL);
		size_t    count = shdr.sh_size / shdr.sh_entsize;

		for (size_t i = 0; i < count && result == 0; i++) {
			GElf_Sym sym;

			if (gelf_getsym(data, i, &sym) != &sym)
				continue;
			if (GELF_ST_TYPE(sym.st_info) != STT_FUNC)
				continue;

			const char *name = elf_strptr(elf, shdr.sh_link, sym.st_name);
			if (!name)
				continue;

			/* Case-insensitive substring search */
			const char *p = name;

			while (*p) {
				size_t j;

				for (j = 0; substr[j] && p[j]; j++) {
					if ((substr[j] | 0x20) != (p[j] | 0x20))
						break;
				}
				if (!substr[j]) {
					result = sym.st_value;
					break;
				}
				p++;
			}
		}
	}

	elf_end(elf);
	close(fd);
	return result;
}

/*
 * Read the PRIMARY PROBE OFFSET from a symbol file written by
 * 04_find_probe_symbol.sh.  Returns 0 if not found.
 */
static uint64_t read_primary_offset(const char *symbol_file)
{
	FILE *f = fopen(symbol_file, "r");
	if (!f)
		return 0;

	char     line[512];
	uint64_t offset = 0;

	while (fgets(line, sizeof(line), f)) {
		if (strncmp(line, "PRIMARY PROBE OFFSET:", 21) == 0) {
			if (sscanf(line + 21, " 0x%lx", &offset) == 1)
				break;
			if (sscanf(line + 21, " %lx",   &offset) == 1)
				break;
		}
	}
	fclose(f);
	return offset;
}

/*
 * Read the first mangled symbol name from a symbol file.
 * The file contains lines like:
 *   0000000000123456 T _ZN6srsran...
 * Returns a malloc'd string or NULL.
 */
static char *read_mangled_symbol(const char *symbol_file)
{
	FILE *f = fopen(symbol_file, "r");
	if (!f)
		return NULL;

	char  line[1024];
	char *result = NULL;

	while (fgets(line, sizeof(line), f)) {
		/* Skip comment/header lines */
		if (line[0] == '=' || line[0] == '#' || line[0] == '\n')
			continue;
		/* Look for " T _Z..." or " t _Z..." (text section symbols) */
		char addr[64], type[4], sym[512];

		if (sscanf(line, "%63s %3s %511s", addr, type, sym) == 3) {
			if ((type[0] == 'T' || type[0] == 't') &&
			    sym[0] == '_' && sym[1] == 'Z') {
				result = strdup(sym);
				break;
			}
		}
	}
	fclose(f);
	return result;
}

/* ── signal handler ──────────────────────────────────────────────────────── */

static void sig_handler(int sig)
{
	(void)sig;
	g_running = 0;
}

/* ── main ────────────────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
	const char *binary      = "./l1_sim/ran_l1_sim";
	const char *symbol_file = NULL;
	const char *bpf_obj     = NULL;
	int         enable      = 0;

	int opt;

	while ((opt = getopt(argc, argv, "b:s:f:o:e")) != -1) {
		switch (opt) {
		case 'b': binary      = optarg; break;
		case 's': g_socket_path = optarg; break;
		case 'f': symbol_file = optarg; break;
		case 'o': bpf_obj     = optarg; break;
		case 'e': enable      = 1;      break;
		default:  return 1;
		}
	}

	/*
	 * Default BPF object: look next to the loader executable,
	 * then fall back to CWD.
	 */
	char bpf_obj_default[4096];

	if (!bpf_obj) {
		/* Derive from argv[0]: replace basename with l1_intercept.bpf.o */
		strncpy(bpf_obj_default, argv[0], sizeof(bpf_obj_default) - 1);
		bpf_obj_default[sizeof(bpf_obj_default) - 1] = '\0';
		char *slash = strrchr(bpf_obj_default, '/');

		if (slash)
			strcpy(slash + 1, "l1_intercept.bpf.o");
		else
			strcpy(bpf_obj_default, "l1_intercept.bpf.o");
		bpf_obj = bpf_obj_default;
	}

	signal(SIGINT,  sig_handler);
	signal(SIGTERM, sig_handler);

	/* Suppress libbpf info messages; errors still go to stderr */
	libbpf_set_print(NULL);

	struct bpf_object *obj = bpf_object__open_file(bpf_obj, NULL);
	if (!obj) {
		fprintf(stderr, "[loader] Cannot open %s\n", bpf_obj);
		return 1;
	}

	int err = bpf_object__load(obj);
	if (err) {
		fprintf(stderr, "[loader] BPF load failed: %d\n", err);
		bpf_object__close(obj);
		return 1;
	}

	/* Set offload enable flag */
	int map_fd = bpf_object__find_map_fd_by_name(obj, "offload_enabled");
	if (map_fd >= 0) {
		__u32 key = 0, val = enable ? 1u : 0u;

		bpf_map_update_elem(map_fd, &key, &val, BPF_ANY);
	}

	/* Locate programs */
	struct bpf_program *prog_ldpc =
		bpf_object__find_program_by_name(obj, "intercept_ldpc_decode");
	struct bpf_program *prog_fft  =
		bpf_object__find_program_by_name(obj, "intercept_fft_process");

	if (!prog_ldpc) {
		fprintf(stderr, "[loader] intercept_ldpc_decode program not found\n");
		bpf_object__close(obj);
		return 1;
	}

	/* ── Resolve probe offset ─────────────────────────────────────────── */
	uint64_t off_ldpc = 0;
	uint64_t off_fft  = 0;

	if (symbol_file) {
		/* srsRAN mode: read pre-computed offset from symbol file */
		off_ldpc = read_primary_offset(symbol_file);

		if (off_ldpc == 0) {
			/* Fallback: try mangled symbol name lookup */
			char *mangled = read_mangled_symbol(symbol_file);

			if (mangled) {
				off_ldpc = find_symbol_by_substr(binary, mangled);
				free(mangled);
			}
		}

		if (off_ldpc == 0) {
			/* Last resort: substring search for common srsRAN patterns */
			off_ldpc = find_symbol_by_substr(binary, "ldpc_decoder_impl");
			if (off_ldpc == 0)
				off_ldpc = find_symbol_by_substr(binary, "ldpc_decode");
		}

		printf("[loader] srsRAN probe offset: 0x%lx\n",
		       (unsigned long)off_ldpc);
	} else {
		/* PoC sim mode: direct C symbol names */
		off_ldpc = find_symbol_by_substr(binary, "ldpc_decode");
		off_fft  = find_symbol_by_substr(binary, "fft_process");
	}

	if (off_ldpc == 0) {
		fprintf(stderr, "[loader] WARN: LDPC symbol not found in %s\n",
		        binary);
		fprintf(stderr, "[loader] Continuing — uprobe may not fire.\n");
	}

	/* ── Attach uprobes ───────────────────────────────────────────────── */
	struct bpf_link *link_ldpc = NULL;
	struct bpf_link *link_fft  = NULL;

	if (off_ldpc) {
		link_ldpc = bpf_program__attach_uprobe(prog_ldpc, false, -1,
		                                        binary, off_ldpc);
		if (!link_ldpc || libbpf_get_error(link_ldpc)) {
			fprintf(stderr, "[loader] uprobe attach (ldpc) failed: %ld\n",
			        libbpf_get_error(link_ldpc));
			link_ldpc = NULL;
		} else {
			printf("[loader] LDPC uprobe attached @ 0x%lx in %s\n",
			       (unsigned long)off_ldpc, binary);
		}
	}

	if (prog_fft && off_fft) {
		link_fft = bpf_program__attach_uprobe(prog_fft, false, -1,
		                                       binary, off_fft);
		if (!link_fft || libbpf_get_error(link_fft)) {
			link_fft = NULL;
		} else {
			printf("[loader] FFT uprobe attached @ 0x%lx\n",
			       (unsigned long)off_fft);
		}
	}

	/* ── Ring buffer ──────────────────────────────────────────────────── */
	int rb_fd = bpf_object__find_map_fd_by_name(obj, "offload_ringbuf");
	struct ring_buffer *rb = NULL;

	if (rb_fd >= 0)
		rb = ring_buffer__new(rb_fd, handle_ringbuf, NULL, NULL);

	printf("[loader] running  offload=%s  binary=%s\n",
	       enable ? "on" : "off", binary);

	while (g_running) {
		if (rb) {
			err = ring_buffer__poll(rb, 200);
			if (err < 0 && err != -EINTR)
				break;
		} else {
			sleep(1);
		}
	}

	ring_buffer__free(rb);
	bpf_link__destroy(link_ldpc);
	bpf_link__destroy(link_fft);
	bpf_object__close(obj);
	return 0;
}
