# 贡献指南 / Contributing

## 中文

感谢你为 ids4c 提交问题、文档或代码。提交前请先阅读 [README.zh-CN.md](README.zh-CN.md) 和 [README.md](README.md)，确认修改符合项目当前的构建方式和查询语法。

### 本地验证

在不需要 GUI 和 Python binding 时，可以使用以下配置：

    cmake -S . -B build -DIDS4C_BUILD_GUI=OFF -DIDS4C_BUILD_PYTHON=OFF -DCMAKE_BUILD_TYPE=Release
    cmake --build build --parallel
    ctest --test-dir build --output-on-failure

如果修改了 GUI、Python binding 或数据库导入逻辑，请使用对应选项重新配置并完成相关验证。

### 文档规范

- 查询语法中的尖括号表达式在正文中使用行内代码格式。
- 查询语法、CLI 参数和 Python API 分别维护在 `docs/` 下对应的文档中。
- 新增特化语法时，同时更新中文和英文文档，并给出至少一个可运行示例。

### 提交问题

请尽量提供操作系统、编译器、CMake 配置、数据库来源、完整查询式和实际输出。涉及解析错误时，请附上输入文件的行号和最小复现片段。

---

## English

Thank you for contributing issues, documentation, or code to ids4c. Before submitting a change, read [README.md](README.md) and [README.zh-CN.md](README.zh-CN.md) to ensure that it follows the current build process and query syntax.

### Local verification

When GUI and Python binding support are not required, use:

    cmake -S . -B build -DIDS4C_BUILD_GUI=OFF -DIDS4C_BUILD_PYTHON=OFF -DCMAKE_BUILD_TYPE=Release
    cmake --build build --parallel
    ctest --test-dir build --output-on-failure

If you modify the GUI, Python binding, or database import code, reconfigure with the corresponding options and run the relevant checks.

### Documentation guidelines

- Put angle-bracket query expressions in inline code formatting when they appear in prose.
- Keep query syntax, CLI options, and Python API documentation in their corresponding files under `docs/`.
- When adding specialized syntax, update both the Chinese and English documentation and include at least one runnable example.

### Issue reports

Please include the operating system, compiler, CMake configuration, database source, complete query expression, and actual output whenever possible. For parser errors, include the input-file line number and a minimal reproducer.
