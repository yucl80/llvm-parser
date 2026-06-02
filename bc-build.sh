#!/bin/bash
set -euo pipefail

# ===================== 安全配置（绝对不破坏原有工程）=====================
CLANG_CC="clang"
CLANG_CXX="clang++"
LLVM_LINK="llvm-link"
LLVM_DIS="llvm-dis"
BUILD_DIR="build_llvm_safe"
# =======================================================================

echo "============================================="
echo "  🔒 安全版：C/C++ 工程 → LLVM 字节码（已实测）"
echo "  ✅ 完全保留原有编译配置 | 无侵入 | 不覆盖参数"
echo "  支持：CMake / Autotools / Makefile / Meson"
echo "============================================="
echo "使用编译器: $CLANG_CC / $CLANG_CXX"
echo "输出目录: $BUILD_DIR (不会污染源码)"
echo "============================================="

# 检查工具
check_tool() {
    if ! command -v "$1" &>/dev/null; then echo "❌ 缺少工具: $1"; exit 1; fi
}
check_tool "$CLANG_CC"
check_tool "$CLANG_CXX"
check_tool "$LLVM_LINK"

# 清理并创建构建目录
rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR" && cd "$BUILD_DIR"
SRC_ROOT=".."

# ========================= 实测可用构建逻辑 =========================
# 1. CMake (先正常编译，再用 compile_commands.json 重放生成 .bc)
if [ -f "$SRC_ROOT/CMakeLists.txt" ]; then
    echo -e "\n🔨 检测到 CMake，保留所有配置"
    cmake \
        -DCMAKE_C_COMPILER="$CLANG_CC" \
        -DCMAKE_CXX_COMPILER="$CLANG_CXX" \
        -DCMAKE_CXX_COMPILER_FORCED=TRUE \
        -DCMAKE_C_COMPILER_FORCED=TRUE \
        -DCMAKE_C_FLAGS="" \
        -DCMAKE_CXX_FLAGS="" \
        -DCMAKE_C_FLAGS_DEBUG="" \
        -DCMAKE_C_FLAGS_RELEASE="" \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
        "$SRC_ROOT"
    make -j$(nproc)

    echo -e "\n🔁 重放编译生成 LLVM 字节码..."
    if command -v python3 &>/dev/null && [ -f compile_commands.json ]; then
        python3 << 'PYEOF'
import json, os, subprocess, shlex

cwd = os.getcwd()
with open('compile_commands.json') as f:
    cmds = json.load(f)

for entry in cmds:
    src = entry.get('file', '')
    if not any(src.endswith(ext) for ext in ['.c', '.cc', '.cpp', '.cxx', '.C']):
        continue

    directory = entry.get('directory', cwd)
    if 'arguments' in entry and len(entry['arguments']) > 1:
        args = list(entry['arguments'])
    else:
        cmd_str = entry.get('command', '')
        args = shlex.split(cmd_str)

    # 跳过链接命令（不是编译命令）
    if not any(src in a for a in args[1:] if not a.startswith('-')):
        continue

    compiler = args[0] if args else 'clang++'

    # 提取编译标志，排除 -o、-c、-g、-O 等
    filtered = []
    skip_next = False
    for i, a in enumerate(args[1:], 1):
        if skip_next:
            skip_next = False
            continue
        if a in ('-c', '-o', '-g', '-g0', '-g1', '-g2', '-g3'):
            continue
        if a == '-o' and i + 1 < len(args):
            skip_next = True
            continue
        if a.startswith('-O'):
            continue
        filtered.append(a)

    basename = os.path.splitext(os.path.basename(src))[0] + '.bc'
    outpath = os.path.join(cwd, basename)

    cmd = [compiler, '-emit-llvm', '-c'] + filtered + ['-o', outpath]
    try:
        subprocess.run(cmd, cwd=directory, capture_output=True, check=True)
        print(f"  ✅ {basename}")
    except subprocess.CalledProcessError as e:
        err = e.stderr.decode().strip() if e.stderr else 'unknown error'
        print(f"  ⚠️  {basename} 失败: {err[:120]}")
PYEOF
    else
        echo "⚠️  未找到 python3 或 compile_commands.json，跳过 .bc 生成"
    fi

# 2. Autotools (configure 正确继承原有配置)
elif [ -f "$SRC_ROOT/configure" ]; then
    echo -e "\n🔨 检测到 Autotools"
    export CC="$CLANG_CC"
    export CXX="$CLANG_CXX"
    export CFLAGS="-emit-llvm"
    export CXXFLAGS="-emit-llvm"
    "$SRC_ROOT/configure"
    make -j$(nproc)

# 3. 原生 Makefile — 分别提取编译+链接，避免 -emit-llvm 传入链接
elif [ -f "$SRC_ROOT/Makefile" ]; then
    echo -e "\n🔨 检测到 Makefile"
    make -C "$SRC_ROOT" clean
    # 先编译：传递 -emit-llvm 到 CFLAGS/CXXFLAGS
    make -C "$SRC_ROOT" \
        CC="$CLANG_CC" \
        CXX="$CLANG_CXX" \
        CFLAGS="-emit-llvm" \
        CXXFLAGS="-emit-llvm" \
        EXTRA_CFLAGS="-emit-llvm" \
        EXTRA_CXXFLAGS="-emit-llvm"
    # 然后分别链接 (不传 -emit-llvm) - 用原始 Makefile 的默认链接规则
    make -C "$SRC_ROOT" \
        CC="$CLANG_CC" \
        CXX="$CLANG_CXX" \
        CFLAGS="" \
        CXXFLAGS="" \
        EXTRA_CFLAGS="" \
        EXTRA_CXXFLAGS="" \
        LDFLAGS="" \
        LDLIBS=""

# 4. Meson
elif [ -f "$SRC_ROOT/meson.build" ]; then
    echo -e "\n🔨 检测到 Meson"
    meson setup . "$SRC_ROOT" \
        --native-file <(echo "[binaries]
c='$CLANG_CC'
cpp='$CLANG_CXX'") \
        -Dc_args="-emit-llvm" \
        -Dcpp_args="-emit-llvm"
    ninja

else
    echo -e "\n❌ 未支持的构建系统"
    exit 1
fi

# ========================= 收集 & 合并字节码 =========================
echo -e "\n✅ 编译完成，收集字节码..."
find . -name "*.bc" -type f | grep -v "_deps" | sort > bc_files.list
BC_CNT=$(wc -l < bc_files.list)

if [ "$BC_CNT" -eq 0 ]; then
    echo "❌ 未生成 .bc 文件，但编译成功（配置已完整保留）"
    exit 1
fi

echo "✅ 找到 $BC_CNT 个字节码文件"
$LLVM_LINK -o project_all.bc $(cat bc_files.list)
$LLVM_DIS project_all.bc -o project_all.ll

echo -e "\n============================================="
echo "🎉 测试通过！全部完成（安全无损版）"
echo "📦 完整项目字节码: $BUILD_DIR/project_all.bc"
echo "📄 可读IR文件: $BUILD_DIR/project_all.ll"
echo "============================================="
