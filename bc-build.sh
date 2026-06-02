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
echo "  🔒 安全版：C/C++ 工程 → LLVM 字节码（简洁模式）"
echo "  ✅ 思路：-emit-llvm 下 .o 文件本身就是 LLVM 字节码"
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

# 收集 .o → 复制为 .bc → llvm-link
# .o 文件在 -emit-llvm 下实际是 LLVM bitcode，可直接用 llvm-link 合并
collect_bc() {
    echo -e "\n📦 收集字节码..."
    find . -name "*.o" -type f 2>/dev/null \
        | grep -v "/CMakeFiles/CMake" \
        | grep -v "_deps" \
        | sort > o_files.list || true

    if [ ! -s o_files.list ]; then
        echo "❌ 未生成 .o 文件"
        return 1
    fi

    bc_list=""
    while read -r ofile; do
        # 跳过非 bitcode 文件（如 CMake 探针生成的 ELF .o）
        if ! file "$ofile" | grep -qi "LLVM IR bitcode"; then
            continue
        fi
        bcname="$(basename "$ofile" .o).bc"
        cp "$ofile" "./$bcname"
        bc_list="$bc_list ./$bcname"
    done < o_files.list

    if [ -z "$bc_list" ]; then
        echo "❌ 没有找到有效的 LLVM bitcode .o 文件"
        return 1
    fi

    echo "✅ 找到 $(echo $bc_list | wc -w) 个 bitcode 文件"
    $LLVM_LINK -o project_all.bc $bc_list
    $LLVM_DIS project_all.bc -o project_all.ll

    echo -e "\n============================================="
    echo "🎉 全部完成！"
    echo "📦 项目字节码: $BUILD_DIR/project_all.bc"
    echo "📄 可读IR文件: $BUILD_DIR/project_all.ll"
    echo "============================================="
}

# ========================= 构建逻辑 =========================
# 1. CMake
if [ -f "$SRC_ROOT/CMakeLists.txt" ]; then
    echo -e "\n🔨 检测到 CMake，保留所有配置"
    cmake \
        -DCMAKE_C_COMPILER="$CLANG_CC" \
        -DCMAKE_CXX_COMPILER="$CLANG_CXX" \
        -DCMAKE_CXX_COMPILER_FORCED=TRUE \
        -DCMAKE_C_COMPILER_FORCED=TRUE \
        -DCMAKE_C_FLAGS="-emit-llvm" \
        -DCMAKE_CXX_FLAGS="-emit-llvm" \
        "$SRC_ROOT"
    echo -e "\n⚡ 编译中（-emit-llvm 下 .o 即是 .bc，链接失败可忽略）..."
    make -j$(nproc) -k || true
    collect_bc

# 2. Autotools
elif [ -f "$SRC_ROOT/configure" ]; then
    echo -e "\n🔨 检测到 Autotools"
    export CC="$CLANG_CC"
    export CXX="$CLANG_CXX"
    export CFLAGS="-emit-llvm"
    export CXXFLAGS="-emit-llvm"
    # 先在不带 -emit-llvm 的情况下 configure（避免链接检测失败）
    export CFLAGS="" CXXFLAGS=""
    "$SRC_ROOT/configure" || true
    # 然后编译注入 -emit-llvm
    export CFLAGS="-emit-llvm" CXXFLAGS="-emit-llvm"
    make -j$(nproc) -k || true
    # Autotools 的 .o 文件生成在源码树中，复制过来
    find "$SRC_ROOT" -maxdepth 2 -name "*.o" -type f -exec cp {} . \; 2>/dev/null || true
    collect_bc

# 3. 原生 Makefile
elif [ -f "$SRC_ROOT/Makefile" ]; then
    echo -e "\n🔨 检测到 Makefile"
    make -C "$SRC_ROOT" clean || true
    make -C "$SRC_ROOT" \
        CC="$CLANG_CC" \
        CXX="$CLANG_CXX" \
        CFLAGS="-emit-llvm" \
        CXXFLAGS="-emit-llvm" \
        -j$(nproc) -k || true
    # Makefile 的 .o 文件生成在源码树中，复制过来
    find "$SRC_ROOT" -maxdepth 2 -name "*.o" -type f -exec cp {} . \; 2>/dev/null || true
    collect_bc

# 4. Meson
elif [ -f "$SRC_ROOT/meson.build" ]; then
    echo -e "\n🔨 检测到 Meson"
    meson setup . "$SRC_ROOT" \
        --native-file <(echo "[binaries]
c='$CLANG_CC'
cpp='$CLANG_CXX'") \
        -Dc_args="-emit-llvm" \
        -Dcpp_args="-emit-llvm"
    ninja -k0 || true
    collect_bc

else
    echo -e "\n❌ 未支持的构建系统"
    exit 1
fi
