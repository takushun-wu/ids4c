# 变更记录 / Changelog

## v0.1.0

### 新增 / Added

- 提供中英文项目概览。
- 提供独立的查询语法、CLI 和 Python API 文档。
- 增加 CMake 安装规则，可安装头文件、库、可执行文件、Python 模块和文档。
- 增加用于 Linux CLI 构建和测试的 GitHub Actions 工作流。

This release now includes English and Chinese project overviews, dedicated query syntax, CLI, and Python API documentation, CMake installation rules for headers, libraries, executables, the Python module, and documentation, and a GitHub Actions workflow for Linux CLI builds and tests.

### 变更 / Changed

- 移除公开构建配置中的运行时数据库自动复制功能。运行时数据库现在需要通过数据库导入流程显式准备。
- 英文 README 作为默认项目概览，中文版本位于 `README.zh-CN.md`。

The public build configuration no longer copies runtime databases automatically. Runtime databases must be prepared explicitly through the database import workflow. The English README is the default project overview; the Chinese version is available as `README.zh-CN.md`.
