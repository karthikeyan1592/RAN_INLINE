# Open Inline v3 — Architecture Diagrams

Companion to `ARCHITECTURE_v3.md`. Source is **PlantUML**; rendered SVGs embedded below and in
`docs/diagrams/`. Regenerate after edits with `scripts/render_diagrams.sh`.

Legend: **solid** = data path · **dashed** = control path · green = proven/open · yellow = spike/gate ·
red = proprietary/not-needed · blue = future/stretch.

Diagrams 1–5 are **Phase 1** (no CXL, buildable now). Diagram 6 is **Phase 2** (locked scope, CXL
swaps the GPU→CPU leg only — not built yet).

---

## 1. [Phase 1] Architecture — complete L1 on GPU (data path, no CXL)

```plantuml
@startuml arch_datapath_v3
title Open Inline v3 — Complete L1 on GPU (uplink), no CXL
skinparam componentStyle rectangle

actor "UE" as UE
node "O-RU\n(SDR or OAI FHI/xran software O-RU)" as RU
cloud "Fronthaul\neCPRI/UDP over Ethernet\n(split 7.2x: RU does FFT)" as FH

node "NIC (Mellanox ConnectX, mlx5)" as NIC #D4EDDA
node "GPU (NVIDIA / AMD / Intel)" {
  [eCPRI depacketize +\nRE-grid reassembly] as DEPKT
  [chan-est] as CE
  [equalize] as EQ
  [demap -> LLR] as DM
  [LDPC decode\n(bit-exact kernel)] as LD
  DEPKT --> CE
  CE --> EQ
  EQ --> DM
  DM --> LD
}
node "CPU" {
  [CRC24] as CRC
  [PHY<->L2 seam\n(custom IPC now,\nFAPI-style later)] as SEAM
  [MAC/RLC/PDCP] as L2
  CRC --> SEAM
  SEAM --> L2
}

UE --> RU : RF
RU --> FH : freq-domain REs
FH --> NIC
NIC ==> DEPKT : ibv_reg_dmabuf_mr()\n(OPEN, no NVIDIA SW needed)\nNIC DMAs into GPU VRAM
LD --> CRC : decoded TB bits\n(standard pinned-memory copy,\nNOT CXL)

note bottom of NIC
  Data plane: 100% open
  (mlx5_core + DMA-BUF + rdma-core)
  Control plane: CPU posts receives
  (open rdma-core/DPDK; GDAKI NOT needed)
end note
@enduml
```

![arch_datapath_v3](docs/diagrams/arch_datapath_v3.svg)

---

## 2. [Phase 1] Split choice — why 7.2x, not split 8

```plantuml
@startuml split_choice
title O-RAN split choice: 7.2x (chosen) vs split 8 (future)
skinparam rectangle {
  BackgroundColor #F7F7F7
}
rectangle "SPLIT 7.2x  (CHOSEN)\nRU does: RF + FFT\nGPU/DU does: chan-est, eq, demap, LDPC\n= 'complete L1' means everything the DU owns\nLower fronthaul bandwidth (freq-domain REs)\nMatches real Aerial/cuPHY deployments" as S72 #D4EDDA
rectangle "SPLIT 8  (FUTURE / STRETCH)\nRU does: RF only\nGPU/DU does: FFT + everything else\nMuch higher fronthaul bandwidth\n(raw time-domain IQ)\nOut of scope unless specifically motivated" as S8 #D1ECF1
S72 -[hidden]-> S8
note bottom of S72
  OAI's FHI/xran fronthaul stack supports BOTH —
  choosing 7.2x is a scoping decision, not a
  capability limitation.
end note
@enduml
```

![split_choice](docs/diagrams/split_choice.svg)

---

## 3. [Phase 1] Open NIC->GPU mechanism — layer by layer

```plantuml
@startuml nic_gpu_layers
title The open NIC -> GPU mechanism (corrected: mlx5 is hardware choice, not SW lock-in)
skinparam rectangle {
  BackgroundColor #F7F7F7
}
rectangle "ConnectX NIC silicon\n(hardware only, buy it)" as L1 #D4EDDA
rectangle "mlx5_core / mlx5_ib\nkernel drivers -- OPEN\n(upstream Linux)" as L2 #D4EDDA
rectangle "DMA-BUF kernel framework\nOPEN, vendor-neutral" as L3 #D4EDDA
rectangle "rdma-core + libibverbs\n+ mlx5 provider -- OPEN\n(linux-rdma/rdma-core)" as L4 #D4EDDA
rectangle "ibv_reg_dmabuf_mr()\nOPEN verb -- does the actual pull" as L5 #D4EDDA
rectangle "DPDK mlx5 PMD -- OPEN (BSD)" as L6 #D4EDDA
rectangle "GPU dmabuf export\n(hsa_amd_portable_export_dmabuf /\nCUDA equiv.) -- OPEN" as L7 #D4EDDA
rectangle "DOCA GPUNetIO / GDAKI\n(GPU rings NIC doorbell itself)\nHistorically NVIDIA-only;\nnew limited open release --\nUNVERIFIED, NOT REQUIRED for us" as L8 #F8D7DA

L1 --> L2
L2 --> L3
L3 --> L4
L4 --> L5
L5 --> L6
L6 --> L7
L7 -[#F8D7DA]-> L8 : optional, not on our\ncritical path

note right of L5
  This is the layer that matters:
  packets land in GPU VRAM,
  CPU never touches payload.
end note
@enduml
```

