# AFLNet 源码文件导览

## 项目定位
AFLNet 在 AFL 的基础上增加了网络协议状态反馈，引导模糊测试客户端对服务器进行消息序列变异，以同时覆盖代码和协议状态空间。【F:README.md†L1-L98】

## 核心模糊测试引擎
| 文件 | 作用概述 |
| --- | --- |
| `afl-fuzz.c` | AFL/AFLNet 的主程序，包含状态图(`ipsm`)初始化、状态反馈位图维护、种子与状态调度等逻辑，是实际执行模糊循环的入口。【F:afl-fuzz.c†L401-L520】【F:afl-fuzz.c†L9231-L9402】 |
| `aflnet.h` | 定义协议消息区域(`region_t`)、状态信息(`state_info_t`)等核心数据结构，并声明所有协议解析、网络通信、消息序列操作等 AFLNet 扩展接口。【F:aflnet.h†L9-L165】 |
| `aflnet.c` | 实现协议请求分段、响应状态码提取、网络收发包装器等功能，为 AFLNet 提供状态反馈、消息级变异和目标服务器通信能力。【F:aflnet.c†L1-L112】【F:aflnet.c†L2520-L2597】 |
| `aflnet-replay.c` | 独立的复现工具，按记录的消息序列重放触发崩溃或异常，并使用协议解析函数打印服务器状态响应，方便问题定位。【F:aflnet-replay.c†L1-L162】 |

## 编译与插桩工具链
| 文件 / 目录 | 作用概述 |
| --- | --- |
| `afl-gcc.c` | GCC/Clang 包装器，负责定位 `afl-as`、注入必要的编译参数，并支持开启硬化或 ASAN 等选项，实现源码插桩编译流程。【F:afl-gcc.c†L18-L159】 |
| `llvm_mode/afl-llvm-pass.so.cc` | LLVM pass，配合 `afl-clang-fast` 在 IR 级别写入共享内存覆盖记录、偏移映射调整等逻辑，是 AFL/AFLNet 的 LLVM 插桩实现。【F:llvm_mode/afl-llvm-pass.so.cc†L18-L183】 |
| `qemu_mode/` | 提供基于 QEMU User 模式的二进制插桩方案，配套脚本可构建补丁版 QEMU 以在闭源程序上产生覆盖反馈。【F:qemu_mode/README.qemu†L1-L125】 |

## 辅助分析与语料处理工具
| 文件 | 作用概述 |
| --- | --- |
| `afl-analyze.c` | 通过对输入字节逐位扰动观察路径变化，推测文件结构与字段属性，帮助理解待测协议格式。【F:afl-analyze.c†L18-L116】 |
| `afl-showmap.c` | 运行目标并打印共享内存位图，支持人类可读或二进制格式，常用于去重或检查插桩覆盖情况。【F:afl-showmap.c†L18-L114】 |
| `afl-cmin` | Bash 脚本，实现覆盖驱动的语料最小化，可配合 `afl-tmin` 精简种子集。【F:afl-cmin†L1-L147】 |
| `dictionaries/` | 存放预置输入字典，配合 `-x` 选项让模糊器快速组合协议关键字，并说明如何自定义语法片段。【F:dictionaries/README.dictionaries†L1-L43】 |

## 运行时辅助库
| 目录 | 作用概述 |
| --- | --- |
| `libdislocator/` | 提供替换 malloc/calloc 的“滥用分配器”，通过加页、金丝雀和 OOM 检测帮助触发越界类漏洞。【F:libdislocator/libdislocator.so.c†L1-L200】 |
| `libtokencap/` | Hook `strcmp`/`memcmp` 等函数，捕获常量字符串 Token，生成自动字典以提升变异效率（Linux 专用）。【F:libtokencap/libtokencap.so.c†L1-L200】 |

## 文档与教程资源
* `README.md`：总体介绍、安装、使用示例与 Live555 教程，适合快速了解 AFLNet 的能力与操作流程。【F:README.md†L1-L200】
* `docs/`：保留 AFL 文档（快速上手、性能调优等），可与主 README 结合查阅实际参数含义。

## 如何扩展协议支持
* 在 `aflnet.h` 中声明新的请求划分与响应解析函数，遵循既有命名约定。【F:aflnet.h†L73-L114】
* 在 `aflnet.c` 中实现相应逻辑，必要时扩展消息分片与状态码映射算法。【F:aflnet.c†L1-L112】
* 在 `afl-fuzz.c` 的选项解析中注册协议枚举值，使 `-P` 参数能够调用新的解析器，并确保状态机调度逻辑可取得反馈。【F:afl-fuzz.c†L401-L520】
