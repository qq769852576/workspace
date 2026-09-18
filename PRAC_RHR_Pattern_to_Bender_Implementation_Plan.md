# PRAC/RHR Pattern 到现有 Bender 的实现方案

## 1. 方案结论

在当前需求边界下，**第一阶段不需要修改 Bender 的 ISA、Pipeline、Adapter 或 DRAM 命令执行链路**。

建议在 Host 端新增一个 `Pattern Compiler`，将 PRAC/RHR 评估工具中的 Pattern 参数转换为：

1. 有限长度的地址访问模板；
2. 现有 Bender 支持的循环、计数和 DRAM 指令；
3. 可复现的二进制指令文件；
4. 用于验收的 Pattern 统计报告。

整体原则是：

> 不逐条复现原工具生成的超长 ACT sequence，而是保持每个评估窗口中的 ACT 总量、地址集合、访问比例、权重、background 比例和必要的时序边界。

该方案能够保留现有 Bender 已经跑通的执行路径：

```text
Pattern 配置
    ↓
Host Pattern Compiler
    ↓
Bender 汇编/二进制指令
    ↓
Frontend → Pipeline → Adapter
    ↓
DRAM
```

Pattern 8 `parallel_access` 按需求方回复不纳入本阶段。

---

## 2. 已确认的需求边界

本方案基于以下已确认信息：

| 项目 | 已确认结论 | 对方案的影响 |
|---|---|---|
| Pattern 用途 | 为 PRAC/RHR 逻辑构造不同访问分布和压力条件 | 验收重点是统计特征和窗口压力，不是经典 RowHammer 物理拓扑 |
| 地址排序 | 原文档中的最终排序已不需要 | Compiler 不再对地址排序，保留生成后的访问顺序 |
| 完整序列 | 不要求逐条严格复现 | 允许有限模板循环和 Host 端预生成 |
| 参考论文/Testcase | 暂无 | 以双方确认的指标作为第一阶段验收基准 |
| Pattern 8 | 不用考虑 | 从第一阶段实现列表中删除 |

### 2.1 第一阶段保持不变的 Bender 模块

- Instruction format / ISA；
- Instruction BRAM 及取指方式；
- `LI`、`ADD`、`BL` 等控制指令；
- `ACT`、`PRE`、`REF`、`WAIT/NOP` 等 DRAM 操作；
- Frontend、Pipeline、Adapter；
- Host 到 FPGA 的二进制指令传输链路；
- Readback 和现有运行控制流程。

### 2.2 第一阶段允许新增的软件内容

- Pattern YAML/JSON 配置；
- Pattern 生成器；
- 有限模板压缩与循环规划；
- Bender 汇编/二进制生成；
- 静态合法性检查；
- Pattern 统计与验证报告。

---

## 3. 总体架构

```text
┌──────────────────────────────┐
│ Pattern 配置                 │
│ 类型、窗口、地址、权重、seed │
└──────────────┬───────────────┘
               ↓
┌──────────────────────────────┐
│ Host Pattern Generator       │
│ 生成逻辑访问序列/配额模板    │
└──────────────┬───────────────┘
               ↓
┌──────────────────────────────┐
│ Pattern Normalizer           │
│ 地址检查、比例量化、窗口切分 │
└──────────────┬───────────────┘
               ↓
┌──────────────────────────────┐
│ Bender Program Compiler      │
│ 模板、循环、WAIT、ACT/PRE    │
└──────────────┬───────────────┘
               ↓
┌──────────────────────────────┐
│ 现有 Assembler/Binary Tool   │
└──────────────┬───────────────┘
               ↓
┌──────────────────────────────┐
│ 现有 Bender 硬件链路         │
└──────────────────────────────┘
```

### 3.1 职责划分

| 层级 | 职责 | 不负责的内容 |
|---|---|---|
| Pattern Generator | 根据配置生成地址分布、权重和 background 混合 | 不关心 Bender 指令编码 |
| Normalizer | 将抽象地址映射为合法 Bank/Row，量化比例并划分窗口 | 不下发 DRAM 命令 |
| Bender Compiler | 将访问模板转为现有指令、循环和等待 | 不在 FPGA 上计算概率 |
| Bender Hardware | 按指令执行访问并回传结果 | 不理解 `grid`、`weighted` 等 Pattern 名称 |

这使 Pattern 逻辑和 Bender 硬件解耦。以后新增 Pattern 时，通常只需修改 Host 软件。

---

## 4. Pattern Descriptor 设计

建议所有 Pattern 统一使用一个配置入口。示例：

