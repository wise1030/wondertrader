#!/usr/bin/env python3
"""
WtFutuCore 配置示例校验脚本

功能:
  1. 从源代码提取实际读取的配置键
  2. 扫描示例配置文件, 检查冗余/缺失/命名风格
  3. 输出校验报告, 供 CI 使用

用法:
  python3 scripts/check_config_schema.py [--strict]
"""

import argparse
import os
import re
import sys
from pathlib import Path

try:
    import yaml
except ImportError:
    print("ERROR: PyYAML not installed. Run: pip install pyyaml")
    sys.exit(1)


ROOT = Path(__file__).resolve().parent.parent
SRC_DIR = ROOT / "src" / "WtFutuCore"
CONFIG_EXAMPLES = SRC_DIR / "config"
DIST_DIRS = [
    ROOT / "dist" / "WtBtFutu",
    ROOT / "dist" / "WtRunnerFutu",
]


def extract_source_keys():
    """从源代码提取配置键"""
    keys = {
        "config": set(),       # FutuConfigLoader.cpp 读取的主配置键
        "coordinator": set(),  # StrategyCoordinator.cpp 读取的协调器键
        "hotparams": set(),    # FutuHotParamManager.cpp 注册的热参数键
    }

    # FutuConfigLoader.cpp - config.yaml 顶层及嵌套键
    loader = SRC_DIR / "FutuConfigLoader.cpp"
    if loader.exists():
        content = loader.read_text(encoding="utf-8")
        # 提取字符串字面量作为候选键
        for m in re.finditer(r'"([a-zA-Z_][a-zA-Z0-9_]*)"', content):
            keys["config"].add(m.group(1))

    # StrategyCoordinator.cpp - coordinator.yaml 键
    coord = SRC_DIR / "StrategyCoordinator.cpp"
    if coord.exists():
        content = coord.read_text(encoding="utf-8")
        for m in re.finditer(r'"([a-zA-Z_][a-zA-Z0-9_]*)"', content):
            keys["coordinator"].add(m.group(1))

    # FutuHotParamManager.cpp - hotparams.yaml 键 (snake_case)
    hot = SRC_DIR / "FutuHotParamManager.cpp"
    if hot.exists():
        content = hot.read_text(encoding="utf-8")
        for m in re.finditer(r'"([a-zA-Z_][a-zA-Z0-9_]*)"', content):
            keys["hotparams"].add(m.group(1))

    return keys


def flatten_yaml(data, parent="", sep="."):
    """展平 YAML 为点分键路径"""
    items = {}
    if isinstance(data, dict):
        for k, v in data.items():
            new_key = f"{parent}{sep}{k}" if parent else k
            if isinstance(v, dict):
                items.update(flatten_yaml(v, new_key, sep))
            else:
                items[new_key] = v
    return items


def load_yaml_keys(filepath):
    """加载 YAML 并返回键集合"""
    if not filepath.exists():
        return None
    try:
        with open(filepath, "r", encoding="utf-8") as f:
            data = yaml.safe_load(f)
        return set(flatten_yaml(data).keys())
    except Exception as e:
        print(f"  YAML ERROR in {filepath}: {e}")
        return set()


def check_file(name, filepath, source_keys, strict=False):
    """校验单个配置文件"""
    print(f"\n=== {name} ===")
    print(f"  Path: {filepath}")

    if not filepath.exists():
        print("  STATUS: MISSING")
        return 1 if strict else 0

    config_keys = load_yaml_keys(filepath)
    if config_keys is None:
        return 1

    # 只比较叶子键名, 忽略嵌套路径
    config_leaf = {k.split(".")[-1] for k in config_keys}
    src_leaf = {k for k in source_keys}

    # 过滤掉明显的非配置字符串
    noise = {"OFF", "ON", "coordinator", "modules", "enabled", "type", "id", "name", "path"}
    config_leaf -= noise
    src_leaf -= noise

    extra = config_leaf - src_leaf
    missing = src_leaf - config_leaf

    print(f"  Keys in config: {len(config_leaf)}")
    print(f"  Keys in source: {len(src_leaf)}")

    if extra:
        print(f"  [WARN] Extra keys (config has, source not read): {sorted(extra)}")
    if missing:
        print(f"  [WARN] Missing keys (source reads, config not has): {sorted(missing)}")

    if not extra and not missing:
        print("  STATUS: OK")
        return 0
    return 1


def check_naming_style(filepath):
    """检查 YAML 键名风格一致性"""
    issues = []
    with open(filepath, "r", encoding="utf-8") as f:
        for lineno, line in enumerate(f, 1):
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            m = re.match(r"^([a-zA-Z_][a-zA-Z0-9_]*)\s*:", line)
            if m:
                key = m.group(1)
                # snake_case 包含下划线, camelCase 不包含
                if "_" in key:
                    issues.append((lineno, key, "snake_case"))
    return issues


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--strict", action="store_true", help="任何不匹配都返回非零")
    args = parser.parse_args()

    print("=" * 60)
    print("WtFutuCore Config Schema Check")
    print("=" * 60)

    src_keys = extract_source_keys()
    errors = 0

    # 检查 src/WtFutuCore/config/ 权威示例
    print("\n[1] 权威示例 (src/WtFutuCore/config/)")
    errors += check_file(
        "config.yaml",
        CONFIG_EXAMPLES / "config.yaml",
        src_keys["config"],
        args.strict,
    )
    errors += check_file(
        "coordinator.yaml",
        CONFIG_EXAMPLES / "coordinator.yaml",
        src_keys["coordinator"],
        args.strict,
    )
    errors += check_file(
        "hotparams.yaml",
        CONFIG_EXAMPLES / "hotparams.yaml",
        src_keys["hotparams"],
        args.strict,
    )

    # 检查 dist 目录示例
    print("\n[2] dist 目录示例")
    for dist_dir in DIST_DIRS:
        if not dist_dir.exists():
            continue
        print(f"\n--- {dist_dir.name} ---")
        for cfg in ["config.yaml", "coordinator.yaml", "hotparams.yaml"]:
            filepath = dist_dir / cfg
            key_type = "hotparams" if cfg == "hotparams.yaml" else ("coordinator" if cfg == "coordinator.yaml" else "config")
            errors += check_file(cfg, filepath, src_keys[key_type], args.strict)

    # 命名风格检查 (仅 hotparams 应为 snake_case)
    print("\n[3] 命名风格检查")
    for cfg in ["config.yaml", "coordinator.yaml"]:
        filepath = CONFIG_EXAMPLES / cfg
        if filepath.exists():
            issues = check_naming_style(filepath)
            if issues:
                print(f"  {cfg}: 发现 snake_case 键 (应使用 camelCase):")
                for lineno, key, style in issues[:10]:
                    print(f"    line {lineno}: {key}")
                if len(issues) > 10:
                    print(f"    ... 共 {len(issues)} 处")

    print("\n" + "=" * 60)
    if errors == 0:
        print("RESULT: PASS")
    else:
        print(f"RESULT: {errors} file(s) with issues")
    print("=" * 60)

    return 1 if (args.strict and errors > 0) else 0


if __name__ == "__main__":
    sys.exit(main())
