CLANG ?= clang
CC ?= gcc
CFLAGS := -Wall -Wextra -Werror -O2 -I..
BPF_CFLAGS := -g -O2 -target bpf -D__TARGET_ARCH_x86 -I.

all: l1_intercept.bpf.o l1_intercept_loader

l1_intercept.bpf.o: l1_intercept.bpf.c l1_intercept.h
	$(CLANG) $(BPF_CFLAGS) -c l1_intercept.bpf.c -o $@

l1_intercept_loader: l1_intercept_loader.c l1_intercept.h
	$(CC) $(CFLAGS) -o $@ l1_intercept_loader.c -lbpf -lelf -lpthread

clean:
	rm -f l1_intercept.bpf.o l1_intercept_loader

.PHONY: all clean
