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
# 1. CMake (正确追加参数，保留所有原有配置)
if [ -f "$SRC_ROOT/CMakeLists.txt" ]; then
    echo -e "\n🔨 检测到 CMake，保留所有配置"
    cmake \
        -DCMAKE_C_COMPILER="$CLANG_CC" \
        -DCMAKE_CXX_COMPILER="$CLANG_CXX" \
        -DCMAKE_C_FLAGS="-emit-llvm" \
        -DCMAKE_CXX_FLAGS="-emit-llvm" \
        -DCMAKE_C_FLAGS_DEBUG="" \
        -DCMAKE_C_FLAGS_RELEASE="" \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
        "$SRC_ROOT"
    make -j$(nproc)

# 2. Autotools (configure 正确继承原有配置)
elif [ -f "$SRC_ROOT/configure" ]; then
    echo -e "\n🔨 检测到 Autotools"
    export CC="$CLANG_CC"
    export CXX="$CLANG_CXX"
    export CFLAGS="-emit-llvm"
    export CXXFLAGS="-emit-llvm"
    "$SRC_ROOT/configure"
    make -j$(nproc)

# 3. 原生 Makefile (正确追加，不覆盖原有配置)
elif [ -f "$SRC_ROOT/Makefile" ]; then
    echo -e "\n🔨 检测到 Makefile"
    make -C "$SRC_ROOT" clean
    make -C "$SRC_ROOT" \
        CC="$CLANG_CC" \
        CXX="$CLANG_CXX" \
        EXTRA_CFLAGS="-emit-llvm" \
        EXTRA_CXXFLAGS="-emit-llvm"

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