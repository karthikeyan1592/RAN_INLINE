# Kernel configuration

Apply `kernel_config_fragment` when building a 6.6+ kernel for QEMU CXL Type-3
testing inside the VM.

On the host, the PoC falls back to shared mmap (`/tmp/cxl_ran_poc_shm`) when
`/dev/dax0.0` is unavailable.