```yaml
pattern:
  name: weighted_random_access
  seed: 20260918

execution:
  act_rounds: 10
  groups_per_round: 4
  accesses_per_window: 16512
  template_size: auto
  replay_mode: quota_shuffle

address:
  bank: 0
  row_min: 0
  row_max: 32767
  seed_num: 5
  scale_factors: [2, 3, 4, 5]

weight:
  values: [1, 2, 3, 4, 5]

background:
  enable: true
  placeholder_count: 16
  ratio: 0

timing:
  act_to_pre_cycles: auto
  pre_to_act_cycles: auto
  window_cycles: auto

output:
  assembly: true
  binary: true
  report: true
```

### 4.1 必须记录的公共参数

| 参数 | 说明 |
|---|---|
| `pattern.name` | Pattern 类型 |
| `pattern.seed` | 随机种子，用于复现 |
| `accesses_per_window` | 单个评估窗口的目标 ACT 数 |
| `act_rounds` | 执行轮数 |
| `groups_per_round` | 每轮分组数量 |
| `template_size` | 有限模板长度，由容量检查自动确定或手动指定 |
| `bank` | 目标 Bank |
| `row_min/row_max` | 合法 Row 范围 |
| `window_cycles` | 评估窗口长度；如果需求只看事件间 ACT 数，也应在报告中明确 |
| `replay_mode` | 模板生成模式 |

### 4.2 两种模板生成模式

#### A. `seeded_random`

使用固定 seed 随机生成模板。相同配置和 seed 必须产生完全相同的模板和二进制文件。

适用场景：希望保留随机抽样特征。

#### B. `quota_shuffle`（第一阶段推荐）

先根据比例计算每种地址应出现的次数，再将模板打乱。例如目标比例为：

```text
A:10%，B:20%，C:30%，D:40%
```

长度为 100 的模板中分别放置 10、20、30、40 个访问，再使用固定 seed 打乱。

优点：

- 模板较短时仍能精确控制比例；
- 循环执行不会积累统计偏差；
- 适合 PRAC/RHR 压力测试；
- 可复现、易调试。

限制：它不是 FPGA 在线产生的无限独立随机序列。

---

## 5. 现有 Bender 的实现方式

### 5.1 确定性 Pattern

对于固定步长、固定权重或固定 ACT 数的 Pattern，优先使用现有寄存器和循环：

```text
初始化 row、计数器、循环上限

LOOP:
    ACT(row)
    满足 tRAS
    PRE
    满足 tRP/tRC
    更新 row 或计数器
    BL LOOP
```

上面仅是逻辑示意，最终语法和时序填充必须由现有 Bender assembler 及 Adapter 规则生成。

### 5.2 随机或混合 Pattern

对于含随机缩放、随机地址选择、background 混合或加权随机的 Pattern：

1. Host 按固定 seed 生成有限模板；
2. Compiler 将模板展开为合法的访问块；
3. 使用现有 `BL` 外层循环重复该访问块；
4. 最后一轮如果不足一个完整模板，则生成 tail block；
5. 运行前输出地址直方图和比例报告。

例如：

```text
模板长度 T = 128
目标访问数 N = 16512

完整循环次数 = N // T = 129
尾部访问数   = N % T = 0
```

因此 16512 次访问不等于 16512 组独立存储的指令。

### 5.3 指令容量自动检查

Compiler 必须在生成二进制前进行容量估算：

```text
T_max = floor((D - H - M) / C_access)
```

其中：

- `D`：Instruction BRAM 可用指令深度；
- `H`：初始化、循环和结束控制指令数量；
- `M`：预留安全空间；
- `C_access`：一次访问平均需要的指令数量；
- `T_max`：允许的最大模板长度。

`template_size: auto` 时，从可用容量和比例分辨率共同确定模板长度。禁止在超出 BRAM 时截断程序后继续运行。

---

## 6. 10 类 Pattern 的映射方案

> Pattern 8 `parallel_access` 已排除，因此本节覆盖其余 10 类。

