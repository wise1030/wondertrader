# Repository Guidelines

## Project Structure & Module Organization
WonderTrader is a C++17 quant-trading framework. All sources live in `src/` (CMake root: `src/CMakeLists.txt`):
- Core engines: `WtCore`, `WtBtCore` (backtest), `WtUftCore` (ultra-low-latency HFT), `WtDtCore`/`WtDataStorage` (market data)
- Strategy modules: `WtCtaStraFact`, `WtHftStraFact`, `WtUftStraFact`, `WtSelStraFact`, plus in-house desks `WtFutuCore` (futures market making) and `WtOptionCore`
- Brokerage gateways: `ParserCTP*`/`TraderCTP*` and other `Parser*`/`Trader*` pairs (market data / order)
- Runners & tools: `WtRunner`, `WtBtRunner`, `WtUftRunner`, `WtPorter`, `WtLatencyHFT`/`WtLatencyUFT`
- Shared code: `Share`, `Includes`, `Common`, `API`
- Tests: `src/TestUnits` (GoogleTest) plus `Test*` harness apps; `src/WtOptionCore/tests` (module-local, plain main() style)
- `ui/optiontrader-ui/` WtOptionCore 监控 UI（P1 只读版；设计: `src/WtOptionCore/analysis/ui-migration-design-2026-11-16.md`）：纯 Python middleware + React19 SPA，经策略内 UiBridge（127.0.0.1:8710 JSON Lines）对接
- `dist/` ready-to-run bundles (e.g. `dist/WtBtFutu`, `dist/WtRunnerFutu`), `deps/` third-party libs, `docker/` Linux build image, `docs/`

## Build, Test, and Development Commands
- Linux full build: `cd src/build_all && cmake .. && make -j$(nproc)` (helpers: `src/build_release.sh`, `src/build_debug.sh`)
- Single module: `make -j$(nproc) WtFutuCore` from the build dir
- Windows: open `src/all.sln` (or focused ones like `uft.sln`, `backtest.sln`) in MSVC; requires `MyDepends141` env var pointing at bundled dependencies
- Unit tests: build target `TestUnits`, then run the produced binary
- Backtest smoke test: `cd dist/WtBtFutu && LD_LIBRARY_PATH=./uft ./uft/WtBtRunner -c config.yaml -l logcfgbt.yaml`
- ⚠ `/mnt/d`（drvfs/WSL）时间戳不可靠，make 增量编译可能产出混合布局 .o（玄学级运行时故障）。发布/排障构建务必用 Linux 原生文件系统：先 `rsync -a --exclude='build_*' src/ ~/wt_src/`，再 `cmake ~/wt_src -B ~/build/rel -DCMAKE_BUILD_TYPE=Release && cmake --build ~/build/rel -j`；改动头文件后全量重建。
- ⚠ 快照/TickData 等公共头文件改动并重编 `libWtOptionCore.so` 后，必须同步重编 `test_*` 测试二进制再跑回归（`.so` 与旧测试二进制 ABI 不同步会出现假阴性/假阳性）。

## Coding Style & Naming Conventions
- Match each file's existing indentation (framework core uses tabs; some strategy modules use spaces)
- Types/classes PascalCase; member variables prefixed `_` (`_cfg`, `_lock`); `.h/.cpp` file pairs named after the class
- Namespaces: `wtp` for framework code (`NS_WTP_BEGIN`), `futu` for strategy code
- No repo-wide formatter/linter is enforced; keep diffs minimal and consistent with surrounding code

## Testing Guidelines
- Framework: GoogleTest in `src/TestUnits`; files named `test_<module>.cpp`
- No coverage gate. For strategy/risk changes, attach backtest evidence (trades/funds/positions CSVs under `dist/WtBtFutu/outputs_bt/`)

## Commit & Pull Request Guidelines
- Conventional Commits with a scope, Chinese or English body: `fix(arb): ...`, `feat(v7.2): ...`, `tune(arb): ...`, `refactor(SpreadArbMgr): ...`, `docs: ...`
- PRs should state the root cause, the fix, and verification (unit test or backtest result); link issues where applicable
- Sub-directory rules: `src/WtFutuCore/AGENTS.md` defines a stricter workflow (plan-before-edit, framework files off-limits) — follow it when working in that module
