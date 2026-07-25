# p0-rig-scaffold — oracle image (golden-vector store + verdict CLI, P0-R5)
# Build context: p0-rig-scaffold root (see compose.sim.yml: context: .., dockerfile:
# docker/oracle/Dockerfile) so this image can COPY src/oracle_verdict/ without duplicating it
# into docker/oracle/.
FROM ubuntu:24.04

RUN apt-get update && apt-get install -y --no-install-recommends python3 \
    && rm -rf /var/lib/apt/lists/*

ENV OI_VECTOR_DIR=/oi/vectors
RUN mkdir -p /oi/vectors /oi/bin

COPY src/oracle_verdict/oracle_verdict.py /oi/bin/oracle-verdict
RUN chmod +x /oi/bin/oracle-verdict
COPY docker/oracle/vectors/ /oi/vectors/

ARG OI_PINS_CORE="{}"
LABEL org.openinline.component="oracle"
LABEL org.openinline.pins=$OI_PINS_CORE

ENV PATH="/oi/bin:${PATH}"
WORKDIR /oi
CMD ["sleep", "infinity"]
