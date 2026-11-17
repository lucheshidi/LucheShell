# 🦆 DuckShell (DUCKSHELL)

[![Windows Build](https://github.com/lucheshidi/DuckShell/actions/workflows/windows.yml/badge.svg)](https://github.com/lucheshidi/DuckShell/actions/workflows/windows.yml)
[![Linux Build](https://github.com/lucheshidi/DuckShell/actions/workflows/linux.yml/badge.svg)](https://github.com/lucheshidi/DuckShell/actions/workflows/linux.yml)
[![macOS Build](https://github.com/lucheshidi/DuckShell/actions/workflows/macos.yml/badge.svg)](https://github.com/lucheshidi/DuckShell/actions/workflows/macos.yml)

[English](README.md) | ***简体中文***

DuckShell 是一款轻量级、跨平台且高度可扩展的现代 Shell。它旨在通过强大的 C++ 插件系统，为开发者提供一个简洁而功能丰富的命令行环境。

## 🌟 核心特性

- **极致轻量**：核心程序仅约 20 MB，启动即瞬完成。
- **真正的跨平台**：原生支持 Windows (x86_64, ARM64)、Linux 和 macOS。
- **强大的插件系统**：支持使用 C++ 编写自定义插件，功能无限扩展。
- **自动化构建**：基于 CMake FetchContent，实现“零手动依赖”安装，自动处理 zlib 和 minizip。
- **开源精神**：完全开源，欢迎社区贡献。

## 📥 安装指南

您可以从 [Latest Release](https://github.com/lucheshidi/DuckShell/releases/latest) 页面下载对应平台的预编译二进制文件。

### 支持架构
- **Windows**: `x86_64` (MinGW), `ARM64` (Clang)
- **Linux**: `amd64`, `arm64`
- **macOS**: `Intel`, `Apple Silicon` (Universal .dmg)

## 🛠️ 编译说明

DuckShell 现已实现自动化依赖管理，您无需手动安装 `zlib` 或 `minizip`。

### 前置要求
- CMake (>= 3.15)
- C++17 兼容编译器 (GCC, Clang, 或 MSYS2 MinGW)
- (仅 Linux/macOS) `libcurl` 开发库

### 构建步骤
```bash
# 克隆仓库
git clone https://github.com/lucheshidi/DuckShell.git
cd DuckShell

# 配置并构建
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

## 🚀 使用方法

- **Windows**: 直接运行 `DuckShell.exe`。
- **Unix-like**: 运行 `./DuckShell`。

### 常用指令
- `help`: 显示帮助信息。
- `cls` / `clear`: 清屏。
- `plugin`: 进入插件管理模式。

### Profile 与脚本
- 启动时读取 `~/duckshell/profile.dsh`；设置 `DUCKSHELL_PROFILE` 可指定其他文件，传入 `--norc` 可跳过加载。
- 与 PowerShell 类似，变量 `PROFILE` 保存 profile 路径，例如 `notepad.exe ${PROFILE}`；变量展开会保留为一个参数，路径含空格也不需要加引号。
- 内置命令 `reload` 可重置所有变量、函数和别名并重新加载 profile；`reload --profile-only` 只重新加载 profile，保留核心变量（PROFILE、HOME、PATH、PROMPT 等）。
- 使用 `DuckShell file.dsh [arguments...]` 运行脚本。当前支持基础 POSIX 风格条件、循环和函数，以及 VBScript 风格的 `If`、`For`、`Do`、`Function`、`Sub` 代码块。
- Minecraft 颜色码可直接写在 profile 和命令中：`§a`、`&c`、`{gold}`、`{bold}`、`{#ff8800}`、`&#ff8800`，也支持 `§x§f§f§8§8§0§0`；`§` 颜色码不需要写成 `\u` 转义。颜色转换默认启用，设置 `DUCKSHELL_COLOR_CODES=0` 可全局关闭；`DUCKSHELL_EXTERNAL_COLOR_CODES=0` 只关闭传给外部程序的参数转换。
- 在 profile 中设置 `DUCKSHELL_VERSION_COLOR` 可自定义启动时版本信息的颜色，例如 `set DUCKSHELL_VERSION_COLOR = "§a"` 会让版本号显示为绿色。
- `echo` 默认解析常见转义（包括 `\n`、`\r`、`\t`、`\e`、`\xHH`、`\uHHHH`、`\UHHHHHHHH` 和八进制转义）；`-n` 不输出结尾换行，`-E` 只关闭反斜杠转义，不影响颜色码转换。

### 后续可实现
管道与重定向、命令替换、通配符展开、作业控制、`case`/`select`、数组，以及更完整的 POSIX/VBScript 表达式和错误语义，都是自然的后续扩展方向。

## 🔌 插件系统

DuckShell 的精髓在于插件。

- **安装插件**：将编译好的插件（`.dll` / `.so` / `.dylib`）放入 `~/duckshell/plugins/` 目录。
- **远程下载**：支持从配置的仓库自动下载并解压插件（依赖 `minizip` 自动化集成）。
- **编写插件**：参考 `ExamplePlugins/HelloWorld` 示例，使用 `plugins_interface.h` 定义的接口即可快速上手。

## 🤝 参与贡献

我们欢迎任何形式的贡献！

1. Fork 本仓库
2. 创建您的特性分支 (`git checkout -b feature/AmazingFeature`)
3. 提交您的修改 (`git commit -m 'Add some AmazingFeature'`)
4. 推送到分支 (`git push origin feature/AmazingFeature`)
5. 开启一个 Pull Request

---

*DuckShell - 让命令行再次简单。*
