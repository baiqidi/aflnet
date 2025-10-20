# Overlay Scheduler Added Code Reference

本文件汇总了在集成“候选集合内部排序”功能时新增的核心代码，并对每个主要模块逐段说明其职责、调用路径与关键数据结构，便于快速理解与维护。

## `queue_entry_types.h`
- **`struct queue_entry` 新增字段**
  - `float novelty_score`：缓存叠加调度计算出的新颖度分数，供轮转排序及调试使用。
  - `struct sess_feat *sess_feat`：指向懒加载的会话特征缓存（消息直方图、状态集合、shingle 哈希等），避免重复解析输入文件。

## `overlay_sched.h`
- **`sess_feat_t` 结构**：集中保存每条种子的特征数据，包括：
  - 消息数量 `msg_count` 与 `msg_hists`（逐消息 256 维直方图，已 L2 归一化）。
  - 状态序列 `states`、去重后的状态集合 `state_set` 与哈希签名 `signature`。
  - `shingle_hashes` / `shingle_signature`：k=3 状态 shingle 的哈希集合及聚合签名。
  - 标志位 `built`：指示缓存是否已填充，可在多次调用间复用。
- **`overlay_cluster_mode_t` 枚举**：描述三种候选分组策略（状态集合 / 不分组 / k=3 shingle）。
- **调度相关接口**：声明了特征构建、相似度计算、候选挑选、窗口化遍历、模式切换与启用开关等入口函数，供 `afl-fuzz.c` 集成调用。

## `overlay_sched.c`
### 日志与运行时开关
- 环境变量探测：`overlay_logging_init()` 会按需读取 `AFL_DEBUG_OVERLAY`、`AFL_STAT_OVERLAY`，分别控制标准错误调试日志与 `<out_dir>/overlay_stats.log` 文件的追加写入。
- `overlay_set_enabled()` / `overlay_is_enabled()`：运行时启停调度器，关闭时立即重置窗口状态，回退到 AFLNet 原生队列遍历。
- `overlay_set_cluster_mode()` / `overlay_get_cluster_mode()`：根据 `-G` 参数选择状态集合、不分组或 shingle 聚类模式，调试时会输出当前模式名称。

### 队列窗口管理
- 常量 `OVERLAY_QUEUE_WINDOW=16`：限制一次重排的候选数量，兼顾开销与效果。
- `overlay_queue_prepare_entry()`：在队列条目入窗前清空旧特征缓存，防止复用陈旧数据。
- `overlay_queue_release_entry()`：执行完毕后释放缓存，避免内存泄漏。
- `overlay_queue_reset()` / `overlay_queue_current()`：维护滑动窗口指针，供主循环在调度器关闭、队列切换或周期重启时复位。
- `overlay_pick_from_queue_window()`：
  1. 若调度关闭，直接返回当前节点，并更新下一节点指针。
  2. 当开启时，维护最多 16 条目的窗口数组，调用 `overlay_pick_next()` 得到本轮优先执行的种子。
  3. 将已执行的条目移出窗口，从原始链表补充新成员，并更新 `overlay_queue_next_cur` 供下一轮使用。

### 会话特征缓存
- `overlay_feat_get_or_build()`：
  1. 读取输入文件并按 `region_t` 边界累积每条消息的直方图，完成 L2 归一化。
  2. 从最近的 region 记录提取完整 `state_sequence`，复制到 `states`。
  3. 通过排序与压缩得到无序状态集合 `state_set`，并计算 FNV 风格哈希签名。
  4. 若状态长度≥3，则构造 k=3 shingle 哈希列表，去重后混合生成 `shingle_signature`。
  5. 填充完毕后设置 `built=1`，下次直接复用缓存。

### 相似度与评分
- `histogram_similarity()`：返回两条消息直方图的余弦相似度（单位向量点积）。
- `overlay_seq_similarity()`：
  1. 以贪心方式匹配消息对，每轮选择当前未使用消息中相似度最高的一对。
  2. 将所有匹配的相似度相加，分母使用两序列消息数的较大值，以未匹配消息按 0 计入平均。
- 簇划分辅助函数：`overlay_feature_signature()`、`overlay_feature_key_ptr()` 等根据当前聚类模式返回匹配键。
- `overlay_pick_next()`：
  1. 为候选集合构建/拉取特征，按签名分簇并记录成员索引。
  2. 对每个簇计算所有成员之间的平均相似度，并将 `1 - avg` 存入 `queue_entry::novelty_score`。
  3. 按新颖度降序排列簇内成员，同时记录调试/统计日志。
  4. 通过跨簇轮转（Round-Robin）在各簇间分配执行机会，返回本轮挑选出的条目。
  5. 释放临时分配的索引、得分与排序数组。

### 日志输出
- `overlay_log_debug()`：在调试模式下把轮次、簇信息、候选得分及最终选择写到标准错误。
- `overlay_log_stat()`：在统计模式下以机器可解析格式（键值对）将每轮数据追加到 `overlay_stats.log`。

## `afl-fuzz.c` 集成要点
- 新增 `#include "overlay_sched.h"` 并在队列生命周期中调用：
  - `overlay_queue_prepare_entry()`：队列条目入窗前清理缓存。
  - `overlay_queue_release_entry()`：条目完成后释放缓存。
  - 多处队列重置逻辑补充 `overlay_queue_reset()`，确保滑动窗口与指针同步。
- 选择候选时的接入点：
  - 状态启发式分支在生成候选集合后调用 `overlay_pick_next()`，以叠加调度器确定下一条种子。
  - 常规队列模式使用 `overlay_pick_from_queue_window()` 维护窗口并获取排序结果。
- 命令行接口：
  - `-O on|off` 切换是否启用叠加调度，关闭时完全恢复 AFLNet 原始调度。
  - `-G state|none|shingle` 在运行时选择分簇策略，并通过 `overlay_set_cluster_mode()` 生效。
  - 帮助信息与参数解析新增对应分支，非法输入会触发 `FATAL` 报错。

## 文档更新提示
- `README-DEVELOPERS.md` 与 `docs/overlay_scheduler_next_steps.md` 已补充运行时开关、聚类模式与日志文件的说明，方便开发者按需启用、关闭或调试叠加调度器。

---
如需在代码中对照阅读，推荐结合 `overlay_sched.c` 中的日志信息与 `overlay_stats.log` 输出，快速定位具体的簇划分、评分与轮转过程。
