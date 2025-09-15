#!/usr/bin/env python3
import re
import argparse
from collections import defaultdict, deque

CONTROL_KEYWORDS = {
    'if','for','while','switch','return','sizeof','catch','new','delete',
    'static_cast','dynamic_cast','reinterpret_cast','const_cast'
}

DEF_EXCLUDE_PREFIX = {
    'else','typedef','using','#','template'
}

FUNC_NAME_RE = re.compile(r'[A-Za-z_]\w*')

def strip_comments(code:str)->str:
    # 去掉 // 和 /* */ 注释
    code = re.sub(r'//.*', '', code)
    code = re.sub(r'/\*.*?\*/', '', code, flags=re.S)
    return code

def find_function_definitions(code:str):
    # 粗略匹配函数定义：行首或行开始有类型 + 名称 (...) {   （忽略结尾为 ; 的声明）
    # 允许返回类型里有 * & :: < > , 等
    pattern = re.compile(
        r'(?:^|\n)\s*'
        r'(?:[A-Za-z_][\w:<>,\s\*\&~]*?)'   # 返回类型及可能的限定
        r'\b([A-Za-z_]\w*)\s*'              # 函数名捕获
        r'\([^;{}]*\)\s*'                   # 参数列表（不含 { } ;）
        r'(\{)',                            # 紧跟 {
        flags=re.M
    )
    funcs = []
    for m in pattern.finditer(code):
        name = m.group(1)
        # 跳过控制关键字误判
        if name in CONTROL_KEYWORDS: 
            continue
        # 取得函数体起始，需配对花括号找到结束
        start = m.end(2)-1
        body, end = extract_brace_block(code, start)
        if body is None:
            continue
        funcs.append( (name, start, end, body) )
    return funcs

def extract_brace_block(code:str, brace_pos:int):
    # brace_pos 指向 '{'
    if brace_pos >= len(code) or code[brace_pos] != '{':
        return None, None
    depth = 0
    for i in range(brace_pos, len(code)):
        c = code[i]
        if c == '{':
            depth += 1
        elif c == '}':
            depth -= 1
            if depth == 0:
                return code[brace_pos+1:i], i+1
    return None, None

def find_calls(body:str, all_func_names:set):
    """找到函数体中的函数调用，返回调用列表（保持顺序）"""
    calls = []  # 改为列表以保持调用顺序
    call_positions = []  # 存储调用位置，用于排序
    
    # 简单：查找 name( 形式
    for name in all_func_names:
        if name in CONTROL_KEYWORDS:
            continue
        # 避免把函数名作为更长标识的一部分
        pattern = re.compile(r'\b' + re.escape(name) + r'\s*\(')
        for match in pattern.finditer(body):
            call_positions.append((match.start(), name))
    
    # 按照在代码中出现的位置排序
    call_positions.sort(key=lambda x: x[0])
    
    # 提取函数名，保持顺序（可能有重复调用）
    calls = [name for pos, name in call_positions]
    
    return calls

def build_call_graph(code:str):
    code_nc = strip_comments(code)
    funcs = find_function_definitions(code_nc)
    names = {f[0] for f in funcs}
    graph = defaultdict(list)  # 改为列表以存储调用顺序和次数
    bodies = {}
    for name, s, e, body in funcs:
        bodies[name] = body
    for name, _, _, body in funcs:
        callees = find_calls(body, names - {name})
        # 统计每个被调用函数的调用次数和顺序
        call_order = {}
        for i, callee in enumerate(callees):
            if callee not in call_order:
                call_order[callee] = []
            call_order[callee].append(i + 1)  # 调用顺序从1开始
        
        # 存储调用信息：(被调用函数, 调用顺序列表)
        for callee, orders in call_order.items():
            graph[name].append((callee, orders))
    
    # 同时返回简化版本的图用于路径分析
    simple_graph = defaultdict(set)
    for caller, call_list in graph.items():
        for callee, _ in call_list:
            simple_graph[caller].add(callee)
    
    return graph, simple_graph, names

