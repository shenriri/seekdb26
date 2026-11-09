# MiniVectorDB 第六阶段：多路径查询加速、正确性验证与性能实验

> 日期：2026-11-09。基线为上一轮 `MiniVectorDB_official_retest_optimized.zip` 中的源码；本阶段在独立工作目录修改。所有测量均为**本地预备 benchmark**，不是 OceanBase 官方隐藏功能准入或性能评分结果。

## 1. 本轮优化的真正目标

基于前一轮精确 TopK 的 AVX2 保守筛选，本阶段消除三个独立瓶颈：

1. 小表高维向量：重复将查询 float32 转为双精度 SIMD 输入，重复 AVX2 候选距离计算。
2. 大表（8 MiB 缓存无法容纳）：每条查询重走 B+Tree、pread 和逐行反序列化；尤其包含 VARCHAR 和低基数过滤的查询。
3. `ORDER BY id [DESC] LIMIT K`：虽有 B+Tree，原实现仍扫描所有行再进行堆排序。

实现保留精确查询语义，支持可关闭开关、差异测试和同负载 A/B。性能优化不变更 WAL/Manifest 的提交、恢复协议，不预先存放任何查询结果。

## 2. 代码变更

| 文件 | 核心改变 |
|---|---|
| `src/db.cpp` | AVX2/FMA、AVX-512 conservative prune；查询向量一次性 double 预转换；有界大表只读映射扫描；BIGINT/VARCHAR 低基数 posting；事务 WriteSet 合并；PK range + LIMIT 早停；LIMIT 0 直接返回；缓存失败负缓存 |
| `src/disk_store.cpp`、`include/minivectordb/disk_store.h` | read-only `HeapView` (mmap + RAII)、保序的 B+Tree `scan_range()`，可正/逆向及提前终止 |
| `tests/benchmark_large_scan.py` | 相同 DB 的大表 A/B、存储值 float32 的独立 TopK 真值、提交后失效与重启对照 |
| `tests/test_mapped_scan_forced.py` | 强制映射的小数据验证，便于 Sanitizer 在可控时间内覆盖映射代码 |
| `tests/test_pk_range.py` | 有符号 PK 边界、页分裂、谓词、ASC/DESC、LIMIT、事务、重启差异验证 |
| `tests/test_simd_exact.py` | 标量、AVX2、FMA、预处理 AVX2、AVX-512 五路线一致性 |
| `tests/benchmark_pk_range.py` | B+Tree 范围查询性能 A/B |
| `CMakeLists.txt` | 注册新测试（Release 总计 15 组） |

### 2.1 大表映射缓存的可靠性和资源控制

- 小表仍沿用原先 8 MiB 内存连续向量缓存。
- 大表当已提交堆文件达到默认阈值 12 MiB 时，采用 `mmap` **映射现有的已提交 Heap 文件**。无需创建第二份向量文件，不增加每条查询的随机主键查找。
- 只将行 `(PK, 向量字节地址)` 与非向量列的 BIGINT 或 VARCHAR 只读视图加入内存目录，不把所有向量解码成 C++ 对象。
- 元数据限额 16 MiB；已提交 Heap 文件映射大小限制 1 GiB；无法映射、元数据超限、类型不支持时回退已有的正确磁盘扫描路径。
- 初始元数据构建时检查物理行长度和 CRC；每次 COMMIT 发布后立即丢弃旧映射和派生过滤 posting。显式事务内跳过被 WriteSet 更新/删除的旧 PK，并重新合并新版本。
- `mmap` 不是零物理内存：访问映射页会触发 page cache，正式 `memory.peak` **包含页缓存**。本地 RSS 不代表比赛内存分数。
- `MVDB_TEST_FORCE_MAPPED=1` 仅测试用，允许小数据强制走映射路径；正式性能测量不得启用。

### 2.2 向量距离的精确性

- SIMD/FMA 只生成用于**淘汰明显不可能进入前 K 的候选**的平方距离估计；阈值用保守的前向舍入误差裕量。
- 所有可能入选的候选仍由原来的逐分量 `double` 距离例程重算，最终按距离 + PK 排序。
- 不支持 AVX-512 的 CPU 自动回退 AVX2/FMA；不支持 AVX2 时保持标量路径。
- 包含非常接近的距离、完全并列、过滤、更新、回滚的差异测试为五路线 **106/106 条结果相同**。对于隐藏输入，仍需官方准入验证。

## 3. 实际 A/B 数据

### 3.1 默认基准——10K×128、1000 TopK、K=10、50% 等值过滤

上一交付版和本轮默认组合各独立跑三轮、交替顺序、同随机种子、新数据目录，取中位数。

| 指标 | 上一交付版 | 本轮 | 变化 |
|---|---:|---:|---|
| 100 批写入（每批 100 行） | 1.339591 秒 | 1.318732 秒 | 差异较小 |
| 1000 次 TopK | 0.435708 秒 | **0.341263 秒** | **1.2768×** |
| TopK QPS | ~2295 | **~2930** | ~27.7% |
| 进程 I/O（读+写） | 14,290,944 B | 14,295,040 B | 基本持平 |
| 采样进程 RSS | 17,866,752 B | 17,903,616 B | 基本持平 |
| 抽样无过滤/有过滤 Recall@10 | 1.0 / 1.0 | 1.0 / 1.0 | 均不下降 |

原始记录：`tests/final_combined_ab.json`。这里的 I/O 和 RSS **不是**容器 cgroup v2 的 `io.stat` / `memory.peak`。

较小的独立实验显示：相对普通 AVX2，FMA 中位 +5.7%；对 FMA 增加查询预转换约 +17.6%；对预转换 AVX2/FMA 增加 AVX-512 约 +14.8%。它们的负载和噪声不同，**不可直接相乘**，应以最终组合 A/B 为准：

- `tests/fma_ab.json`
- `tests/prepared_ab.json`
- `tests/avx512_ab.json`

### 3.2 大表精确 TopK——25K×128，BIGINT+VARCHAR

20 次相同 TopK（混合无过滤、字符串过滤和组合过滤），每种模式用相同数据库重启两轮。

| 路径 | 第一次 | 第二次 | 中位数 |
|---|---:|---:|---:|
| 原始 B+Tree+行解码 | 6.7155 秒 | 6.9003 秒 | 6.8079 秒 |
| 已提交 Heap 只读映射 | 0.1246 秒 | 0.1242 秒 | 0.1244 秒 |

**约 54.7×，返回结果逐条相同**。本实验也验证了 COMMIT 后映射失效、插入/删除/向量更新和重启后的对照。混合过滤负载不同于赛事默认负载，不应外推该倍数为正式得分。RSS 约由原路径 13.5 MB 增至映射路径 18.1 MB；额外物理页缓存也必须被考虑。

原始数据：`tests/large_scan_mixed_ab_final.json`。

### 3.3 100K×128 的映射和过滤目录

- 已提交行文件 `55,550,000` 字节；映射查询结束时 RSS 采样约 `63–64 MB`。
- 相同 20 次查询、相同持久化数据，关闭/开启低基数 posting 的稳态耗时为 `0.1101 / 0.0539` 秒，**2.04×**。这种 20 次/双样本的测量不足以证明跨硬件的稳定加速。
- 使用独立、按已存 float32 重新计算的全量精确 TopK oracle（覆盖重复向量、PK 并列规则）：**20/20 查询完全一致**。
- 该 100K 测量**没有**与旧全表扫描跑完同规模 A/B；旧路径在更大查询量下曾超时，不能臆造加速比。

原始数据：`tests/large_100k_mapped_only.json`。

### 3.4 主键有序 LIMIT——20K 行，300 次查询

| 路径 | 三轮中位数 |
|---|---:|
| 旧全表扫描后排序 | 2.716478 秒 |
| B+Tree 正逆向范围遍历与早停 | **0.023295 秒** |

**约 116.6×**，三轮返回内容逐条一致。该指标对应主键有序 LIMIT，不能与向量 TopK 倍数叠乘。原始记录：`tests/pk_range_ab.json`。

## 4. 功能和可靠性结果

