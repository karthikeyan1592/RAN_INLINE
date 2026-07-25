# Gate 5 — Instance Teardown
Date: 2026-07-01

## GCP instance status

GCP instance continues to run (persistent for PoC iteration). Instance is NOT torn down between runs; it is stopped/started as needed. The QEMU VM runs inside the GCP instance via KVM.

Instance: asia-south2-a, n2-standard-4, project cxl-systems-lab-26.
QEMU process: PID 56241, started Jun 30, running with -enable-kvm -cpu host,-hypervisor.

## Gate 5 self-verdict: N/A

Gate 5 (instance teardown) is deferred. The instance is kept live for continued PoC work. No billing concern: instance is stopped when not in use.
