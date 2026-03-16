# Multi-Page-Size & Perforated Pages 设计报告

## 第一部分：Multi-Page-Size（4KB + 2MB）

### 1. 设计目标

原版 ChampSim 只支持单一 4KB 页大小。我们的目标是：

- 在运行时支持 **4KB 和 2MB 两种页大小混合**
- 通过 `--pmap` 文件指定每个虚拟页的大小
- **最小化对现有代码的侵入**：保持全局 `PAGE_SIZE = 4096` 不变

### 2. 核心设计决策

**保留 4KB 作为全局基线**。ChampSim 内部有 30+ 处使用 `champsim::page_number` / `champsim::page_offset`（都基于 12-bit 偏移）。如果改成 2MB 全局页大小，改动量巨大。因此我们的策略是：

> 2MB 页作为"特殊路径"处理，仅在 TLB 和翻译流水线中做分支，其余部分不动。

### 3. 整体架构

原版 ChampSim 的翻译路径是单条链：

```
L1D ──lower_translate──> DTLB(4K) ──> STLB(4K) ──> PTW
```

我们新增了一条 **并行的 2MB TLB 链**：

```
                     ┌──lower_translate──> DTLB   (offset=12) ──> STLB   (offset=12) ──> PTW
L1D ─── page_size? ──┤
                     └──lower_translate_2m> DTLB_2M(offset=21) ──> STLB_2M(offset=21) ──> PTW
```

L1I 也有同样的结构（ITLB / ITLB_2M）。两条链 **共享同一个 PTW**，但 PTW 内部根据 `page_size` 做不同深度的 walk。

### 4. page_size 字段的传播

我们在请求/响应的每一层都增加了 `uint8_t page_size` 字段，它沿整个翻译流水线传播：

```
                       ┌──────────────────────────────────────────────────────────┐
                       │                  page_size 字段流向                       │
                       │                                                          │
request.page_size ───> tag_lookup_type.page_size ───> mshr_type.page_size         │
                                                          │                       │
                                                          ▼                       │
                                                   response.page_size ◄───────────┘
                                                          │
                                        PTW mshr_type.page_size ──> response.page_size
```

枚举值定义（`inc/vmem.h`）：

```cpp
enum class PageSize : uint8_t { PAGE_4K = 0, PAGE_2M = 1, PAGE_PERF = 2 };
```

### 5. 翻译请求的发起与路由

当 L1D 需要翻译一个虚拟地址时，`issue_translation()` 决定走哪条 TLB 链：

```
                    issue_translation(entry)
                            │
                   ┌────────▼────────┐
                   │ 查询 pmap：      │
                   │ g_vmem->         │
                   │ get_page_size()  │
                   └────────┬────────┘
                            │
               ┌────────────┼────────────┐
               │            │            │
          PAGE_4K       PAGE_2M      PAGE_PERF
               │            │            │
               ▼            ▼            ▼
          lower_translate   lower_translate_2m
          (DTLB 4K链)       (DTLB_2M 链)
```

关键代码（`src/cache.cc` `issue_translation()`）：

```cpp
// 1. 查询 pmap 确定页大小
if (g_vmem && !q_entry.page_size_determined) {
    auto ps = g_vmem->get_page_size(champsim::page_number{q_entry.v_address});
    q_entry.page_size = static_cast<uint8_t>(ps);
    q_entry.page_size_determined = true;
}

// 2. 路由到对应 TLB 链
champsim::channel* target_tlb = lower_translate;          // 默认 4KB
if ((page_size == PAGE_2M || page_size == PAGE_PERF)
    && lower_translate_2m != nullptr) {
    target_tlb = lower_translate_2m;                      // 走 2MB 链
}

q_entry.translate_issued = target_tlb->add_rq(fwd_pkt);
```

### 6. pmap 查询逻辑

`get_page_size()` 的查找策略是两级的：

