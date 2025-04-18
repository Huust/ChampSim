#!/usr/bin/env python3
#
#    Copyright 2023 The ChampSim Contributors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# calling chain:
# config.sh: write_files() -> parse.py: parse_config() 在parse_config函数中实例化 NormalizedConfiguration 时会自动调用这个类的 __init__ 方法，这个方法是修改的核心之一
# -> 回到parse_config函数，调用 apply_defaults_in()
# -> 回到 config.sh: write_files() 
# -> filewrite.py: write_file() -> from_config() -> get_instantiation_header() | get_instantiation_lines()
# -> instantiation_file.py: get_instantiation_header() | get_instantiation_lines()
import json
import sys,os
import itertools
import argparse

import config.filewrite
import config.parse
import config.util

# Read the config file
def parse_file(fname):
    with open(fname) as rfp:
        # json.load 会读取 JSON 文件并转换成 Python 对象
        return json.load(rfp)

if __name__ == '__main__':
    champsim_root = os.path.dirname(os.path.abspath(__file__))
    test_root = os.path.join(champsim_root, 'test')
    # argparse 是 Python 的标准库；parser可以解析命令行参数
    parser = argparse.ArgumentParser(description='Configure ChampSim')

    path_group = parser.add_argument_group(title='Path Configuration', description='Options that control the output locations of ChampSim configuration')

    # 当用户在命令行中使用这些参数时，argparse 会自动解析并将值存储在 args 对象中，
    # 后续可以通过 args.prefix、args.bindir、args.makedir 来访问这些值
    # 这比c/c++手写解析要方便得多
    path_group.add_argument('--prefix', default='.',
            help='The prefix for the configured outputs')
    path_group.add_argument('--bindir',
            help='The directory to store the resulting executables')
    path_group.add_argument('--makedir',
            help='The directory to store the resulting makefile fragment. Note that `make` must later be invoked with -I.')

    search_group = parser.add_argument_group(title='Search Paths', description='Options that direct ChampSim to search additional paths for modules')

    search_group.add_argument('--module-dir', action='append', default=[], metavar='DIR',
            help='A directory to search for all modules. The structure is assumed to follow the same as the ChampSim repository: branch direction predictors are under `branch/`, replacement policies under `replacement/`, etc.')
    search_group.add_argument('--branch-dir', action='append', default=[], metavar='DIR',
            help='A directory to search for branch direction predictors')
    search_group.add_argument('--btb-dir', action='append', default=[], metavar='DIR',
            help='A directory to search for branch target predictors')
    search_group.add_argument('--prefetcher-dir', action='append', default=[], metavar='DIR',
            help='A directory to search for prefetchers')
    search_group.add_argument('--replacement-dir', action='append', default=[], metavar='DIR',
            help='A directory to search for replacement policies')

    parser.add_argument('--no-compile-all-modules', action='store_false', dest='compile_all_modules',
            help='Do not compile all modules in the search path')
    parser.add_argument('--compile-all-modules', action='store_true', dest='compile_all_modules',
            help='Compile all modules in the search path')

    parser.add_argument('-v', action='store_true', dest='verbose')

    parser.add_argument('--join', choices=['chain','product'], default='product',
            help='The joining method when multiple files are specified. A "chain" join concatenates the files, building the union of all specifications. A "product" join merges each possible combination of the specified builds. In the case of "product", the last file specified has the highest priority.')

    parser.add_argument('files', nargs='*',
            help='A sequence of JSON files describing the configuration.')

    # 之前的代码都是在"定义"命令行参数的格式和规则
    # 这一行才是真正"执行"命令行参数的解析
    args = parser.parse_args()

    # 设置二进制文件目录：如果指定了 bindir 就用它，否则用 prefix/bin
    bindir_name = os.path.expanduser(args.bindir or os.path.join(args.prefix, 'bin'))
    # 设置编译中间文件目录：使用 prefix/.csconfig
    objdir_name = os.path.expanduser(os.path.join(args.prefix, '.csconfig'))

    # 之前提供了files参数，但如果实际并没有读取到files参数
    if not args.files:
        print("No configuration specified. Building default ChampSim with no prefetching.")
    # reversed(args.files) 是针对多个文件的顺序
    # 经过这条脚本，files就是类json格式的python对象
    files = map(config.util.wrap_list, map(parse_file, reversed(args.files)))

    # 根据 join 参数决定如何合并多个配置文件
    if args.join == 'product':
        # product模式：生成所有配置文件的组合
        # 例如：files=[A,B], A=[a1,a2], B=[b1,b2]
        # 结果：[(a1,b1,{}), (a1,b2,{}), (a2,b1,{}), (a2,b2,{})]
        config_files = itertools.product(*files, ({},))
    elif args.join == 'chain':
        # chain模式：将所有配置文件串联
        # 例如：files=[A,B], A=[a1,a2], B=[b1,b2]
        # 结果：[(a1,), (a2,), (b1,), (b2,)]
        config_files = ((c,) for c in itertools.chain(*files))

    # 解析测试配置，生成测试可执行文件的配置
    parsed_test = config.parse.parse_config({'executable_name': '000-test-main'}, module_dir=[os.path.join(test_root, 'cpp', 'modules')], compile_all_modules=True)


    # 收集所有解析配置需要的参数
    parse_args = {
        'module_dir': args.module_dir,
        'branch_dir': args.branch_dir,
        'btb_dir': args.btb_dir,
        'pref_dir': args.prefetcher_dir,
        'repl_dir': args.replacement_dir,
        'compile_all_modules': args.compile_all_modules,
        'verbose': args.verbose
    }
    # 解析所有配置组合，生成最终的配置对象
    parsed_configs = (config.parse.parse_config(*c, **parse_args) for c in config_files)

    # 使用 FileWriter 将解析后的配置写入文件
    # 这会生成必要的C++代码和Makefile
    with config.filewrite.FileWriter(bindir_name=bindir_name, objdir_name=objdir_name, makedir_name=args.makedir, verbose=args.verbose) as wr:
        for c in parsed_configs:
            wr.write_files(c)

# vim: set filetype=python:
