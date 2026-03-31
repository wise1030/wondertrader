# WonderTrader 部署与安装指南

本文档详细说明 WonderTrader 的编译、安装、部署及运维流程。

## 1. 环境准备 (Prerequisites)

### Windows 环境
*   **操作系统**: Windows 10 / Server 2016 及以上
*   **编译器**: Visual Studio 2017 或更高版本 (支持 C++17)
*   **依赖库**:
    *   Boost 1.72+
    *   RapidJSON 1.0.2
    *   spdlog 1.9.2
    *   nanomsg 1.1.5
*   **工具**: CMake 3.17+, Git, Python 3.8+ (用于脚本和 wtpy)

### Linux 环境
*   **操作系统**: CentOS 7+ / Ubuntu 18.04+
*   **编译器**: GCC 8.4.0+
*   **工具**: CMake 3.17+, Git, Python 3.8+

---

## 2. 编译指南 (Build Instructions)

### Windows 编译

1.  打开 `src/` 目录。
2.  使用 Visual Studio 打开 `all.sln` 解决方案文件。
3.  选择构建配置 (通常为 `Release` / `x64`)。
4.  右键点击解决方案 -> **生成解决方案 (Build Solution)**。
5.  编译产出位于 `src/x64/Release/` (或对应配置目录)。

**命令行编译脚本 (参考):**
```bat
:: 使用提供的自动化编译脚本
cd scripts/deployment
build_win.bat
```

### Linux 编译

1.  进入项目根目录。
2.  执行构建脚本（**脚本会自动检查并尝试安装缺失的依赖，如 CMake, Boost 等**）：
    ```bash
    cd scripts/deployment
    chmod +x build_linux.sh
    ./build_linux.sh
    ```
3.  编译产出将生成在 `src/build_all/` 目录下。

---

## 3. 部署流程 (Deployment)

### 使用部署脚本 (推荐)
已提供自动化部署脚本，位于 `scripts/deployment/`：

**Windows**:
```bat
cd scripts/deployment
deploy_win.bat
```

**Linux**:
```bash
cd scripts/deployment
chmod +x deploy_linux.sh
./deploy_linux.sh
```

脚本将自动执行以下操作：
1.  创建 `dist/` 部署目录。
2.  复制可执行文件和核心库。
3.  复制插件和依赖。
4.  复制启动脚本。

### 手动部署结构规划
如果手动部署，建议的运行目录结构：
```
deploy/
├── bin/                # 可执行文件 (WtRunner.exe / WtRunner)
├── config/             # 配置文件 (config.yaml, logcfg.yaml, strategies.json)
├── dll/                # 动态库/插件 (CTP.dll, WtPorter.dll 等)
├── data/               # 行情数据存储
├── logs/               # 运行日志
└── scripts/            # 运维脚本 (start.bat, stop.bat)
```

### 部署步骤
1.  **创建目录**: 按照上述结构创建文件夹。
2.  **复制二进制**:
    *   将 `WtRunner.exe` (或 Linux 可执行文件) 复制到 `bin/`。
    *   将编译生成的模块 DLL/SO (`ParserCTP`, `TraderCTP`, `WtCtaStraFact` 等) 复制到 `bin/` 或 `dll/` (需配置加载路径)。
    *   将依赖库 (`nanomsg.dll`, `boost_*.dll`) 复制到 `bin/`。
3.  **配置环境**:
    *   复制 `config.yaml` 到 `config/` 并根据实盘环境修改账户、行情源信息。
    *   配置 `logcfg.yaml` 设置日志级别和输出路径。

---

## 4. 运维指南 (Operations)

### 启动 (Start)
建议使用生成的启动脚本，它会自动配置环境变量和路径。

**Windows**:
```bat
cd dist
start_runner.bat
```

**Linux**:
```bash
cd dist
./start_runner.sh
```

### 停止 (Stop)
WonderTrader 捕获系统信号进行安全退出。
*   **Windows**: 在控制台按 `Ctrl+C` 或关闭窗口。
*   **Linux**: `kill -15 <pid>` (不要使用 kill -9，会导致数据未落盘)。

### 监控 (Monitor)
*   **日志监控**: 查看 `logs/` 目录下的日志文件 (`WtRunner.log`, `Error.log`).
*   **远程监控**: 使用 `wtpy` 提供的 `WtMonSvr` 搭建 Web 监控台，通过 UDP 接收 WtRunner 的推送状态。