```
                get_page_size(vpn_4k)
                        │
            ┌───────────▼───────────┐
            │ pmap.find(vpn_4k)     │  ← 精确匹配（4KB 粒度条目）
            └───────────┬───────────┘
                   找到? ──Yes──> 返回 pmap[vpn_4k]
                        │
                       No
                        │
            ┌───────────▼───────────┐
            │ base = (vpn>>9)<<9    │  ← 计算 2MB 对齐的基地址
            │ pmap.find(base)       │     (2MB = 512 × 4KB)
            └───────────┬───────────┘
                   找到且是
                  2MB/PERF? ──Yes──> 返回 pmap[base]
                        │
                       No
                        │
                 返回 PAGE_4K（默认）
```

这样做的好处：pmap 中只需要一个 2MB 条目就能覆盖 512 个 4KB VPN。

### 7. TLB 响应与地址拼接

当 TLB 返回翻译结果后，L1D 的 `finish_translation()` 负责将物理页号和虚拟地址偏移拼接成最终物理地址：

```
            finish_translation(response)
                        │
            ┌───────────▼───────────┐
            │ response.page_size ?   │
            └───────────┬───────────┘
                        │
           ┌────────────┼────────────┐
           │                         │
       PAGE_4K                   PAGE_2M
           │                         │
    splice(PPN, offset_12bit)   splice(PPN, offset_21bit)
    ┌──────┴──────┐             ┌──────┴──────┐
    │ PA[47:12]=PPN│             │ PA[47:21]=PPN│
    │ PA[11:0]=VA │             │ PA[20:0]=VA  │
    └─────────────┘             └──────────────┘
```

**4KB** 拼接：物理页号占 bit[47:12]，虚拟地址的低 12 位作为页内偏移。
**2MB** 拼接：物理页号占 bit[47:21]，虚拟地址的低 21 位作为页内偏移。

关键代码：

```cpp
if (pkt_page_size == PAGE_2M) {
    // 21-bit offset splice
    entry.address = splice(PPN_bits[47:21], VA_bits[20:0]);
} else {
    // 12-bit offset splice（默认 4KB）
    entry.address = splice(PPN, page_offset{entry.v_address});
}
```

### 8. 2MB 响应的匹配规则

4KB 翻译的匹配很简单：虚拟页号完全相等。但 2MB 翻译需要特殊处理——同一个 2MB 页内的 512 个 4KB 子页应该都能匹配同一个翻译响应：

```
             matches_vpage(response, entry)
                        │
            ┌───────────▼───────────┐
            │ response.page_size ?   │
            └───────────┬───────────┘
                        │
           ┌────────────┼────────────┐
           │                         │
       PAGE_4K                   PAGE_2M / PAGE_PERF
           │                         │
    VPN == VPN ?                (VPN & ~0x1FF) == (VPN & ~0x1FF) ?
    (精确匹配)                   (2MB 对齐匹配：忽略低 9 位)
```

这一点至关重要：如果 DTLB_2M 的 MSHR 合并了多个请求（比如 VPN=0x400 和 VPN=0x401），TLB 响应只携带一个 `v_address`（VPN=0x400）。使用 2MB 对齐匹配后，VPN=0x401 的请求也能被正确匹配。

### 9. PTW 中的变深度 Walk

PTW 内部根据 `page_size` 决定何时停止页表遍历：

```
                  x86-64 四级页表结构

    Level 4 (PML4) ──> Level 3 (PDPT) ──> Level 2 (PDE) ──> Level 1 (PTE)
         │                  │                  │                  │
         │                  │                  │                  │
         │                  │                  ▼                  ▼
         │                  │            ┌──────────┐      ┌──────────┐
         │                  │            │ 2MB 页   │      │ 4KB 页   │
         │                  │            │ 在此停止 │      │ 在此停止 │
         │                  │            └──────────┘      └──────────┘
         ▼                  ▼
    每一级都需要一次内存访问（通过 cache/memory hierarchy）
```

判断逻辑（`src/ptw.cc`）：

```cpp
auto is_last_step = [](auto x) {
    if (x.page_size == PAGE_2M)
        return x.translation_level <= 1;   // 2MB: Level 2 之后就停
    if (x.page_size == PAGE_PERF)
        return x.perf_state == BITMAP_PENDING; // perforated: 需额外步骤
    return x.translation_level <= 0;       // 4KB: Level 1 之后停
};
```

