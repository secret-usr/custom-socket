对一个纯单文件 C/C++ 源码的函数调用关系分析脚本。

特点：

- 纯正则+简单词法，零依赖，适合快速粗粒度分析  
- 识别普通/带指针/引用/返回类型组合的函数定义（不含宏展开、模板特化等复杂情况）  
- 过滤 if/for/while/switch/return 等关键字的“伪调用”  
- 生成：  
  1) 文本调用图  
  2) 可选 Graphviz dot 文件  
  3) 从 main 出发的调用链枚举（去重，限制深度）  

使用方式：  
python3 func-call-analyzer.py ../../proto/socket_comm.cpp --dot callgraph.dot --max-depth 6

说明/局限：
- 不解析宏展开、模板多实例、重载区分、命名空间作用域解析与内联定义中的嵌套类方法等复杂情况
- 可能把同文件内的同名重载函数视为一个节点
- 对条件编译 (#ifdef) 内部未展开的分支不分析
- 如需更精确可改用 libclang（clang.cindex）建立 AST 后再生成调用图
