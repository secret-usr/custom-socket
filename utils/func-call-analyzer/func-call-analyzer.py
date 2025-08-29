
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
    calls = set()
    # 简单：查找 name( 形式
    for name in all_func_names:
        # 避免把函数名作为更长标识的一部分
        # (?<!\.) 避免方法链 obj.method?（这里仍可能漏/误，后面可改进）
        pattern = re.compile(r'\b' + re.escape(name) + r'\s*\(')
        if pattern.search(body):
            calls.add(name)
    # 过滤控制关键字
    return {c for c in calls if c not in CONTROL_KEYWORDS}

def build_call_graph(code:str):
    code_nc = strip_comments(code)
    funcs = find_function_definitions(code_nc)
    names = {f[0] for f in funcs}
    graph = defaultdict(set)
    bodies = {}
    for name, s, e, body in funcs:
        bodies[name] = body
    for name, _, _, body in funcs:
        callees = find_calls(body, names - {name})
        for cal in callees:
            graph[name].add(cal)
    return graph, names

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
        callees = sorted(graph[caller])
        if callees:
            lines.append(f"{caller} -> {', '.join(callees)}")
        else:
            lines.append(f"{caller} -> (no calls)")
    return "\n".join(lines)

def output_dot(graph):
    out = ["digraph CallGraph {"]
    out.append('  rankdir=LR;')
    for caller, callees in graph.items():
        if not callees:
            out.append(f'  "{caller}";')
        for cal in callees:
            out.append(f'  "{caller}" -> "{cal}";')
    out.append("}")
    return "\n".join(out)

def main():
    ap = argparse.ArgumentParser(description="简单 C/C++ 单文件函数调用关系分析")
    ap.add_argument("file", help="源文件 (单文件)")
    ap.add_argument("--dot", help="输出 Graphviz dot 文件")
    ap.add_argument("--max-depth", type=int, default=6, help="main 出发路径最大深度")
    args = ap.parse_args()

    with open(args.file, 'r', encoding='utf-8', errors='ignore') as f:
        code = f.read()

    graph, names = build_call_graph(code)

    print("=== 调用图 (caller -> callees) ===")
    print(output_text(graph))
    print()

    if "main" in names:
        print("=== 从 main 出发的调用链 (深度 <= {}) ===".format(args.max_depth))
        paths = enumerate_paths(graph, "main", max_depth=args.max_depth)
        for p in paths:
            print("  " + " -> ".join(p))
        print("总路径数:", len(paths))
        print()
    else:
        print("未找到 main，跳过路径枚举\n")

    cycles = detect_recursion(graph)
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