2MB 页的 walk 比 4KB 少一级（3 次 vs 4 次内存访问），这是 2MB 页减少 TLB miss 开销的原因之一。

### 10. 2MB TLB 的参数配置

2MB TLB 的关键区别是 `offset_bits = 21`（而非 12），这意味着：

| 参数 | DTLB (4KB) | DTLB_2M (2MB) |
|------|-----------|---------------|
| offset_bits | 12 | 21 |
| Sets | 16 | 4 |
| Ways | 4 | 4 |
| 一个 TLB entry 覆盖 | 4KB | 2MB |

STLB_2M 类似：16 sets × 12 ways × 2MB/entry = 最多覆盖 384 MB 虚拟地址空间。

### 11. 物理内存的 2MB 页管理

`vmem.cc` 中的 `populate_pages()` 在初始化时：

1. 先按原有逻辑建立 4KB 物理页的空闲链表
2. 然后扫描链表，找出 **2MB 对齐且连续的 512 个 4KB 页**，将它们移入 `ppage_free_list_2m`
3. 最多将每个设备一半的物理页划给 2MB 池

当 PTW 完成 2MB 页的翻译时，调用 `va_to_pa_2m()` 从 2MB 池中分配。

---

## 第二部分：Perforated Pages（穿孔页）

### 1. 问题背景

2MB 大页需要 **连续的 2MB 物理内存**，在内存碎片化严重时很难满足。穿孔页（Perforated Page）是一个折中方案（来自 ISCA 2020 论文）：

> 一个 2MB 的大页中，**大部分** 4KB 子页使用连续的 2MB 物理帧，少数"洞"（holes）映射到独立的 4KB 物理帧。

```
    2MB 虚拟页（512 个 4KB 子页）
    ┌──┬──┬──┬──┬──┬──┬──┬──┬──┬──┬──┬──┬──┬──┬──┬──┐
    │  │  │  │  │  │  │  │  │  │  │  │  │  │  │  │  │ ...
    └──┴──┴──┴──┴──┴──┴──┴──┴──┴──┴──┴──┴──┴──┴──┴──┘
     ▲  ▲                    ▲
     │  │                    │
    hole hole              hole     ← 这些映射到独立的 4KB 物理帧
     │  │                    │
     ▼  ▼                    ▼
    ┌──┐┌──┐              ┌──┐
    │4K││4K│              │4K│      ← 散落在物理内存各处
    └──┘└──┘              └──┘

    其余子页 ────> 共享一个连续的 2MB 物理帧
```

### 2. 数据结构

每个穿孔页有两级过滤结构：

```
    Coarse Filter（8 bit，1 bit / region）
    ┌─────────────────────────────────────────────────────┐
    │ R0 │ R1 │ R2 │ R3 │ R4 │ R5 │ R6 │ R7 │
    └──┬──┴──┬──┴──┬──┴──┬──┴──┬──┴──┬──┴──┬──┴──┬──────┘
       │     │     │     │     │     │     │     │
       ▼     ▼     ▼     ▼     ▼     ▼     ▼     ▼
    ┌──────┬──────┬──────┬──────┬──────┬──────┬──────┬──────┐
    │ 64个 │ 64个 │ 64个 │ 64个 │ 64个 │ 64个 │ 64个 │ 64个 │  Hole Bitmap
    │子页  │子页  │子页  │子页  │子页  │子页  │子页  │子页  │  (512 bit)
    └──────┴──────┴──────┴──────┴──────┴──────┴──────┴──────┘

    共 512 个子页，分 8 个 region，每 region 64 个子页

    Coarse Filter bit = 0 → 该 region 无 hole → 快速跳过 bitmap
    Coarse Filter bit = 1 → 该 region 有 hole → 需查 bitmap
```

### 3. L1 Cache 中的三路径解析

当 L1D 收到一个 `PAGE_PERF` 类型的 TLB 响应后，需要在 `finish_translation()` 中做细粒度判断——当前 4KB 子页是不是 hole？

