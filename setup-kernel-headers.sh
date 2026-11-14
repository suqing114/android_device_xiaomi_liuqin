#!/bin/bash
# ============================================================
#  为预编译内核提供 KERNEL_OBJ/usr 头文件目录
#
#  背景: 本设备 TARGET_FORCE_PREBUILT_KERNEL := true
#        -> FULL_KERNEL_BUILD := false, 构建系统不生成
#           $(TARGET_OUT_INTERMEDIATES)/KERNEL_OBJ/usr 的规则,
#        而 QTI 音频模块声明了
#           LOCAL_ADDITIONAL_DEPENDENCIES := $(TARGET_OUT_INTERMEDIATES)/KERNEL_OBJ/usr
#        ninja 于是报 "missing and no known rule to make it"。
#
#  关键约束 (踩过的坑):
#        liblx-osal 等模块把该目录以 -I 传入, 排在 bionic 之前 (-isystem bionic/libc/include)。
#        因此这里 **绝不能** 放任何 bionic 已提供的 libc/内核头,
#        否则 bionic/libc/include/sys/types.h 里的 #include <linux/types.h>
#        会命中我们的副本, 触发:
#          error: "Attempt to use kernel headers from user space"
#          error: redefinition of 'sigaction'   (asm-generic/signal.h 与 bionic 冲突)
#
#  做法: 只链接「bionic 内核 UAPI 里没有的」QTI/设备专有头 (如 linux/msm_audio.h
#        linux/msm_ion.h linux/dma-buf.h ...)。先扫描 bionic 的 UAPI 集合,
#        再逐文件判断是否已存在, 只补缺失的。
#  幂等: 关键头齐备则直接返回。
# ============================================================
set -uo pipefail
SRC="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../.." && pwd -P)"
OUT="${OUT_DIR:-out}"
DEV="${TARGET_DEVICE:-liuqin}"
KOBJ="$SRC/$OUT/target/product/$DEV/obj/KERNEL_OBJ/usr"

cd "$SRC" || exit 0

python3 - "$SRC" "$OUT" "$DEV" <<'PY' || exit 0
import os, sys, time

src_root, out, dev = sys.argv[1], sys.argv[2], sys.argv[3]
usr = os.path.join(src_root, out, 'target/product', dev, 'obj/KERNEL_OBJ/usr')
inc = os.path.join(usr, 'include')

need = ['linux/msm_audio.h', 'linux/msm_ion.h', 'linux/dma-buf.h',
        'linux/dma-heap.h', 'sound/asound.h']

# --- 1. 收集 bionic 已经提供的内核 UAPI 头 (这些绝不能放进来) ---
bionic_roots = [
    'bionic/libc/kernel/uapi',
    'bionic/libc/kernel/uapi/asm-arm64',
    'bionic/libc/kernel/android/uapi',
    'bionic/libc/kernel/android/scsi',
]
blocked = set()
for r in bionic_roots:
    p = os.path.join(src_root, r)
    if not os.path.isdir(p):
        continue
    for dirpath, _, files in os.walk(p):
        sub = os.path.relpath(dirpath, p)
        for fn in files:
            rel = fn if sub == '.' else os.path.join(sub, fn)
            blocked.add(rel)

# --- 2. 候选来源, 按优先级 ---
sources = [
    'device/xiaomi/liuqin-kernel/kernel-headers',
    'kernel/xiaomi/sm8450-modules/qcom/opensource/audio-kernel/include/uapi',
    'kernel/xiaomi/sm8450/include/uapi',
]

if all(os.path.exists(os.path.join(inc, h)) for h in need):
    # 仍需清理可能存在的 bionic 冲突项
    stale = [h for h in blocked if os.path.lexists(os.path.join(inc, h))]
    if not stale:
        sys.exit(0)

# 重建: 干净起步, 避免历史遗留的冲突链接
import shutil
if os.path.isdir(inc):
    shutil.rmtree(inc)
os.makedirs(inc, exist_ok=True)

linked = skipped_bionic = 0
for rel in sources:
    root = os.path.join(src_root, rel)
    if not os.path.isdir(root):
        continue
    for dirpath, _, files in os.walk(root):
        sub = os.path.relpath(dirpath, root)
        for fn in files:
            relf = fn if sub == '.' else os.path.join(sub, fn)
            if relf in blocked:          # bionic 已提供 -> 跳过, 防止覆盖
                skipped_bionic += 1
                continue
            dest = os.path.join(inc, relf)
            if os.path.lexists(dest):
                continue
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            try:
                os.symlink(os.path.join(dirpath, fn), dest)
                linked += 1
            except OSError:
                pass

# --- 兜底: 内核宏头 (__user/__force) ---
# 设备树的 UAPI 头 (如 sound/devdep_params.h) 直接用 __user,
# 但其 #include <linux/compiler.h> 在农场里不存在 (bionic 只在 android/uapi 下提供),
# 而农场 -I 排在 bionic 之前, 故需在此提供同一份兜底头。
import shutil as _sh
_comp = os.path.join(src_root, 'bionic/libc/kernel/android/uapi/linux/compiler.h')
if os.path.exists(_comp):
    _dst = os.path.join(inc, 'linux/compiler.h')
    os.makedirs(os.path.dirname(_dst), exist_ok=True)
    if not os.path.lexists(_dst):
        _sh.copy2(_comp, _dst)

# --- 农场专用包装: linux/types.h ---
# 设备树 UAPI 头直接用 __user/__force, 而 bionic 的 linux/types.h 不含这些宏。
# 农场用 -I 前置, 故在此包装: 先补宏, 再 include_next 转发 bionic 原版。
_types_wrapper = """#ifndef _MIKUI_KERNEL_OBJ_LINUX_TYPES_WRAPPER_H
#define _MIKUI_KERNEL_OBJ_LINUX_TYPES_WRAPPER_H
#ifndef __user
#define __user
#endif
#ifndef __force
#define __force
#endif
#ifndef __bitwise
#define __bitwise
#endif
#include_next <linux/types.h>
#endif
"""
_dst = os.path.join(inc, 'linux/types.h')
os.makedirs(os.path.dirname(_dst), exist_ok=True)
with open(_dst, 'w') as _f:
    _f.write(_types_wrapper)

missing = [h for h in need if not os.path.exists(os.path.join(inc, h))]
log = os.path.join(src_root, out, 'kernel-headers.log')
nfiles = sum(len(fs) for _, _, fs in os.walk(inc))
with open(log, 'a') as f:
    f.write(f'[kernel-headers] {time.strftime("%Y-%m-%dT%H:%M:%S")} dev={dev}\n')
    f.write(f'  linked={linked} skipped_bionic={skipped_bionic} total={nfiles} '
            f'MISSING={",".join(missing) if missing else "none"}\n')
PY

exit 0