| No. | Pattern | 需要保留的特征 | Host 端处理 | Bender 端执行 | 是否改 RTL |
|---:|---|---|---|---|---|
| 1 | `grid` | 三维索引形成的 multiplier；2~5 随机缩放；目标 ACT 数 | 生成有限 `multiplier × scale` 模板；不排序 | 循环执行 ACT/PRE 模板 | 否 |
| 2 | `grid_fixed10` | multiplier × 10；目标 ACT 数 | 可直接生成固定地址表，或压缩为地址步进循环 | 地址更新 + ACT/PRE + BL | 否 |
| 3 | `grid_background` | grid 地址分布、background 比例及范围 | 用 quota 或 seeded random 混合两类地址 | 循环执行混合模板 | 否 |
| 4 | `grid_weighted` | x 位置权重；2~5 缩放；总 ACT 数 | 按 `x+1` 或 `x_dim-x` 生成配额并打乱/分组 | 模板循环；连续重复可转内层循环 | 否 |
| 5 | `grid_fixed10_weighted` | 固定 ×10；x 位置权重 | 生成确定性权重表 | 地址循环 + repeat 循环 | 否 |
| 6 | `grid_interval` | 每窗口 ACT 数为 `n/(1+interval_num)` | 计算整数目标 ACT 数并生成窗口计划 | 执行目标次数，剩余窗口保持空闲或等待边界 | 否 |
| 7 | `grid_alternating` | 当前 V0 为单窗口 ACT 数减半 | 基线按 `n/2` 生成；可选支持高/低窗口交替 | 两个窗口块或减半模板循环 | 否 |
| 9 | `random_access` | seed 地址集合、2~5 缩放、placeholder、ACT 总数 | 固定 seed 生成模板 | 模板循环 + tail block | 否 |
| 10 | `random_background` | random/background 比例、地址范围、ACT 总数 | 配额混合并打乱 | 混合模板循环 | 否 |
| 11 | `weighted_random_access` | seed 权重、2~5 缩放、placeholder、ACT 总数 | 按累计权重或 quota 生成模板 | 模板循环 + tail block | 否 |

### 6.1 Pattern 1：`grid`

原 Pattern 中每次访问仍含 2~5 的随机缩放，因此不能简单等同为连续 Row 自增。第一阶段应在 Host 端生成有限缩放模板，例如：

```text
(multiplier=1, scale=3) → row 3
(multiplier=2, scale=5) → row 10
(multiplier=3, scale=2) → row 6
...
```

不再执行原文档中的最终地址排序。

### 6.2 Pattern 2：`grid_fixed10`

地址生成是确定性的，最适合用现有循环压缩。若实际地址映射允许，可由寄存器保存当前 Row 并按固定步长更新；若 ACT 地址不能由寄存器直接驱动，则由 Host 生成一个短地址块再循环。

### 6.3 Pattern 3：`grid_background`

推荐用 `quota_shuffle`：

```text
normal_count     = round(T × (1 - background_ratio))
background_count = T - normal_count
```

分别生成 normal 和 background 地址后打乱。这样可以保证 background 比例，不需要 FPGA 在线比较随机数。

### 6.4 Pattern 4/5：Weighted Grid

权重定义保持：

```text
sort == 0  → repeat_times = x + 1
sort != 0  → repeat_times = x_dim - x
```

Compiler 可选择两种编码：

- 地址连续重复较多：使用内层 repeat/branch；
- 顺序需要打乱：由 Host 生成配额模板后循环。

### 6.5 Pattern 6：`grid_interval`

该 Pattern 的核心不是单纯增加命令间 WAIT，而是降低每个测试窗口中的 ACT 数：

```text
target_count = n / (1 + interval_num)
```

因此实现时必须同时满足：

1. 目标窗口内 ACT 数正确；
2. 窗口边界正确；
3. 剩余时间不会误执行额外 ACT。

`target_count` 非整数时使用 `floor`、`ceil` 还是四舍五入，需要需求方在编码前确认。建议默认采用 `floor` 并在报告中记录。

### 6.6 Pattern 7：`grid_alternating`

原文档当前 V0 代码实际使用：

```text
target_count = n / 2
```

它并未真正生成“高 ACT 窗口—低/空 ACT 窗口”的完整交替序列。因此建议分为：

- `alternating_v0`：每窗口固定 `n/2`，用于对齐现有模型；
- `alternating_window`：按 `[high_count, low_count]` 两种窗口交替，作为可选增强。

第一阶段先实现 `alternating_v0`，除非需求方明确要求真实窗口交替。

### 6.7 Pattern 9/10/11：Random 类

Random 类全部采用 Host 预生成模板，不在 FPGA 中新增 PRNG：

```text
固定 seed
    ↓
生成 seed address / scale / background / weight 配额
    ↓
打乱为有限模板
    ↓
编译为现有 Bender 指令
    ↓
循环执行
```

模板报告必须同时给出：

- seed；
- 模板长度；
- 各地址出现次数；
- scale factor 2/3/4/5 的次数；
- background 数量和比例；
- 各权重对象的访问次数与期望比例。

---

## 7. 时序与窗口处理