| 验证 | 本轮实际结果 | 说明 |
|---|---|---|
| 官方固定提交公开用例 | **5/5、32/32** | 自编适配器执行官方 SQL 和断言，不是官方完整 runner |
| Release CTest | **15/15** | 包含新大表差异、强制映射和 PK 边界 |
| ASan/UBSan 选定测试 | **7/7** | 包含 `mapped_vector_forced`、PK 范围、公开用例、事务与 SIMD |
| 100K 独立 TopK Oracle | **20/20** | Float32 存储值 + tie by PK；本地生成数据 |
| Sanitizer 大规模基准 | **未完成（超时）** | 并非通过；已使用可执行的小数据强制映射补充针对性内存安全检查 |

完整记录：`tests/final_release_ctest_phase6.txt`、`tests/final_sanitizer_phase6.txt`、`tests/large_100k_mapped_only.json`。

## 5. 从零编译和重跑

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4
ctest --test-dir build --output-on-failure -j4

# 官方公开用例的本地 JSON 比较适配器（不是官方 run_case.py）
python3 tests/run_official_public.py ./build/mvdb_public_case_adapter ./official_mysqltest

# 单次默认规模性能与独立 numpy 精确真值（本地，不是官方评分）
python3 tests/benchmark.py ./build/minivectordb --n 10000 --d 128 --q 1000 --verify 100

# 复测大表 mmap 和字符串过滤
python3 tests/benchmark_large_scan.py ./build/minivectordb --rows 25000 --queries 20 --varchar

# 100K 映射路径和独立 Oracle（不运行极慢的旧路径）
python3 tests/benchmark_large_scan.py ./build/minivectordb --rows 100000 --queries 20 --varchar --mapped-only

# 关闭硬件优化作差异比较（正式运行不建议随意关闭）
MVDB_SIMD_PRUNE=0 python3 tests/benchmark.py ./build/minivectordb --n 10000 --d 128 --q 1000
MVDB_AVX512=0 python3 tests/benchmark.py ./build/minivectordb --n 10000 --d 128 --q 1000
```

原有代码仍保留 `MVDB_VECTOR_CACHE=0`、`MVDB_VECTOR_FILTER_INDEX=0` 等开发开关，新代码增加：

- `MVDB_MAPPED_SCAN=0`：关闭大表 mmap 路径。
- `MVDB_MMAP_FILTER_INDEX=0`：关闭大表低基数 posting。
- `MVDB_PK_FASTPATH=0`：关闭 B+Tree 有序范围早停。
- `MVDB_FMA_PRUNE=0`、`MVDB_PREPARED_QUERY=0`、`MVDB_AVX512=0`：分别关闭新 SIMD 子路径。
- `MVDB_TEST_FORCE_MAPPED=1`：**仅用于测试**，不得用于正式性能测评。

## 6. 尚未实现与不能夸大的内容

1. **比赛隐藏功能准入与协议**：官方当前公开的 `mysqltest` 是基础用例，不含完整 `run_case.py`、stdin/stdout 正式驱动或隐藏用例。自编协议不等于已正式准入。
2. **正式性能分**：未在官方 CPU/磁盘/容器隔离条件下统计 cgroup v2 `io.stat`、`memory.peak`；无法断言性能排名最优。
3. **磁盘空间回收**：已有离线代际 compact 和 WAL 轮转，但在线可复用槽式堆页和长期碎片自动策略仍未完成。
4. **ANN**：已有 IVF-Flat、事务维护、重启修复，但较少探测桶时召回不足；当前小中型负载的优选仍是 exact 或 ANN 模式回退 exact。
5. **超大数据**：大于 1 GiB 的 Heap 映射会回退原磁盘扫描，16 MiB 元数据上限也会导致 fallback；这些限制保护内存但未实现最优带外内存扫描。
6. **超大未提交事务**：WriteSet 可能突破内存预算，仍需磁盘溢写设计。
7. **Docker & GitHub 提交**：当前环境没有 Docker 引擎，未实际验证镜像；GitHub 集成先前对 `shenriri/seekdb26` 新分支创建返回 403，源码包并未推送至远程。

因此本轮目标是获得**已复现的局部优化最优组合**，而不是宣称在未知环境或全部可能算法中达到数学意义的最优。