```
           finish_translation() 收到 PAGE_PERF 响应
                        │
            ┌───────────▼───────────┐
            │ coarse_filter_pass()  │  ← 1 cycle
            │ 检查当前 region 是否   │
            │ 有任何 hole           │
            └───────────┬───────────┘
                        │
              ┌─────────┼─────────┐
              │                   │
         bit = 0              bit = 1
         (无 hole)            (可能有 hole)
              │                   │
              ▼                   ▼
    ┌─────────────────┐ ┌────────────────┐
    │  FAST PATH      │ │ is_hole()      │  ← +10 cycles
    │  用 2MB PPN 拼接│ │ 查 512-bit     │
    │  +1 cycle       │ │ bitmap         │
    └─────────────────┘ └───────┬────────┘
                                │
                      ┌─────────┼─────────┐
                      │                   │
                 bitmap = 0          bitmap = 1
                 (不是 hole)         (是 hole!)
                      │                   │
                      ▼                   ▼
            ┌─────────────────┐ ┌─────────────────┐
            │  MEDIUM PATH    │ │  SLOW PATH       │
            │  用 2MB PPN 拼接│ │  重置为 PAGE_4K  │
            │  +1+10 cycles   │ │  重发到 4KB TLB  │
            └─────────────────┘ │  +10+10 cycles   │
                                │  然后走完整的     │
                                │  DTLB→STLB→PTW   │
                                └─────────────────┘
```

延迟开销汇总：

| 路径 | 条件 | 额外延迟 | 结果 |
|------|------|---------|------|
| Fast Path | coarse filter = 0 | +1 cycle | 用 2MB PPN 翻译 |
| Medium Path | coarse filter = 1, bitmap = 0 | +11 cycles | 用 2MB PPN 翻译 |
| Slow Path | coarse filter = 1, bitmap = 1 | +20 cycles + 4KB walk | 重发到 4KB TLB 链 |

### 4. Slow Path 的重入机制

当检测到 hole 时，需要将该请求"降级"为 4KB 翻译：

```
    entry 在 L1D 内的状态变化（Slow Path）：

    初始：  page_size = PERF,  translate_issued = true,   is_translated = false
                                        │
            ┌───────────────────────────▼────────────────────────────┐
            │ finish_translation() 检测到 hole：                      │
            │   entry.page_size = PAGE_4K                            │
            │   entry.page_size_determined = true  ← 防止 pmap 重查  │
            │   entry.translate_issued = false     ← 触发重新发起    │
            │   entry.event_cycle += 20 cycles                       │
            └───────────────────────────┬────────────────────────────┘
                                        │
            下一个 operate() 周期：
                                        │
            ┌───────────────────────────▼────────────────────────────┐
            │ issue_translation()：                                   │
            │   page_size_determined = true → 不再查 pmap             │
            │   page_size = PAGE_4K → 路由到 lower_translate (DTLB)  │
            └───────────────────────────┬────────────────────────────┘
                                        │
                                        ▼
                              DTLB(4K) → STLB(4K) → PTW
                              PTW 调用 va_to_pa() 分配 4KB 物理页
```

`page_size_determined` 标志至关重要：因为 `PAGE_4K = 0`，与"未设置"无法区分。没有这个标志，`issue_translation()` 会重新查 pmap，得到 `PAGE_PERF`，又路由到 2MB TLB——形成死循环。

### 5. PTW 中的 Bitmap 步骤

对于穿孔页，PTW 的 walk 比普通 2MB 页多一步——读取 bitmap：

```
    PTW page table walk 对比：

    4KB 页：   L4 → L3 → L2 → L1 → va_to_pa()
                                  4 次内存访问

    2MB 页：   L4 → L3 → L2 → va_to_pa_2m()
                              3 次内存访问

    穿孔页：   L4 → L3 → L2 → [bitmap read] → va_to_pa_2m()
                              3 + 1 = 4 次内存访问
                                      │
                                      └── 额外的一次内存访问
                                          读取 bitmap 数据
```

PTW 用 `PerfState` 状态机管理这个额外步骤：

```cpp
enum class PerfState { NORMAL, BITMAP_PENDING };
```

- 当 `translation_level <= 1` 且 `page_size == PAGE_PERF` 且 `perf_state == NORMAL` 时：不结束 walk，而是发起 bitmap 读取，状态变为 `BITMAP_PENDING`
- 当 `perf_state == BITMAP_PENDING` 的响应回来时：walk 结束，调用 `va_to_pa_2m()` 分配 2MB 基地址

