CC ?= gcc
CFLAGS := -Wall -Wextra -Werror -O2 -fPIC -I.
LDFLAGS := -lm -lnuma

OBJS := ldpc.o fft.o

all: libran_l1.a ran_l1_sim

libran_l1.a: $(OBJS)
	$(AR) rcs $@ $^

ran_l1_sim: ran_l1_sim.o libran_l1.a
	$(CC) $(CFLAGS) -o $@ ran_l1_sim.o -L. -lran_l1 $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f *.o *.a ran_l1_sim

.PHONY: all clean
