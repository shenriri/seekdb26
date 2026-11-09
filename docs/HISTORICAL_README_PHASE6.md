# MiniVectorDB — 第六阶段性能优化版（2026-11-09）

**新增结果：** 官方公开基础用例 **5/5，32/32 SQL**；Release **15/15**；选定 ASan/UBSan **7/7**。上一交付版与本轮默认 10K×128 的三轮交替 A/B：1000 次 TopK 中位数由 **0.435708s → 0.341263s**（**1.277×**）；内存和进程 I/O 基本持平。25K×128 磁盘大表混合条件 **54.7×**，20K 行 PK 排序 LIMIT **116.6×**，均有独立对照与正确性检查。**不是官方隐藏测评或性能排名。**

详细方案、风险、A/B 数据、命令及代码变更见 **`docs/PHASE6_PERFORMANCE_AND_VALIDATION.md`**。正式评测的运行协议、Docker 和 cgroup 指标仍待验证。

---

# MiniVectorDB — 官方公开用例复测 + AVX2 保守 TopK 加速（2026-11-09）

**状态：** 可独立编译的 C++17 技术原型；官方公开基础用例 **5/5，32/32 SQL**；本地 Release 回归 **12/12**。尚未通过赛事隐藏准入测试，亦未拿到完整官方评测运行器、性能基线及 Docker 镜像验证结果。

## 官方测试集来源与边界

- 官方位置：<https://github.com/oceanbase/seekdb/tree/a3dba559c1e7ebdc48bc990ffb1a4c75d1c1075c/mysqltest>
- 固定提交：`a3dba559c1e7ebdc48bc990ffb1a4c75d1c1075c`
- 官方公开包版本：`minivectordb-public-basic-v1`
- `official_mysqltest/cases/*.test` 已与 GitHub 官方 Git Blob SHA **逐字节核对**。
- `official_mysqltest/cases/*.result` **保留官方字段与值，经过 JSON 空白重排**，不宣称与官方原始文件字节一致。实际断言依据官方 `README.md` 中的字段规则，包括 `affected_rows`、`columns`、`rows`、`unordered` 和浮点容差 `1e-5`。
- 官方特别说明：该目录**不包含**完整测试驱动 `run_case.py`、崩溃重启用例、隐藏负载、性能基线或评测通信协议。因此这里的执行器是自行编写的结构化**本地适配器**，不是官方运行器。公开测试通过不能代替正式 MySQL case 准入。

| 官方用例 | 数量 | 本地断言通过 |
|---|---:|---:|
| sql_crud | 9 | 9 |
| sql_comparisons | 7 | 7 |
| sql_ordering | 7 | 7 |
| sql_empty | 4 | 4 |
| vector_euclidean | 5 | 5 |

## 对上阶段代码的更改

1. 将 `Result.affected_rows` 加入数据库核心；Insert 按成功插入行数，Update/Delete 按匹配到的行数返回，不再只返回普通 `OK`。该字段现在不依赖临时的 `main.cpp` 协议。
2. 增加开发专用的 `mvdb_public_case_adapter`，逐条输出 JSON，并用 `tests/run_official_public.py` 执行官方公开样例；不声称它是赛事指定 stdout 协议。
3. 加入自适应 AVX2 保守上界筛选（`MVDB_SIMD_PRUNE=1` 默认）。仅用于 `exact` 模式下符合条件的小表、事务外、向量 TopK 路径；所有可能进入 TopK 的候选继续用**原有标量 double 欧氏距离**重算。对于无法判断安全排除的候选不跳过，因此不以降低 Recall 换取速度。
4. 对无 AVX2 的机器，自动保留标量计算。设置 `MVDB_SIMD_PRUNE=0` 可关闭新优化，便于同二进制公平 A/B。
5. 新增 `test_simd_exact.py`：随机向量、等距/接近等距、过滤、事务、更新、删除与 rollback 下的 SIMD/标量结果一致性。

## 编译和复测

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4
# 仅运行官方公开 SQL 的本地适配测试（不是官方 runner）
python3 tests/run_official_public.py ./build/mvdb_public_case_adapter ./official_mysqltest
# 完整本地回归
ctest --test-dir build --output-on-failure -j3
# 精确 TopK 独立性能 A/B（可分别重复 3 次）
MVDB_SIMD_PRUNE=0 python3 tests/benchmark.py ./build/minivectordb --n 10000 --d 128 --q 1000 --verify 100
MVDB_SIMD_PRUNE=1 python3 tests/benchmark.py ./build/minivectordb --n 10000 --d 128 --q 1000 --verify 100
```

编译无需下载 SQL 解析器、向量库或第三方数据库引擎。测量脚本需要 Python 的 `numpy` 和 `psutil`，但 C++ 程序不依赖这些包。Dockerfile 已更新以包含新增本地测试适配器源文件，但此环境没有 Docker 引擎，镜像构建未实测。

## 最终三轮交替基准（本地、非官方评分）

默认 10,000 行，128D float32，100 批每批 100 INSERT，1000 次 K=10 查询，一半含普通列等值过滤，30 次预热。使用固定随机种子 20261109、同一个可执行程序，仅改变环境变量以开关优化。每轮分别从空数据目录开始。

| 指标（三轮中位） | 关闭 SIMD | 开启 SIMD |
|---|---:|---:|
| 写入耗时 | 1.400719 s | 1.361392 s |
| TopK 查询耗时 | 0.945611 s | **0.442323 s** |
| 查询吞吐 | 约 1,057 QPS | **约 2,261 QPS** |
| 进程读写 I/O 观测 | 14,290,944 B | 14,290,944 B |
| 采样进程 RSS | 17,858,560 B | 17,870,848 B |
| 抽样 Recall@10，无过滤 | 1.0 | 1.0 |
| 抽样 Recall@10，有过滤 | 1.0 | 1.0 |

查询中位耗时加速比 **2.1378x**，保留三轮逐项原始数据于 `tests/final_ab/`。两组 Recall 分别在每轮总计 100 次抽样查询中检查，绝非隐藏用例的全量检验。写入、中位 I/O、内存没有明显的长期收益。本地 `/proc` 进程级指标**不等于**赛事规定的 cgroup v2 `io.stat` 及 `memory.peak`（它们包含页缓存、索引准备和写回）。

## 回归与已知限制

- `tests/final_release_ctest.txt`：Release **12/12** 通过，包括 B+Tree 深层压力、WAL 恢复、ANN、压缩和有界内存。
- `tests/official_round1_sanitizer_small.txt`：ASan + UBSan **7/7 选定套件**通过，包括官方公开断言与 SIMD 差异测试。更大范围的 9 项 Sanitizer 测试中途超时，不得计为全部通过。
- `tests/final_ab/summary.json`：最终三轮测量原始数据与中位数。
- 该 AVX2 优化主要适用于 **8 MiB 向量扫描缓存可容纳**的表，超过缓存预算走原有磁盘扫描路径；未解决大表精确 TopK 的高扫描成本。
- 已实现的磁盘 IVF-Flat 在以前随机 128D 数据上低探测数 Recall 未达 0.95；本次性能比较是 `exact` 路径，不得宣称 ANN 加速。
- 本地未运行赛事未提供的隐藏用例、完整协议、Docker 镜像构建或 cgroup v2 三项正式指标。远程 `shenriri/seekdb26` 依然没有进行推送；此前 GitHub 集成写入被 403 拒绝。
- 赛题要求同一提交同时包含源码、Dockerfile、以及真实且脱敏的 `agent/conversation.json`；本文件里记录了真实用户提示和工具实验，未将 Agent 自行分析冒充学生独立学习成果。