def detect_recursion(graph):
    visited = set()
    stack = set()
    cycles = []

    def dfs(node, path):
        visited.add(node)
        stack.add(node)
        path.append(node)
        for nxt in graph.get(node, []):
            if nxt not in visited:
                dfs(nxt, path)
            elif nxt in stack:
                # 找到环
                if nxt in path:
                    idx = path.index(nxt)
                    cyc = path[idx:] + [nxt]
                    cycles.append(cyc)
        stack.remove(node)
        path.pop()

    for n in graph:
        if n not in visited:
            dfs(n, [])
    return cycles

def enumerate_paths(graph, start, max_depth=6, limit=2000):
    paths = []
    stack = [(start, [start])]
    seen = set()
    while stack:
        node, path = stack.pop()
        if len(path) > max_depth:
            continue
        key = tuple(path)
        if key in seen:
            continue
        seen.add(key)
        paths.append(path)
        for nxt in graph.get(node, []):
            if nxt in path:  # 避免深度搜索中的递归扩张
                continue
            stack.append((nxt, path + [nxt]))
        if len(paths) >= limit:
            break
    return paths

def output_text(graph):
    lines = []
    for caller in sorted(graph.keys()):
        call_list = graph[caller]
        if call_list:
            call_strs = []
            for callee, orders in call_list:
                if len(orders) == 1:
                    call_strs.append(f"{callee}(#{orders[0]})")
                else:
                    order_str = ",".join(map(str, orders))
                    call_strs.append(f"{callee}(#{order_str})")
            lines.append(f"{caller} -> {', '.join(call_strs)}")
        else:
            lines.append(f"{caller} -> (no calls)")
    return "\n".join(lines)

def output_dot(graph):
    out = ["digraph CallGraph {"]
    out.append('  rankdir=LR;')
    out.append('  node [shape=box, style=filled, fillcolor=lightblue];')
    out.append('  edge [fontsize=10];')
    out.append('  ordering=out;')  # 添加此行以按定义顺序排列出边
    
    for caller, call_list in graph.items():
        if not call_list:
            out.append(f'  "{caller}";')
        else:
            # 按调用顺序排序：按orders的第一个值（第一次调用顺序）
            sorted_call_list = sorted(call_list, key=lambda x: x[1][0])
            for callee, orders in sorted_call_list:
                # 构建边的标签，显示调用顺序
                if len(orders) == 1:
                    label = f"#{orders[0]}"
                else:
                    label = f"#{','.join(map(str, orders))}"
                
                out.append(f'  "{caller}" -> "{callee}" [label="{label}"];')
    
    out.append("}")
    return "\n".join(out)

def main():
    ap = argparse.ArgumentParser(description="简单 C/C++ 单文件函数调用关系分析（支持调用顺序）")
    ap.add_argument("file", help="源文件 (单文件)")
    ap.add_argument("--dot", help="输出 Graphviz dot 文件")
    ap.add_argument("--max-depth", type=int, default=6, help="main 出发路径最大深度")
    args = ap.parse_args()

    with open(args.file, 'r', encoding='utf-8', errors='ignore') as f:
        code = f.read()

    graph, simple_graph, names = build_call_graph(code)

    print("=== 调用图 (caller -> callees with order) ===")
    print(output_text(graph))
    print()

    if "main" in names:
        print("=== 从 main 出发的调用链 (深度 <= {}) ===".format(args.max_depth))
        paths = enumerate_paths(simple_graph, "main", max_depth=args.max_depth)
        for p in paths:
            print("  " + " -> ".join(p))
        print("总路径数:", len(paths))
        print()
    else:
        print("未找到 main，跳过路径枚举\n")

    cycles = detect_recursion(simple_graph)
    if cycles:
        print("=== 递归 / 循环调用检测 ===")
        for cyc in cycles:
            print("  " + " -> ".join(cyc))
    else:
        print("未检测到递归/循环调用")

    if args.dot:
        with open(args.dot, "w", encoding="utf-8") as f:
            f.write(output_dot(graph))
        print(f"\n已写入 dot 文件: {args.dot} (可用 dot -Tpng {args.dot} -o graph.png)")

if __name__ == "__main__":
    main()
