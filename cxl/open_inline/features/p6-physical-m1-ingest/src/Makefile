# Build-verified-only, per this session's explicit scope: P6-R1/R2/R3 standalone probe programs,
# compiled here to confirm they're compile-clean against this host's real installed libibverbs/
# mlx5dv headers -- NOT run against real hardware (none present). See ../STATUS.md and this
# session's VERIFICATION notes for what "build-verified-only" means for each probe.
CC      ?= gcc
CFLAGS  ?= -Wall -Wextra -std=c11
LDLIBS  := -libverbs -lmlx5

.PHONY: all clean
all: probe01_nic_ident probe02_dmabuf_mr probe03_rawqp_hostmem

probe01_nic_ident: probe01_nic_ident.c
	$(CC) $(CFLAGS) $< -o $@ -libverbs

probe02_dmabuf_mr: probe02_dmabuf_mr.c
	$(CC) $(CFLAGS) $< -o $@ -libverbs

probe03_rawqp_hostmem: probe03_rawqp_hostmem.c
	$(CC) $(CFLAGS) $< -o $@ $(LDLIBS)

clean:
	rm -f probe01_nic_ident probe02_dmabuf_mr probe03_rawqp_hostmem
