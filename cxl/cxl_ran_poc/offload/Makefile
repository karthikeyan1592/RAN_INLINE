CC ?= gcc
CFLAGS := -Wall -Wextra -Werror -O2 -fPIC -I.. -I../gpu_daemon -I../l1_sim
LDFLAGS := -shared -lnuma -lpthread

all: libl1_offload.so

# cxl_memory.c must be compiled with -fPIC for the shared library; the copy
# in gpu_daemon/ is built without -fPIC (it links into an executable).
cxl_memory_pic.o: ../gpu_daemon/cxl_memory.c
	$(CC) $(CFLAGS) -c -o $@ $<

libl1_offload.so: l1_offload.o cxl_memory_pic.o ../l1_sim/ldpc.o ../l1_sim/fft.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) -lm

l1_offload.o: l1_offload.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f *.o libl1_offload.so

.PHONY: all clean
