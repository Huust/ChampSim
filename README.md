# 运行清单
- 常见使用: `./bin/champsim_dram --generate-heatmap "./heatmap" -w 50000000 -i 100000000 path/to/605.mcf_s-1536B && ./bin/champsim_tiered --use-heatmap "./heatmap" --ratio "1:1" -w 50000000 -i 100000000 path/to/605.mcf_s-1536B`
    - 两条指令分别代表使用dram_only模式运行，以及使用tiered_memory模式运行
    - 使用dram_only是为了生成heatmap，使用tiered memory是为了使用heatmap；可以选择配置不同的hot和cold ratio（例子中采用1:1，默认配置是1:3）
        - --generate-heatmap加heatmap路径会将在dram_only模式下运行产生LLC miss的所有请求的access count和criticality count保存在heatmap
        - --use-heatmap会读取路径并按照ratio排序后分为冷热数据
    - -w和-i分别表示warmup和simulation的指令数
    - 最后是path/to/traces
    > 常见的错误可以是：
    > 忘记分别使用dram_only和tiered_memory的二进制程序运行，转而只使用二者之一

# 在pelle上运行
- 步骤
    1. 编译所有二进制文件（包括 champsim_tiered_memory_skip）`./scripts/make_all_binaries.sh`
    2. 运行标准配置（7 种配置）`python3 scripts/runall_pelle.py`，输出到: `/proj/.../results/`
    3. 运行 skip 配置（4 种配置）`python3 scripts/run_skip.py`，输出到: `/proj/.../results_skip/`

# 数据统计
- 假设两份数据存储在code base目录的parent directory中，分别名为results和results_skip
- 首先运行scripts/collected_stats.py在code base目录中得到analysis文件夹，里面包含collected_stats.csv以及collected_stats.pkl
    - 这份collected_stats统计数据会包含每一个output（什么configuration？是否采用skip translation？）的ROI（IPC等）
- 运行compare_ipc.py可以从results和results_skip中读取所有traces的结果，对比四种配置下（access/criticality，ratio）每种trace在使用和不使用skip translation时得到的IPC speedup
- find_slowest_run.py可以从全部的输出结果中找到耗时最久的TOP K个配置+traces组合，方便在某次需要快速得到结果/寻找运行性能瓶颈
- plot.py详细绘制了每一个traces的IPC speedup，memory access breakdown，MPKI；这个脚本侧重于分析每一个trace的特征，适用于刚开始实现prototype时可视化每一个trace的output，确认我们的原型实现是否有问题
- plot_by_suite.py则是：将属于同一个benchmark的每个trace做geomean，再把属于同一个suite的benchmarks归类到一张图中；适用于prototype已经正确实现后，做更加统一的可视化分析我们提出的idea是否work
- skip_vs_no_skip.py：也是基于suite画图，只不过baseline从dram-only（上一个脚本）变为no_skip。
