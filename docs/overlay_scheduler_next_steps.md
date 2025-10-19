# Overlay Scheduler Integration Checklist

This checklist summarizes the follow-up work required to exercise the
novelty-based candidate ordering that now ships with AFLNet.  It follows the
original proposal step-by-step so you can verify that the implementation is
behaving as intended and understand where to extend it next.

## 1. 环境配置（Environment setup）

完成一次覆盖调度器实验之前，先准备一个干净的 Linux 主机或容器，推荐使用基于
Debian/Ubuntu 的发行版。执行以下命令安装构建 AFLNet 所需的基础依赖：

```bash
sudo apt-get update
sudo apt-get install -y build-essential libgraphviz-dev clang pkg-config \
  python3 python3-venv python3-pip git
```

> **提示**：`libgraphviz-dev` 是链接 `afl-fuzz` 时必需的头文件；缺失时会在构建阶段
> 出现 “missing graphviz development libraries” 的报错。

随后克隆或更新 AFLNet 代码并完成编译：

```bash
git clone https://github.com/aflnet/aflnet.git
cd aflnet
make -j"$(nproc)"
```

如果你在已有仓库内工作，只需执行 `git pull` 更新并重新编译即可。

## 2. 实验准备（Prepare inputs & targets）

1. **待测服务**：准备好目标协议实现，例如一个监听在本地端口的 UDP/TCP 服务。
   * 建议通过 systemd、supervisor 或手工终端启动，确保能够在 fuzzing 期间稳定运行。
2. **初始语料（`-i`）**：收集或编写最少量的有效输入消息，放入一个目录，例如
   `./seeds_dir`。这些样例应能触发 IPSM 跟踪，从而生成状态序列。
3. **输出目录（`-o`）**：创建一个空目录用于保存 AFLNet 运行过程中的中间结果，
   如 `./findings_dir`。
4. **环境变量**：决定是否启用调试输出：
   * `AFL_DEBUG_OVERLAY=1` 会打印每次候选排序的详细日志。
   * `AFL_STAT_OVERLAY=1` 会把聚类、轮转计数和新颖度分数追加到
     `<out_dir>/overlay_stats.log`，方便离线分析。
5. **聚类模式**：运行时通过 `-G` 选择候选分类策略：
   * `-G state`（默认）按状态集合去重。
   * `-G none` 跳过聚类，直接在整体集合上计算新颖度。
   * `-G shingle` 依据状态序列的 k=3 连片哈希分簇。

## 3. 实验步骤（Run the experiment）

1. **启动目标服务**（若需要）并记录其监听地址/端口。
2. **运行 AFLNet**：依据协议类型设置 `-N`（传输层）和 `-P`（协议名称），再加上
   `-D` 指定超时时间。例如 fuzz 一个 UDP 协议 `XYZ` 的服务：

   ```bash
   AFL_DEBUG_OVERLAY=1 AFL_STAT_OVERLAY=1 ./afl-fuzz \
     -i seeds_dir -o findings_dir \
     -N udp -P XYZ -D 1000 -G state -- ./target_binary @@
   ```

   * 如果需要 TLS、TCP 或自定义端口，请参考 `README.md` 调整附加参数。
   * 运行后在终端中观察 `overlay: cluster=... novelty=...` 等日志，确认覆盖调度器
     已经接管候选排序流程。不同的 `-G` 模式会在日志的 `mode=` 字段中体现。
3. **监控运行状态**：
   * 在 AFLNet 主界面关注 `#queue`, `pending_favs` 等指标判断整体进展。
   * 结合 `AFL_STAT_OVERLAY` 输出，查看每个簇的候选数量、当前轮转位置以及被选中
     种子的 `novelty = 1 - avg_sim_all`。
4. **收集中间数据**：若需要更深入分析，可定期复制 `findings_dir/fuzzer_stats` 和
   `findings_dir/overlay_stats.log`（当启用 `AFL_STAT_OVERLAY=1` 时生成）。

## 4. 校验特征提取（Validate feature extraction）

1. 保持 `AFL_DEBUG_OVERLAY=1`，在日志中关注 `msg_count`, `state_count`、`key=` 等
   字段。它们分别来自 `overlay_extract_messages()`、缓存的状态序列以及当前聚类键
   （状态集合或 shingle 数量），可用来核对消息切分与直方图统计是否符合预期。
2. 如果发现消息边界异常，检查 `aflnet.c` 中写入 `region_t` 结构的逻辑，确认记录的
   `start_off` / `end_off` 与实际报文对应。

## 5. 分析聚类与轮转结果（Inspect clustering & scheduling）

1. 在 `AFL_STAT_OVERLAY` 输出中比对不同簇的 `signature` 与 `key_len`。若全部种子落
   在同一簇，可能意味着在当前 `-G` 模式下的聚类键完全一致，可以通过切换到其他
   模式或手工构造差异输入来拉开簇。
2. 观察 `overlay_rr_pos` 或类似字段，确认轮转指针按簇依次推进。构造“多 vs. 少”
   种子簇的对比实验，验证轮转不会让小簇长期饥饿。

## 6. 后续扩展（Next extensions）

* If you plan to experiment with alternative similarity measures, swap out
  `overlay_seq_similarity` with the desired routine.  The rest of the pipeline
  (clustering + round-robin) can remain untouched.
* To persist novelty scores across restarts, serialize `queue_entry->novelty`
  alongside the other metadata when saving the queue state.  The current code
  rebuilds the cache on demand.
* The stub `overlay_queue_pick_from_queue_window()` already exposes a queue
  window helper; wire it into custom scheduling strategies if you need to reuse
  the novelty ordering outside of `pick_next_entry()`.

## 7. 复用现有输出目录（Resume from an existing findings dir）

Recommendation

Reuse the existing fuzzing output directory by launching AFLNet in "in-place resume" mode: pass -i- (a literal dash) so the fuzzer treats the directory named by -o as both the output location and the source of prior state. AFLNet’s own documentation shows this exact pattern: ./afl-fuzz -i- -o existing_output_dir [...etc...].

Ensure the previous run has stopped before resuming, because the resume code temporarily moves files under _resume/ inside that directory while it restores the queue and metadata. Starting multiple fuzzers against the same output tree at once can corrupt the corpus.

Your revised command would therefore look like:

```bash
AFL_DEBUG_OVERLAY=1 AFL_STAT_OVERLAY=1 timeout 1h \
  /home/hxq/Documents/AFLnet-sort/aflnet/afl-fuzz -d \
  -i- \
  -o /home/hxq/Documents/AFLnet-sort/live555/testProgs/out-live555 \
  -N tcp://127.0.0.1/8554 \
  -x $AFLNET/tutorials/live555/rtsp.dict \
  -P RTSP -D 10000 -q 3 -s 3 -E -K \
  -R ./testOnDemandRTSPServer 8554
```

This tells AFLNet to resume directly in the existing out-live555 directory instead of creating a new one, while leaving the rest of your parameters untouched.

If you need to archive the old results before resuming, create a backup copy first; the resume process may rename files as it reconstructs the queue, so keeping a snapshot ensures you can roll back if necessary.

Following these steps should help you confirm that the implementation matches
the plan and highlight the exact touch points for further experimentation.