### 6. 自动穿孔（Auto-Perforation）

除了手写 pmap 中的 bitmap，还可以通过 CLI 参数自动生成：

```bash
bin/champsim --pmap base.pmap --perf-frag-ratio 0.25 --perf-frag-dist random ...
```

这会将 pmap 中所有 `PAGE_2M` 条目转换为 `PAGE_PERF`，并按指定分布生成 hole bitmap：

| 分布模式 | 行为 |
|---------|------|
| `clustered` | 每个 region 的前 N 个子页是 hole（连续放置） |
| `dispersed` | 每隔固定间隔放一个 hole（均匀分布） |
| `random` | 每个子页独立以 ratio 概率成为 hole |

注意：`random` 模式下 coarse filter 几乎无效——因为每个 64 子页 region 几乎都至少有一个 hole。

---

## 第三部分：验证中发现的 Bug 与修复

### Bug 1：TLB 响应丢失 page_size（导致死锁）

**症状**：使用 2MB pmap 时立即死锁，CPU panic。

**根因**：`CACHE::handle_fill()` 和 `CACHE::try_hit()` 在构造 response 时使用了 5 参数构造函数，没有传 `page_size`，导致响应的 `page_size` 始终为 0。

```
    请求路径（正确）：                   响应路径（bug）：

    L1D → DTLB_2M                      DTLB_2M → L1D
    page_size = 2 ✓                    page_size = 0 ✗
                                               │
                                               ▼
                                    finish_translation() 用 4KB 匹配
                                    只能匹配一个请求，其余永远等待
                                               │
                                               ▼
                                           DEADLOCK
```

**修复**：使用 6 参数构造函数，传入 `fill_mshr.page_size` / `handle_pkt.page_size`。

### Bug 2：穿孔页 hole 重入死循环

**症状**：使用穿孔页时死锁，event_cycle 异常高（亿级）。

**根因**：hole 重入后 `issue_translation()` 重查 pmap → 得到 PERF → 再走 2MB TLB → 再检测 hole → 无限循环。

```
    finish_translation: hole → page_size = PAGE_4K (= 0)
           │
           ▼
    issue_translation: page_size == 0 → 查 pmap → PAGE_PERF!
           │
           ▼
    路由到 DTLB_2M → 又收到 PERF 响应 → 又检测到 hole → ...
```

**修复**：增加 `page_size_determined` 标志，一旦 pmap 查过或 page_size 被显式设置，就不再重查。

### Bug 3：穿孔页统计未拷贝

**症状**：运行正确但 stats 始终显示 0。

**根因**：`end_phase()` 中将 `sim_stats` 拷贝到 `roi_stats` 时，漏了 `perf_total` 等 4 个字段。

**修复**：在 `end_phase()` 中补上这 4 个字段的拷贝。

---

## 第四部分：验证结果

### Phase 1：Multi-Page-Size

使用 445.gobmk (SPEC 2006) 和 605.mcf_s (SPEC 2017)：

| 配置 | IPC | DTLB Access | DTLB_2M Access | 结论 |
|------|-----|-------------|----------------|------|
| 无 pmap | 1.001 | 1,548,579 | 0 | baseline |
| 全 4KB | 1.001 | 1,548,579 | 0 | = baseline ✓ |
| 全 2MB | 1.022 | 0 | 1,102,202 | IPC +2.1% ✓ |
| 混合 | 1.030 | 861,553 | 522,555 | 两链都活跃 ✓ |

### Phase 2：Perforated Pages

| 配置 | IPC | 穿孔统计 |
|------|-----|---------|
| 混合 4K+2M+PERF | 0.884 | coarse 67%, bitmap 33%, hole 0.0% |
| auto-perf 0% | 1.022 | 无（= 纯 2MB）✓ |
| auto-perf 10% | 0.523 | hole = 10.1% |
| auto-perf 25% | 0.498 | hole = 19.9% |
| auto-perf 50% | 0.439 | hole = 41.5% |

IPC 随 hole 比例增加单调下降，符合预期。