![nic_gpu_layers](docs/diagrams/nic_gpu_layers.svg)

---

## 4. [Phase 1] Hardware staging plan (mechanism -> PHY -> portability -> live)

```plantuml
@startuml hw_staging
title Hardware staging (answers "GCP + AMD/Intel + live UE traffic" honestly)
skinparam defaultTextAlignment center
skinparam rectangle {
  BorderColor black
  BackgroundColor #F7F7F7
}

rectangle "STAGE 1 -- MECHANISM SPIKE\nCheap bare-metal box: 1x ConnectX card\n+ 1x GPU (any vendor), same PCIe root\nPROVE: dmabuf NIC->GPU VRAM ingest works\n(freeze-breaker: if not in <=2wk, fall back\nto CPU-staged ingest)" as S1 #FFF3CD

rectangle "STAGE 2 -- PHY CORRECTNESS\nOAI FHI/xran synthetic eCPRI injector\n(no RF hardware) -> NIC -> GPU\nfull PHY (chan-est/eq/demap/LDPC)\nPROVE: bit-exact vs ground truth oracle" as S2 #D1ECF1

rectangle "STAGE 3 -- MULTI-VENDOR PORTABILITY\nRepeat Stage 1+2 on GCP (NVIDIA)\nand an AMD box/cloud (AMD)\nPROVE: same OpenCL/SYCL code,\nboth vendors" as S3 #D4EDDA

rectangle "STAGE 4 -- LIVE DEMO (optional, later)\nSwap synthetic injector for SDR loopback\nor real UE + real O-RU" as S4 #E2E3E5

S1 --> S2
S2 --> S3
S3 --> S4

note right of S1
  GCP A3-Ultra/A4 ConnectX-7 = candidate
  but UNVERIFIED for external-sender traffic
  (tuned for GPU-to-GPU NCCL fabric) + costly.
  Bare-metal is cheaper and more controllable.
end note
note right of S4
  "GCP + AMD/Intel + live traffic in ONE step"
  is NOT achievable -- GCP has no AMD/Intel GPU.
  This staged plan is the honest path.
end note
@enduml
```

![hw_staging](docs/diagrams/hw_staging.svg)

---

## 5. [Phase 1] PHY<->L2 seam — structural change from v1/v2

```plantuml
@startuml phy_l2_seam
title PHY<->L2 seam: retired BBDev PMD vs the new full-PHY seam
skinparam componentStyle rectangle

package "v1/v2 (retired) -- LDPC lookaside only" #F8D7DA {
  [OAI CPU: owns entire PHY] as OAIv2
  [O-RAN AAL / DPDK-BBDev\n(FEC-lookaside-only interface)] as AAL
  [GPU: LDPC decode ONLY] as GPUv2
  OAIv2 --> AAL
  AAL --> GPUv2
}

package "v3 Phase 1 (current) -- GPU owns entire PHY" #D4EDDA {
  [GPU-resident PHY process\n(entire DU L1)] as GPUv3
  [PHY<->L2 seam\nsimple custom IPC now;\nFAPI-style (SCF FAPI) later\nif production interop needed] as SEAMv3
  [CPU: L2+ only\n(MAC/RLC/PDCP)] as CPUv3
  GPUv3 --> SEAMv3
  SEAMv3 --> CPUv3
}

note bottom of AAL
  AAL/BBDev doesn't apply anymore --
  it's FEC-only. GPU now owns the
  WHOLE PHY, needs a bigger interface.
end note
@enduml
```

![phy_l2_seam](docs/diagrams/phy_l2_seam.svg)

---

## 6. [Phase 2, locked scope] CXL swaps the GPU→CPU leg only

```plantuml
@startuml phase2_cxl_handoff
title Phase 2 (locked, not yet built) -- CXL replaces ONE leg: GPU->CPU handoff
skinparam componentStyle rectangle

package "PHASE 1 (working baseline)" #D4EDDA {
  [GPU: decoded TB bits\nin VRAM] as G1
  [pinned host buffer] as P1
  [CPU reads] as C1
  G1 --> P1 : hipMemcpyAsync /\nclEnqueueReadBuffer\n(free completion event)
  P1 --> C1
}

package "PHASE 2 (locked scope, later)" #FFF3CD {
  [GPU: decoded TB bits\nin VRAM] as G2
  [CXL shared region\n(Type-3)] as X2
  [CPU reads\n(same region)] as C2
  G2 --> X2 : DMA write\n(streaming, ~20 GB/s,\n~658 ns -- CCCL, NVIDIA/CUDA;\nAMD/OpenCL path UNPROVEN)
  X2 --> C2 : needs explicit status-flag/\npolling (no free completion\nevent on raw CXL memory)
}

note bottom of X2
  Everything else is UNCHANGED from Phase 1:
  NIC->GPU ingest, GPU-resident full PHY,
  split 7.2x choice, PHY<->L2 seam.
  Only this one leg moves to CXL.
end note
note bottom of C2
  Research question: does CXL work here,
  and how does it compare to the Phase-1
  pinned-memory baseline?
  No perf claim from QEMU/emulated CXL --
  real hardware only (three-tier method,
  research/feasibility_research.md).
end note
@enduml
```

![phase2_cxl_handoff](docs/diagrams/phase2_cxl_handoff.svg)

---

## Rendering
`./scripts/render_diagrams.sh` regenerates all SVG+PNG from the blocks above. If a diagram and
`ARCHITECTURE_v3.md` disagree, the text wins — update the diagram.
