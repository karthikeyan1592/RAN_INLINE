# Running p2e-k4's tests

```
cd ..                    # feature root (p2e-k4/)
make bootstrap-ocudu     # first time only, or after OCUDU_BUILD has been wiped
make test                # builds + runs k4_test
```

Reuses the shared `OCUDU_BUILD` directory (`open_inline/.build/ocudu`) — safe to re-run, only
builds what's missing. This slice adds `ocudu_channel_modulation`.

`k4_test` must run with `tests/` as the working directory (same CWD-relative `.cl` source path
convention as the other p2* slices); `make run-k4`/`make test` handle this.
