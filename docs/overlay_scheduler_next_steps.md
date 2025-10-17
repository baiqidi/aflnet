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
   * `AFL_STAT_OVERLAY=1` 会输出聚类、轮转计数和新颖度分数的统计。

## 3. 实验步骤（Run the experiment）

1. **启动目标服务**（若需要）并记录其监听地址/端口。
2. **运行 AFLNet**：依据协议类型设置 `-N`（传输层）和 `-P`（协议名称），再加上
   `-D` 指定超时时间。例如 fuzz 一个 UDP 协议 `XYZ` 的服务：

   ```bash
   AFL_DEBUG_OVERLAY=1 AFL_STAT_OVERLAY=1 ./afl-fuzz \
     -i seeds_dir -o findings_dir \
     -N udp -P XYZ -D 1000 -- ./target_binary @@
   ```

   * 如果需要 TLS、TCP 或自定义端口，请参考 `README.md` 调整附加参数。
   * 运行后在终端中观察 `overlay: cluster=... novelty=...` 等日志，确认覆盖调度器
     已经接管候选排序流程。
3. **监控运行状态**：
   * 在 AFLNet 主界面关注 `#queue`, `pending_favs` 等指标判断整体进展。
   * 结合 `AFL_STAT_OVERLAY` 输出，查看每个簇的候选数量、当前轮转位置以及被选中
     种子的 `novelty = 1 - avg_sim_all`。
4. **收集中间数据**：若需要更深入分析，可定期复制 `findings_dir/fuzzer_stats` 和
   `overlay_stats.log`（当启用 `AFL_STAT_OVERLAY=1` 时生成）。

## 4. 校验特征提取（Validate feature extraction）

1. 保持 `AFL_DEBUG_OVERLAY=1`，在日志中关注 `msg_count`, `state_count` 等字段。
   它们分别来自 `overlay_extract_messages()` 与缓存的状态序列，可用来核对消息切分
   与直方图统计是否符合预期。
2. 如果发现消息边界异常，检查 `aflnet.c` 中写入 `region_t` 结构的逻辑，确认记录的
   `start_off` / `end_off` 与实际报文对应。

## 5. 分析聚类与轮转结果（Inspect clustering & scheduling）

1. 在 `AFL_STAT_OVERLAY` 输出中比对不同簇的 `signature`；若全部种子落在同一簇，
   可能意味着 IPSM 状态序列完全一致，可以通过增加目标覆盖或手动构造差异输入来
   拉开簇。
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

Following these steps should help you confirm that the implementation matches
the plan and highlight the exact touch points for further experimentation.
