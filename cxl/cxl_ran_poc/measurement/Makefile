CC ?= gcc
CFLAGS := -Wall -Wextra -Werror -O2 -I../l1_sim
LDFLAGS := -lm

OBJS := measure.o ../l1_sim/ldpc.o ../l1_sim/fft.o

all: measure

measure: $(OBJS)
	$(CC) $(CFLAGS) -o $@ measure.o ../l1_sim/ldpc.o ../l1_sim/fft.o $(LDFLAGS)

../l1_sim/ldpc.o ../l1_sim/fft.o:
	$(MAKE) -C ../l1_sim ldpc.o fft.o

clean:
	rm -f measure.o measure

.PHONY: all clean