Pattern 的统计分布正确并不代表 DRAM 命令合法。Compiler 仍需遵守当前 Bender/Adapter 已确定的时序约束。

### 7.1 每次访问的基本约束

至少检查：

- ACT 到 PRE 的最小间隔；
- PRE 到下一次 ACT 的最小间隔；
- 同 Bank 相邻 ACT 的最小间隔；
- REF 前后的等待；
- Bank/Row 地址范围；
- 同一窗口中的最大可执行 ACT 数。

### 7.2 两种窗口模式

| 模式 | 定义 | 适用情况 |
|---|---|---|
| `count_only` | 只控制相邻评估事件之间的 ACT 数 | 对齐当前 V0 软件评估模型 |
| `timed_window` | 同时控制窗口周期数和窗口内 ACT 数 | 上板评估真实 PRAC/RHR 压力时推荐 |

如果需求方目前只关心 ACT 数，第一阶段可以先使用 `count_only`；但配置和报告中必须明确模式，避免后续把“次数模型”误认为“精确时间模型”。

---

## 8. Compiler 输出物

每次编译建议生成一个独立运行目录：

```text
run_20260918_001/
├── pattern.yaml
├── pattern_resolved.json
├── pattern_sequence.csv
├── bender_program.asm
├── bender_program.bin
├── compile_report.md
└── manifest.json
```

### 8.1 `pattern_resolved.json`

保存所有默认值展开后的最终配置，防止只保存原始 YAML 导致参数不完整。

### 8.2 `pattern_sequence.csv`

至少包含：

```text
index, window_id, bank, row, source_id, scale_factor, weight, is_background
```

### 8.3 `compile_report.md`

至少报告：

- Pattern 类型与 seed；
- 目标 ACT 数和实际 ACT 数；
- 每窗口 ACT 数；
- 模板长度、循环次数、tail 长度；
- 指令使用量和 BRAM 剩余量；
- 地址范围和非法地址检查；
- background 实际比例；
- weighted 实际比例；
- 时序检查结果；
- 二进制文件哈希。

---

## 9. 验证与验收标准

### 9.1 软件单元测试

| 检查项 | 验收标准 |
|---|---|
| 可复现性 | 相同配置和 seed 生成完全相同的 sequence、assembly 和 binary hash |
| ACT 总数 | 实际值等于配置目标值 |
| 窗口 ACT 数 | 每个窗口与该 Pattern 的目标一致 |
| 地址合法性 | 所有 Bank/Row 均在配置和硬件范围内 |
| 排序行为 | 不执行旧版最终地址排序 |
| Pattern 8 | 不生成、不出现在可选列表中 |
| 指令容量 | 总指令数不超过 BRAM 上限，且保留配置的安全余量 |
| Branch/Loop | 循环次数及 tail block 无 off-by-one |

### 9.2 分布验收

建议默认误差规则：

```text
ratio_tolerance = max(1 / template_size, 1%)
```

| Pattern 类型 | 验收指标 |
|---|---|
| Background | 实际 background 比例与目标差值不超过 `ratio_tolerance` |
| Weighted | 每个 source 的实际比例与理论权重比例差值不超过 `ratio_tolerance` |
| Scale 2~5 | 若定义为均匀选择，各 scale 的比例满足同一误差规则 |
| Interval | 每窗口 ACT 数等于约定的整数化结果 |
| Alternating V0 | 每窗口 ACT 数等于 `n/2` 的约定整数化结果 |

如果使用 `quota_shuffle`，优先做到模板内精确配额；只有无法整除时才使用上述误差范围。

### 9.3 Bender 仿真/Trace 对比

在上板前，从 Pipeline/Adapter 可观察点导出 ACT trace，并与 `pattern_sequence.csv` 比较：

1. ACT 数量是否一致；
2. Bank/Row 是否一致；
3. 窗口边界是否一致；
4. PRE/REF/WAIT 是否满足约束；
5. 循环回跳后模板起点是否正确；
6. 最后一轮 tail 是否正确结束。

### 9.4 上板验收

建议分三步：

1. **小规模确定性测试**：固定 2~4 个 Row，人工核对 Trace/计数；
2. **单 Pattern 压力测试**：逐个验证 10 类 Pattern 的 ACT 统计；
3. **长时间循环测试**：检查循环稳定性、指令计数、PRAC/RHR 事件和 readback。

最终结果应能回答：

- Bender 实际执行了多少次 ACT；
- 每个 Row 被访问多少次；
- 每个窗口的访问密度是多少；
- background/weighted 比例是否达到目标；
- PRAC/RHR 在何时产生了什么事件。

---

## 10. 分阶段实施计划

