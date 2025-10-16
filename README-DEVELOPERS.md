# AFLNet Developer README / 开发者指南

## 项目简介 / Project Overview
AFLNet is a state-aware greybox fuzzer for network protocol implementations built on top of American Fuzzy Lop (AFL). It replays captured client-server message sequences, mutates them, and leverages coverage plus protocol response feedback to explore complex server state machines. This repository extends AFLNet with an overlay scheduler that extracts per-seed session features, clusters executions by IPSM state signatures, and prioritizes novel seeds via round-robin rotation across clusters. The goal of this document is to help developers bootstrap a working environment, understand the code layout, and contribute changes confidently.

## 环境配置与快速启动 / Environment Setup & Quick Start
1. **Install prerequisites (Ubuntu 20.04/22.04 tested):**
   ```bash
   sudo apt-get update
   sudo apt-get install -y build-essential clang llvm-dev libgraphviz-dev libcap-dev pkg-config python3 python3-pip
   ```
2. **Clone and build AFLNet:**
   ```bash
   git clone https://github.com/aflnet/aflnet.git
   cd aflnet
   make clean all         # builds afl-fuzz and auxiliary tools
   (cd llvm_mode && make) # optional: enable llvm_mode instrumentation
   ```
3. **Run a smoke test and export helper variables:**
   ```bash
   ./afl-fuzz -h | head
   export AFLNET=$(pwd)
   export AFL_PATH=$AFLNET
   export PATH="$AFLNET:$PATH"
   ```
4. **Prepare seeds and target binaries:** follow the tutorials in `tutorials/` (e.g., Live555) or craft your own protocol captures under `testcases/`.

## 项目文件结构 / Repository Layout
```
├── README-DEVELOPERS.md     # (this document)
├── README.md                # Project overview for users & researchers
├── afl-fuzz.c               # Core fuzzer loop with overlay scheduler hook points
├── overlay_sched.c/.h       # Feature extraction, clustering, and round-robin novelty ranking
├── queue_entry_types.h      # Shared queue_entry definition and metadata helpers
├── docs/
│   └── overlay_scheduler_next_steps.md  # Experiment checklist and extended guidance
├── tutorials/               # Protocol-specific walkthroughs (e.g., Live555)
├── llvm_mode/               # LLVM-based instrumentation toolchain
├── dictionaries/            # Example protocol dictionaries
└── testcases/               # Seed corpora for sample targets
```
**Key concepts:**
- *Queue entries* encapsulate recorded sessions plus metadata used by the overlay scheduler.
- *Overlay scheduler* enriches candidate selection without altering AFLNet's existing heuristics.
- *Docs/tutorials* capture reproducible experiments and environment notes.

## 实验准备 / Preparing an Experiment Corpus
1. Capture protocol traces with tools such as `tcpdump` or Wireshark and convert them into AFLNet seed sequences (see `tutorials/live555`).
2. Populate `testcases/<protocol>` with curated seeds and dictionaries under `dictionaries/` to improve mutation quality.
3. Configure target servers to run under deterministic conditions (fixed ports, stable session IDs, reproducible timestamps).
4. Use `ipsm_state_sequences.md` to interpret state logs generated during fuzzing runs.

## 常见问题 (FAQ)
1. **`make` fails because of missing Graphviz headers.** Install `libgraphviz-dev` and rerun the build.
2. **`llvm_mode` build cannot find `llvm-config`.** Set `LLVM_CONFIG` to the appropriate binary (e.g., `export LLVM_CONFIG=llvm-config-14`).
3. **Overlay scheduler reports zero novelty.** Verify that feature caches are populated and candidate windows contain diverse state signatures; run with `AFL_DEBUG_OVERLAY=1` for verbose logging.
4. **How do I enable state-aware mode?** Use `afl-fuzz -E -q 3 -s 3 ...` to activate IPSM-guided heuristics alongside the overlay scheduler.
5. **Where are feature caches stored?** Feature metadata is maintained in-memory within each `queue_entry` and released when entries are pruned.

## 运行测试用例 / Running Tests & Diagnostics
- **Primary build & self-test:**
  ```bash
  make -j$(nproc)
  make test_build
  ```
- **Instrumentation sanity check:**
  ```bash
  ./afl-showmap -o /tmp/map.out -- ./test-instr @@
  ```
- **Overlay scheduler smoke run (replace placeholders):**
  ```bash
  AFL_DEBUG_OVERLAY=1 ./afl-fuzz \
    -d -i testcases/<protocol> -o out-overlay \
    -N tcp://127.0.0.1/<port> -P <protocol_name> [extra AFLNet flags] -- \
    ./path/to/server_binary <server_args>
  ```
- **Static analysis (optional):**
  ```bash
  clang-tidy overlay_sched.c -- -I.
  ```

## 贡献建议 / Contribution Tips
- Use `clang-format` (e.g., `clang-format -i overlay_sched.c`) before committing C/C++ changes.
- Add regression artifacts under `docs/` or `tutorials/` to document new protocol targets.
- Include references to novelty metrics in commit messages when touching the overlay scheduler.

Happy fuzzing!
