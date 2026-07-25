# p0-rig-scaffold — gpu-phy image (SIM compute_backend skeleton)
# SPEC: P0-R3/R4/R6. Ubuntu 24.04, PoCL ICD, AdaptiveCpp (OMP backend), clinfo, LDPC bit-exact suite.
# No /dev mounts, no privileges (enforced by compose.sim.yml, not this file).
#
# Build context: this file expects an ADDITIONAL named build context "ocudu" pointing at the
# pinned OCUDU checkout (third_party/ocudu, release_26_04) — see helpers/build_images.sh, which
# invokes: docker buildx build --build-context ocudu=<path-to-third_party/ocudu> ...
# This avoids vendoring/copying the ~96MB OCUDU source tree into this repo just to reach it as a
# build dependency (research/ocudu_repin.md; the source itself stays a pinned, symlinked
# reference at docker/upstream — this Dockerfile's "ocudu" context is a second, independent path
# to the same pinned checkout, used only for the LDPC-encoder library link dependency).

ARG ADAPTIVECPP_TAG=v25.10.0

########################################
# Stage 1: OCUDU channel-coding library #
########################################
# Only what bit_diff_test.cpp needs to link: the LDPC ENCODER (oracle ground-truth generator).
# All RF/hardware/testing options disabled -- this is a pure DSP/math library, no radio deps.
FROM ubuntu:24.04 AS ocudu-build

RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential cmake pkg-config git ca-certificates \
      libyaml-cpp-dev libboost-context-dev libboost-fiber-dev libsctp-dev libmbedtls-dev \
      ocl-icd-opencl-dev opencl-headers \
    && rm -rf /var/lib/apt/lists/*

# Named build context "ocudu" -> pinned third_party/ocudu checkout (release_26_04, BSD-3).
COPY --from=ocudu . /src/ocudu

RUN mkdir -p /build/ocudu && cd /build/ocudu && \
    cmake /src/ocudu \
      -DCMAKE_BUILD_TYPE=Release \
      -DBUILD_TESTING=OFF \
      -DENABLE_UHD=OFF -DENABLE_SIDEKIQ=OFF -DENABLE_ZEROMQ=OFF \
      -DENABLE_FFTW=OFF -DENABLE_MKL=OFF -DENABLE_FFTZ=OFF -DENABLE_ARMPL=OFF \
      -DENABLE_DPDK=OFF -DENABLE_LIBNUMA=OFF -DENABLE_BACKWARD=OFF && \
    cmake --build . --target ocudu_channel_coding -j"$(nproc)"

######################################
# Stage 2: AdaptiveCpp (OMP backend) #
######################################
# Built from source against system LLVM/Clang 17 (no upstream Ubuntu 24.04 package exists).
# OMP backend only in this image (SIM has no GPU); the same install works unchanged when p2 adds
# vendor targets later (SIM §3 design decision 3: baked in from P0 so p2 doesn't churn the base).
FROM ubuntu:24.04 AS acpp-build
ARG ADAPTIVECPP_TAG

RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential cmake git ca-certificates python3 \
      llvm-17-dev libclang-17-dev clang-17 clang-tools-17 libomp-17-dev lld-17 \
      libboost-context-dev libboost-fiber-dev libnuma-dev \
    && rm -rf /var/lib/apt/lists/*

RUN git clone --branch "${ADAPTIVECPP_TAG}" --depth 1 \
      https://github.com/AdaptiveCpp/AdaptiveCpp.git /src/acpp && \
    git -C /src/acpp rev-parse HEAD > /src/acpp.sha

RUN mkdir -p /build/acpp && cd /build/acpp && \
    cmake /src/acpp \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX=/opt/acpp \
      -DWITH_CPU_BACKEND=ON \
      -DWITH_CUDA_BACKEND=OFF -DWITH_ROCM_BACKEND=OFF -DWITH_LEVEL_ZERO_BACKEND=OFF \
      -DWITH_OPENCL_BACKEND=OFF \
      -DLLVM_DIR=/usr/lib/llvm-17/cmake \
      -DACPP_LLD_PATH=/usr/lib/llvm-17/bin/ld.lld && \
    cmake --build . -j"$(nproc)" && \
    cmake --install .

##############################
# Stage 3: LDPC suite binary #
##############################
FROM ocudu-build AS ldpc-build
COPY ldpc_suite/bg_tables.h ldpc_suite/ldpc_decode.cl ldpc_suite/bit_diff_test.cpp /src/ldpc_suite/

RUN mkdir -p /oi/logs /oi/bin && \
    g++ -std=c++17 -O2 \
      -I /src/ldpc_suite \
      -I /src/ocudu/include \
      -I /src/ocudu/external/fmt/include \
      -DRESULTS_CSV='"/oi/logs/bit_correctness.csv"' \
      /src/ldpc_suite/bit_diff_test.cpp \
      /build/ocudu/lib/phy/upper/channel_coding/libocudu_channel_coding.a \
      /build/ocudu/lib/phy/upper/channel_coding/ldpc/libocudu_ldpc.a \
      /build/ocudu/lib/phy/upper/channel_coding/polar/libocudu_polar.a \
      /build/ocudu/lib/phy/upper/channel_coding/short/libocudu_short_block.a \
      /build/ocudu/lib/phy/upper/channel_coding/libocudu_crc_calculator.a \
      /build/ocudu/lib/phy/upper/liblog_likelihood_ratio.a \
      /build/ocudu/lib/ocuduvec/libocuduvec.a \
      /build/ocudu/lib/support/libocudu_support.a \
      /build/ocudu/lib/support/math/libocudu_support_math.a \
      /build/ocudu/lib/ocudulog/libocudulog.a \
      /build/ocudu/external/fmt/libfmt.a \
      -lOpenCL -lpthread \
      -o /oi/bin/bit_diff_test

################################
# Final: gpu-phy runtime image #
################################
FROM ubuntu:24.04

RUN apt-get update && apt-get install -y --no-install-recommends \
      pocl-opencl-icd ocl-icd-libopencl1 clinfo \
      llvm-17-runtime libclang-cpp17t64 clang-17 libomp5-17t64 \
      libboost-context1.83.0 libboost-fiber1.83.0 \
      ca-certificates \
    && rm -rf /var/lib/apt/lists/*

ENV OI_CL_PLATFORM=pocl
ENV OI_VECTOR_DIR=/oi/vectors
ENV OI_LOG_DIR=/oi/logs
RUN mkdir -p /oi/vectors /oi/logs /oi/bin

COPY --from=acpp-build /opt/acpp /opt/acpp
ENV PATH="/opt/acpp/bin:${PATH}"

COPY --from=ldpc-build /oi/bin/bit_diff_test /oi/bin/bit_diff_test
COPY ldpc_suite/ldpc_decode.cl /oi/bin/ldpc_decode.cl

ARG OI_PINS_CORE="{}"
LABEL org.openinline.component="gpu-phy"
LABEL org.openinline.pins=$OI_PINS_CORE

WORKDIR /oi/bin
CMD ["sleep", "infinity"]
