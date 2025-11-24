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
