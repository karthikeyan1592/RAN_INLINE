# Running p2b-k5-k6's tests

```
cd ..                    # feature root (p2b-k5-k6/)
make bootstrap-ocudu     # first time only, or after OCUDU_BUILD has been wiped — ~1 min
make test                # builds + runs k5_test and k6_test
```

`bootstrap-ocudu` configures and builds OCUDU (`third_party/ocudu`) into
`open_inline/.build/ocudu` by default — a location under `open_inline/`, not `/tmp`, specifically
so it survives across sessions (the previous scratch-tmp build directory this slice originally
used got wiped by session cleanup twice during development). Override the location with
`OCUDU_BUILD=/some/path` on either target if you already have a build elsewhere.

Only two CMake targets get built: `ocudu_channel_coding` (pulls in ldpc/polar/short_block/
crc_calculator transitively) and `ocudu_sequence_generators` (K5's oracle; not part of
p0-rig-scaffold's own Docker build, needed here for the first time in this slice).

## Why a Makefile at all

Both test binaries link against 11 real OCUDU static libraries in an order that matters
(`libocudu_channel_coding.a` must precede `libocudu_ldpc.a` — its factory functions resolve
symbols the linker only scans for once) and locate their `.cl` kernel source via paths hardcoded
relative to the *runtime* working directory (`tests/`), not the build directory. Getting either
wrong doesn't silently produce a wrong answer — it fails loudly (undefined-reference linker
errors, or `fopen` returning null) — but reconstructing the right recipe by hand each time is
exactly the kind of friction that erodes confidence in "verified" claims once this pattern
repeats across p2c–p2f. `make test` is the one command that is guaranteed to reproduce the
results recorded in `../VERIFICATION.md`.

## Manual invocation (for reference — prefer `make test`)

If you need to run a single binary directly rather than via `make`, both binaries must be
executed with `tests/` as the working directory:

```
cd tests
../build/k5_test
../build/k6_test
```