### Phase 0：接口确认

目标：冻结第一阶段需求，避免软件完成后再改变验收口径。

需要确认：

1. Random 类是否接受有限模板循环；
2. `grid_interval` 非整数 ACT 数的取整方式；
3. `grid_alternating` 采用 V0 减半，还是实际高/低窗口交替；
4. 窗口是 `count_only` 还是 `timed_window`；
5. 需要观察和导出的 PRAC/RHR 信号或结果字段。

交付：一页接口确认记录。

### Phase 1：最小可行 Compiler

优先实现 4 个代表性 Pattern：

1. `grid_fixed10`：验证确定性循环；
2. `grid_background`：验证混合比例；
3. `grid_interval`：验证窗口 ACT 数；
4. `weighted_random_access`：验证最复杂的权重随机模板。

交付：配置解析、sequence CSV、Bender assembly/binary、compile report。

### Phase 2：补齐其余 Pattern

实现：

- `grid`；
- `grid_weighted`；
- `grid_fixed10_weighted`；
- `grid_alternating`；
- `random_access`；
- `random_background`。

交付：10 类 Pattern 的统一接口和单元测试。

### Phase 3：Bender 仿真与上板验证

按“小规模—单 Pattern—长时间循环”的顺序验证，保存 Trace、统计报告和 readback。

### Phase 4：决定是否需要硬件增强

只有出现以下需求时，再考虑增加 FPGA Pattern Engine/PRNG：

- 要求每次 ACT 在线生成新随机数；
- 不允许短模板重复；
- 要求运行中动态改变权重或 background 比例；
- 指令 BRAM 无法容纳达到统计精度所需的最小模板；
- Host 重新编译和加载的切换延迟不可接受；
- 需要根据实时 PRAC/RHR 反馈自适应改变下一次访问。

在这些条件出现前，不建议修改 Bender RTL。

---

## 11. 风险与控制措施

| 风险 | 影响 | 控制措施 |
|---|---|---|
| 短模板周期性过强 | 与真实随机序列存在差异 | 自动增大模板、支持多个模板轮换，并在报告中给出周期 |
| 比例无法整除 | 权重/background 出现量化误差 | 使用最大可用模板并报告理论与实际比例 |
| 指令 BRAM 超限 | 程序无法加载或被截断 | 编译前容量估算，超限直接报错 |
| 循环边界错误 | ACT 总数偏差 | 自动生成 tail block，并对 trace 做精确计数 |
| 地址映射越界 | 访问错误 Bank/Row | Normalizer 统一检查物理地址范围 |
| 只匹配次数、不匹配时间 | 上板压力与软件模型不一致 | 明确 `count_only/timed_window`，硬件实验优先 timed window |
| V0 alternating 含义模糊 | 双方对结果理解不同 | 将 V0 减半和真实交替拆成两个模式 |
| 随机模板被误认为真随机 | 结论外推过度 | 报告中明确 seed、模板长度和重复次数 |

---

## 12. 建议发送给需求方的确认内容

> 我们计划保持现有 Bender 的 ISA、Pipeline 和 Adapter 不变，在 Host 端增加 Pattern Compiler。Compiler 会根据 Pattern 参数预生成有限长度、固定 seed 的访问模板，再利用 Bender 现有循环执行。我们会保持每个窗口的 ACT 数、地址集合、访问比例、权重、background 比例以及必要的时序边界，但不逐条复现原工具的完整长序列，也不再执行旧版地址排序。Pattern 8 暂不实现。请确认 Random/Weighted Random Pattern 可以接受这种“有限模板循环”的实现方式。另外请确认 `grid_interval` 的非整数 ACT 数取整规则，以及 `grid_alternating` 是按当前 V0 的每窗口减半实现，还是需要真正的高/低窗口交替。

---

## 13. 最终建议

第一阶段采用：

```text
现有 Bender 硬件
+
Host Pattern Compiler
+
有限随机/配额模板
+
现有 Loop/Counter
+
统计报告与 Trace 验证
```

该方案以最小硬件改动覆盖目前 10 类有效 Pattern，满足 PRAC/RHR 访问分布和压力测试需求，同时保留后续增加在线 PRNG 或 Pattern Engine 的升级空间。

第一阶段是否成功，不应以“是否逐条复现原 Python sequence”为标准，而应以以下结果为标准：

1. ACT 数量正确；
2. 地址与权重分布正确；
3. 窗口密度正确；
4. DRAM 时序合法；
5. 结果可复现、可追踪、可回放；
6. 不修改现有 Bender 核心硬件即可稳定运行。
