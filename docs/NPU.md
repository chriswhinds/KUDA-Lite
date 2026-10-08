<!--
Copyright 2026 Christopher Hinds, Stratum Labs llc
SPDX-License-Identifier: Apache-2.0 (see the LICENSE file at the project root)
-->

# NPU Model Inference on the Orange Pi 6 Plus: Decision Record and Plan

**Status:** deferred until hardware is available. **Decision date:** 7 October 2026.
**Applies to:** KUDA-Lite 0.3 (the Orange Pi 6 Plus platform, [PLATFORMS.md](PLATFORMS.md)).

This document records why NPU-accelerated model inference was **not** included when Orange Pi 6 Plus support was added. It also covers what is known about the NPU and its software, the design questions still open, and the plan to implement and test it once boards are available.

## 1. Background

The Orange Pi 6 Plus was chosen as the second worker platform partly because it has an NPU. KUDA-Lite 0.3 (pull request #1) added full Orange Pi 6 Plus support, but **all kernels still run on the CPU cores**. The NPU is listed as future work in [PLATFORMS.md §6](PLATFORMS.md#6-the-npu) and the [roadmap](ROADMAP.md).

After the PR was merged, the question was asked: *why was NPU use for model inference left for later rather than done now?* The answer, below, was accepted. **NPU work starts when boards are available, and is then implemented and tested in full.**

Two clarifications from that discussion:

- **Board:** this concerns the **Orange Pi 6 Plus** (CIX P1). The **Orange Pi 5** is a different board: Rockchip RK3588, a 6 TOPS NPU, and a different software stack (RKNN). It isn't a KUDA-Lite target.
- **Model choice:** the base **Orange Pi 6** has the same NPU. What the Plus adds is 2 × 5 GbE and up to 64 GB of RAM, so the Plus remains the right choice for KUDA-Lite.

## 2. The hardware

| Item | Detail |
|---|---|
| SoC | CIX P1 (CD8180/CD8160) |
| NPU | Arm China **Zhouyi** AIPU (V3, 3 cores) |
| NPU throughput | about **30 TOPS** from the NPU alone; the often-quoted **45 TOPS** combines CPU + GPU + NPU |
| Precisions | INT4, INT8, INT16, FP16, BF16, TF32 |
| Kernel driver | `aipu.ko` (version 5.11.0 in the stock image), part of the vendor kernel |
| User-space runtime | NOE runtime, part of CIX's **NeuralONE** AI SDK |

## 3. Why it was deferred

### 3.1 It could not be built or tested without a board

CIX's NeuralONE SDK is only available **pre-installed on the official Orange Pi images**, and the NPU driver ships with the vendor kernel. With no board available, any NPU code would have been unbuildable and untested, written against an API that couldn't be inspected. Everything else in KUDA-Lite was compiled and run before delivery; NPU code could not meet that standard.

### 3.2 The software stack is still maturing

Findings from public sources (§10):

- **Fixed input shapes:** models must be converted ahead of time into the vendor's format with **fixed input shapes**. A model with a dynamic batch size quietly **falls back to the CPU**.
- **Driver portability:** one community project needed **six patches** to the NPU driver to build it for a newer kernel (6.18).
- **Stability:** a published review could not measure Ollama (LLM inference) on the Orange Pi 6 because of **system instability**.
- **No LLM path:** there is **no public NPU back end for llama.cpp** on the Zhouyi NPU. **LLM inference on this NPU is not available today**; LLMs would run on the CPU cores.
- **What does work:** **vision and embedding models** (YOLOv8n, CLIP ViT-B/32, DeepLabv3) from CIX's model collection.

### 3.3 It is a different kind of work from KUDA-Lite's kernels

KUDA-Lite kernels are **general compute**: 32-bit floating-point maths split into blocks, verified against a strict numerical error bound (see [MATMUL.md §5](MATMUL.md#5-verification-method)). An NPU runs a **whole pre-compiled neural-network model** at reduced precision (INT8 or FP16).

So "use the NPU" is not a switch on the existing kernels. It means a new operation, *run model X on this batch*, and several design decisions (§5) that belong to the project owner.

### 3.4 Process note

Leaving the NPU out was flagged in the pull request, but it should have been **raised as a question up front** rather than decided during implementation. This record fixes that, and future scope decisions of this kind will be asked first.

## 4. Proposed design

### 4.1 Inference as a cluster operation

1. **Model registry on workers.** Each worker loads pre-compiled models from a model directory (e.g. `/var/lib/kudalite/models/<name>/`). The controller learns at registration which models each worker can run, and on which device (NPU or CPU).
2. **Host API**, for example:

   ```cpp
   clError_t clModelLoad(const char* name, clModel_t* model);
   clError_t clInfer(clStream_t stream, clModel_t model,
                     clDevPtr input, clDevPtr output, uint32_t batch);
   ```

   Inputs and outputs live in KUDA-Lite global memory, like any kernel's data, so the existing copy, stream and event machinery applies unchanged.
3. **Scheduling: data-parallel by default.** The controller splits the batch into slices and hands them to workers' NPUs, the same way it hands out blocks today. Each slice must match the model's **fixed compiled batch size** (§3.2), so the scheduler works in whole multiples of it.
4. **CPU fallback.** Boards without an NPU (Raspberry Pi 5, or an Orange Pi whose NPU stack is missing) run the same model on the CPU, for example via ONNX Runtime. Mixed clusters keep working, just more slowly on those nodes.
5. **Memory model unchanged.** The worker gathers inputs from global memory into NPU-visible buffers, runs the NPU job, and writes results back. The NPU never touches global memory directly.

### 4.2 Telemetry and dashboard

- Per-node NPU utilisation, inference count and latency, appended to the telemetry sample as version 3 (the existing [compatibility rule](OBSERVABILITY.md#4-wire-format) covers this).
- The dashboard shows NPU busy state alongside the compute threads.

### 4.3 LLMs

Until the vendor provides an LLM path for this NPU, LLMs run on the **CPU cores**, e.g. llama.cpp per node, with KUDA-Lite distributing requests. If a supported NPU path appears, it plugs into the same `clInfer` design.

## 5. Decisions needed from the project owner

| # | Decision | Options | Notes |
|---|---|---|---|
| D1 | Which models | Vision (detection, segmentation), embeddings, LLMs, other | Vision and embeddings are supported on the NPU today; LLMs are not |
| D2 | How work is split | Data-parallel (whole model per worker, batch split) **or** model split across boards | Data-parallel is simpler and scales well; splitting a model across boards is much harder |
| D3 | How models reach workers | Pre-installed by the deploy scripts, **or** pushed by the controller | Pre-installed is simpler; pushing needs model signing (security) |
| D4 | CPU fallback | Required for mixed clusters, or Orange Pi-only inference | Fallback keeps Pi 5 workers useful |
| D5 | Accuracy acceptance | Per-model tolerance vs the FP32 reference | Reduced precision changes results; the tests need a defined tolerance |

## 6. Plan

### Phase 0: hands-on check (one board, before writing code)

Run on an Orange Pi 6 Plus with the **official Orange Pi image**:

| # | Check | Pass / record |
|---|---|---|
| N0.1 | Identify the installed NeuralONE SDK, NOE runtime and `aipu.ko` versions | Record versions |
| N0.2 | Run a model-hub sample (YOLOv8n or CLIP) once | Produces correct output |
| N0.3 | Run the same model in a loop for 1 hour | **No hangs**, no driver errors in `dmesg` |
| N0.4 | Measure throughput (inferences/s) and latency at the compiled batch size | Record |
| N0.5 | Measure board power during N0.3 | Record; update [POWER_OPI6PLUS.md](POWER_OPI6PLUS.md) |
| N0.6 | Convert one of *our* chosen models (D1) with the SDK | Converts; runs on the NPU, not falling back to the CPU |
| N0.7 | Review the SDK and runtime licence terms for redistribution and use | Licence compatible with KUDA-Lite (Apache-2.0) deployment |

**Go/no-go:** if N0.3 shows hangs or N0.7 rules out use, NPU work stays deferred and we look at vendor fixes or a different approach.

### Phase 1: implementation (with boards)

1. Worker model registry and NPU executor, with ONNX Runtime CPU fallback.
2. Host API (`clModelLoad`, `clInfer`) and controller scheduling in fixed-batch slices.
3. Deployment: an install option for model files; detection of the NPU runtime in `install.sh`.
4. Tests: unit tests with a simulated NPU back end; integration tests on the local simulated cluster (CPU fallback path); and **hardware tests on the boards** (§7).

### Phase 2: observability

NPU telemetry (version 3), dashboard display, `cl-top` column.

### Optional head start (no hardware needed)

The host API, scheduler changes, CPU-fallback path and a simulated NPU back end can be built and fully tested on a PC before boards arrive. Only the real NPU executor needs hardware. This would be clearly marked "NPU path not yet hardware-tested" until Phase 0 and Phase 1 hardware tests pass.

## 7. Hardware acceptance tests (NPU)

To be added to [TESTING.md §4](TESTING.md#4-hardware-acceptance-plan) when implemented:

| # | Test | Pass criterion |
|---|---|---|
| N1 | `clInfer` on one Orange Pi 6 Plus, chosen model | Output within the D5 tolerance of the FP32 CPU reference |
| N2 | Batch split across 4 workers | Same results as N1; throughput ≥ 3× a single worker |
| N3 | Mixed cluster: Orange Pi NPU workers + Pi 5 CPU-fallback worker | Correct results; slower node takes fewer slices |
| N4 | 8-hour inference soak | No hangs; no growth in worker memory |
| N5 | Fault injection: kill a worker mid-batch | Host gets an error; the rest of the cluster stays up |
| N6 | Dashboard | NPU busy state and inference rate shown per node |

## 8. What is needed to start

- **Hardware:** at least one Orange Pi 6 Plus running the official Orange Pi image, either with the hands-on output sent back, or with SSH access to the board. Four boards are needed for N2–N5.
- **Decisions:** D1–D5 (§5), at least D1 (which models) before Phase 1.

## 9. Related documents

- [PLATFORMS.md](PLATFORMS.md): Orange Pi 6 Plus platform support, §6 the NPU
- [ROADMAP.md](ROADMAP.md): NPU back end
- [POWER_OPI6PLUS.md](POWER_OPI6PLUS.md): power estimates (to be updated with NPU measurements)
- [TESTING.md](TESTING.md): hardware acceptance plan

## 10. Sources

- CNX Software, [Orange Pi 6 Plus: CIX P1 SBC with up to 64 GB LPDDR5 and 45 TOPS](https://www.cnx-software.com/2025/10/15/orange-pi-6-plus-cix-p1-sbc-64gb-lpddr5-45-tops-ai-performance/)
- CIX Technology, [developer site (NeuralONE AI SDK)](https://developer.cixtech.com/)
- visorcraft, [orange-pi-6-plus-npu](https://github.com/visorcraft/orange-pi-6-plus-npu): `aipu.ko` 5.11.0, six patches for kernel 6.18, NOE runtime, models from the CIX model hub, no llama.cpp NPU back end
- Radxa, [DeepLabv3 on the CIX NPU](https://docs.radxa.com/en/orion/o6/app-development/artificial-intelligence/deeplab_v3): model conversion; fixed input shapes; dynamic batch falls back to the CPU
- avafinger, [orangepi-6-plus-experiments](https://github.com/avafinger/orangepi-6-plus-experiments): NPU experiments on the Orange Pi 6 Plus
- n4hy, [NPU_OrangePi6Plus](https://github.com/n4hy/NPU_OrangePi6Plus): community NPU work on the Orange Pi 6 Plus
- bret.dk, [Orange Pi 6 initial thoughts](https://bret.dk/orange-pi-6-initial-thoughts/): Ollama could not be measured because of system instability
- Boiling Steam, [Orange Pi 6 Plus review](https://boilingsteam.com/orange-pi-6-plus-review/)
