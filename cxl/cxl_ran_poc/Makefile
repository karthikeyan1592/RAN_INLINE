.PHONY: all clean l1 gpu ebpf offload measure

all: l1 gpu ebpf offload measure

l1:
	$(MAKE) -C l1_sim

gpu:
	$(MAKE) -C gpu_daemon

ebpf:
	$(MAKE) -C ebpf

offload:
	$(MAKE) -C offload

measure:
	$(MAKE) -C measurement

clean:
	$(MAKE) -C l1_sim clean
	$(MAKE) -C gpu_daemon clean
	$(MAKE) -C ebpf clean
	$(MAKE) -C offload clean
	$(MAKE) -C measurement clean
