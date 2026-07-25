# Running p2d-k2-k3's tests

```
cd ..                    # feature root (p2d-k2-k3/)
make bootstrap-ocudu     # first time only, or after OCUDU_BUILD has been wiped
make test                # builds + runs k2_test and k3_test
```

Reuses the same shared `OCUDU_BUILD` directory (`open_inline/.build/ocudu`) p2b/p2c bootstrap —
safe to re-run, only builds what's missing. This slice adds the channel-estimation
(`ocudu_dmrs_pusch`, `ocudu_channel_estimator`), equalization (`ocudu_channel_equalizer`), and DFT
(`ocudu_dft`, generic — no FFTW/MKL/ARMPL) targets.

Both binaries must run with `tests/` as the working directory (same CWD-relative `.cl` source
path convention as `p2b-k5-k6`/`p2c-k1`); `make run-k2`/`make run-k3`/`make test` handle this.
