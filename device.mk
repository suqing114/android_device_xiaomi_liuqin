#
# SPDX-FileCopyrightText: 2023 The LineageOS Project
# SPDX-License-Identifier: Apache-2.0
#

# Fstab: liuqin builds its read-only partitions (system/system_ext/product/
# vendor/vendor_dlkm/odm) as EROFS (see BoardConfig.mk). sm8450-common installs
# an ext4-only fstab.qcom by default; mounting EROFS images with it fails in
# first_stage_mount (EINVAL) and the device reboots to the bootloader at ~14s.
# liuqin/init/fstab.qcom carries dual ext4+erofs entries (fs_mgr tries each in
# order). This must be set BEFORE inheriting common.mk, which picks it up via
# `TARGET_DEVICE_FSTAB ?= <common ext4 fstab>`.
TARGET_DEVICE_FSTAB := $(LOCAL_PATH)/init/fstab.qcom

# WiFi-only tablet: no modem/SIM. Build without the telephony stack so the
# persistent com.android.phone (TeleService) doesn't crash-loop against an
# absent RIL (constant CPU wakeups / battery drain) and Settings doesn't show
# phantom SIM options. Consumed by common.mk's TARGET_HAS_NO_TELEPHONY guard;
# must be set BEFORE inheriting it.
TARGET_HAS_NO_TELEPHONY := true

# WiFi-only tablet with no NFC controller and no secure element. Skip the
# common NFC / SecureElement packages and feature permission XMLs. Without this
# guard common.mk copies android.hardware.se.omapi.{uicc,ese}.xml even though no
# SE HAL exists, so SecureElementService binds a missing HAL and ANRs every 20s
# (executing service com.android.se/.SecureElementService, waited 20006ms).
# Consumed by common.mk's NFC/SE guard; must be set BEFORE inheriting it.
TARGET_HAS_NO_NFC := true

# Mark liuqin as a tablet, matching the official LineageOS pipa (Pad 6) tree
# and the hiper25 liuqin tree. Set before inheriting common.mk so any tablet
# guards see it.
TARGET_IS_TABLET := true

# Register the Dolby DAP effect (uuid 9d4921da-..., backed by libhwdap.so) in
# the audio effects config so the framework's AudioEffect can find/instantiate
# it (the XiaomiDolby app attaches it to the global output mix). sm8450-common
# installs its own no-Dolby audio_effects.xml to this same sku_cape path;
# PRODUCT_COPY_FILES is first-wins for a given destination, so this liuqin copy
# MUST be listed BEFORE the sm8450-common inherit below to take precedence.
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/configs/audio/audio_effects.xml:$(TARGET_COPY_OUT_VENDOR)/etc/audio/sku_cape/audio_effects.xml

# Inherit from xiaomi sm8450-common
$(call inherit-product, device/xiaomi/sm8450-common/common.mk)

# Inherit from the proprietary version (optional - only present after
# extract-files.py has run against a stock liuqin firmware dump)
$(call inherit-product-if-exists, vendor/xiaomi/liuqin/liuqin-vendor.mk)

# Recovery
# Use the QTI minui DRM backend: atomic multi-plane modeset across the panel's
# layer mixers (m81 is dual-DSI + dual-DSC, topology <2 2 2>). The generic minui
# DRM path does a single-plane cold modeset that underruns this panel -> white
# recovery + RGB-garbage off-mode charging. Backend auto-detects lm count from
# the SDE connector topology blob. See android_device_xiaomi_pipa change 463649.
$(call soong_config_set_bool,recovery,target_recovery_uses_qti_drm,true)

# Recovery touch firmware: NT36532 is flashless, the driver downloads these
# ~14s after probe (nvt_fwu_wq). In recovery the kernel only tries direct
# loading (no ueventd fallback, firmware_class.path empty), so the files must
# sit in the kernel's default search path /lib/firmware (verified on device:
# "Direct firmware load ... failed with error -2" when placed elsewhere).
PRODUCT_COPY_FILES +=     $(foreach f,fw_csot fw_tm mp_csot mp_tm,         vendor/xiaomi/liuqin/proprietary/vendor/firmware/novatek_nt36532_m81_$(f).bin:$(TARGET_COPY_OUT_RECOVERY)/root/lib/firmware/novatek_nt36532_m81_$(f).bin)

# Recovery /data decryption: fbe_helper (device/xiaomi/liuqin/recovery_fbe) plus the
# KeyMint HAL declaration, so recovery's servicemanager accepts the vendor KeyMint
# service that fbe_helper starts from the real vendor partition.
PRODUCT_PACKAGES += fbe_helper
PRODUCT_COPY_FILES += \
    vendor/xiaomi/sm8450-common/proprietary/vendor/etc/vintf/manifest/android.hardware.security.keymint-service-qti.xml:$(TARGET_COPY_OUT_RECOVERY)/root/system/etc/vintf/manifest/keymint-recovery.xml

# Soong namespaces
PRODUCT_SOONG_NAMESPACES += \
    $(LOCAL_PATH)

# AAPT
PRODUCT_AAPT_CONFIG := normal
PRODUCT_AAPT_PREF_CONFIG := xxxhdpi
PRODUCT_AAPT_PREBUILT_DPI := xxxhdpi xxhdpi xhdpi hdpi

# Audio overrides for liuqin (CS35L41 quad-amp via TDM tertiary RX).
# These shadow the same-named files inherited from vendor/xiaomi/liuqin
# (which carry the stock sku_cape configuration targeting WSA SoundWire
# hardware that this tablet does not have).
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/audio/mixer_paths_waipio_mtp.xml:$(TARGET_COPY_OUT_VENDOR)/etc/audio/sku_cape/mixer_paths_waipio_mtp.xml \
    $(LOCAL_PATH)/audio/resourcemanager_waipio_mtp.xml:$(TARGET_COPY_OUT_VENDOR)/etc/audio/sku_cape/resourcemanager_waipio_mtp.xml \
    $(LOCAL_PATH)/audio/usecaseKvManager.xml:$(TARGET_COPY_OUT_VENDOR)/etc/usecaseKvManager.xml

# Audio debug tools (tinymix / tinyplay / tinycap / tinypcminfo).
# Useful for verifying mixer kctl names and PCM routing on-device.
PRODUCT_PACKAGES += \
    tinymix \
    tinyplay \
    tinycap \
    tinypcminfo

# Characteristics (Pad 6 Pro is a WiFi-only tablet)
PRODUCT_CHARACTERISTICS := tablet,nosdcard

# Display config (liuqin panel)
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/configs/displayconfig/display_id_4630947141052476290.xml:$(TARGET_COPY_OUT_VENDOR)/etc/displayconfig/display_id_4630947141052476290.xml \
    $(LOCAL_PATH)/configs/displayconfig/display_id_4630947200012256898.xml:$(TARGET_COPY_OUT_VENDOR)/etc/displayconfig/display_id_4630947200012256898.xml

# Dolby Atmos (DAP audio): the Dolby DAP blobs (libhwdap/libswdap/the DMS HAL)
# link the API-33 snapshot of libstagefright_foundation; install the LineageOS
# compat shim so they load on Android 16. It is also pulled in automatically as
# a shared_libs dep of the Dolby prebuilts, but list it explicitly.
#
# The DMS HAL also needs its own VINTF manifest fragment (the firmware ships one
# but it was never extracted): an inline entry in manifest_xiaomi.xml is not
# honored by hwservicemanager at registration -> dms-hal-2-0 crash-loops. The
# fragment module lives in dolby/Android.bp.
PRODUCT_PACKAGES += \
    libstagefright_foundation-v33 \
    vendor.dolby.hardware.dms@2.0-service.xml

# VINTF fragment for the Qualcomm TUI comm HAL (fragment module lives in tui_comm/Android.bp).
# Without it the service cannot register and init restarts it forever.
PRODUCT_PACKAGES += \
    vendor.qti.hardware.tui_comm@1.0-service.xml

# Dolby control app: XiaomiDolby (com.xiaomi.dolby) - the open-source Atmos
# control app (Settings -> Sound entry + QS tile). Its DolbyAtmos class is an
# AudioEffect bound to the DAP effect (uuid 9d4921da-...), so toggling it on
# attaches the dap post-processing effect to the global output mix and restores
# the chosen profile on every playback/device change (DolbyUtils). Requires the
# dap effect to be registered in audio_effects.xml (below) and libhwdap.so +
# the DMS HAL (above). Mirrors the upstream AOSPA bring-up; no MIUI/Dirac deps.
PRODUCT_PACKAGES += \
    XiaomiDolby

# Dolby Vision (video) stays disabled: the dolbycodec2 C2 stack is a separate,
# harder A16 port (see proprietary-files.txt).

# Init scripts (liuqin-specific). init.target.rc and ueventd.xiaomi.rc are
# dropped because sm8450-common already installs its own at the same paths.
# (fstab.qcom is overridden to liuqin's EROFS variant via TARGET_DEVICE_FSTAB,
# set at the top of this file before the sm8450-common inherit.)
PRODUCT_PACKAGES += \
    init.mi_perf.rc \
    init.mi_service.rc

# Input device configuration (stylus + keyboard)
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/configs/idc/Vendor_1915_Product_4d81.idc:$(TARGET_COPY_OUT_VENDOR)/usr/idc/Vendor_1915_Product_4d81.idc \
    $(LOCAL_PATH)/configs/idc/Vendor_1915_Product_eaea.idc:$(TARGET_COPY_OUT_VENDOR)/usr/idc/Vendor_1915_Product_eaea.idc

# Mi Pay / IFAA
PRODUCT_PACKAGES += \
    IFAAService

# Package removals (sensor-notifier, QTI vibrator HAL) are done at their source
# in device/xiaomi/sm8450-common/common.mk, guarded by TARGET_PRODUCT, because
# Android product config is strictly additive: a $(filter-out ...) here cannot
# remove a package added by an inherited makefile (inherit-product only splices
# the inherited PRODUCT_PACKAGES in AFTER this file is fully evaluated, so the
# filter never sees the package name). The previous filter-out lines here were
# silent no-ops. vendor.lineage.health-service.default is intentionally left in:
# the ROM boots fine with it present, so it is not the boot-loop cause.
#
# SecureElement.apk is AOSP-inherited (base_system.mk), so it is dropped via a
# phony package with LOCAL_OVERRIDES_PACKAGES (see overrides/Android.mk) — the
# only device-tree-only way to remove an inherited package.
PRODUCT_PACKAGES += \
    liuqin_overrides

# Overlays
PRODUCT_PACKAGES += \
    LiuqinFrameworks \
    LiuqinLauncher3 \
    LiuqinSettings \
    LiuqinSettingsProvider \
    LiuqinSystemUI \
    LiuqinWifi

# Parts
PRODUCT_PACKAGES += \
    XiaomiParts

# Setup wizard (allow rotation on tablet)
PRODUCT_PRODUCT_PROPERTIES += \
    ro.setupwizard.rotation_locked=false

# Tablet
PRODUCT_COPY_FILES += \
    frameworks/native/data/etc/android.software.freeform_window_management.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.software.freeform_window_management.xml \
    $(LOCAL_PATH)/configs/tablet_core_hardware.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/tablet_core_hardware.xml

PRODUCT_PACKAGES += \
    RemoveTelephonyPackages

$(call inherit-product, $(SRC_TARGET_DIR)/product/window_extensions.mk)

# WiFi: sm8450-common already provides the qca6490 firmware symlinks
# and a generic WCNSS_qcom_cfg_qca6490.ini. liuqin doesn't need its own
# overrides at this stage.

# ---------------------------------------------------------------
# VINTF: 关闭内核版本要求检查 (Android 17 矩阵已不再支持 5.10)
#
# 本设备用预编译 waipio 内核 5.10.252-android12-...，而 Android 17 的框架兼容性
# 矩阵只保留到 FCM 7/8/202404/202504/202604，没有 5.10 对应的内核条目，于是
# target-files 的 VINTF 检查会失败:
#   ERROR: files are incompatible: Runtime info and framework compatibility matrix
#          are incompatible: No kernel entry found for kernel version 5.10 at kernel
#          FCM version 6.
#          Cannot find framework matrix at FCM version 6.
#
# PRODUCT_SHIPPING_API_LEVEL >= 29 会让 PRODUCT_OTA_ENFORCE_VINTF_KERNEL_REQUIREMENTS
# 默认为 true；置 false 后 build/make/core/Makefile 不再给 check_vintf 传 --kernel，
# 内核要求检查随之跳过（AOSP 的 goldfish 也是这样关闭的）。HAL manifest 与矩阵的
# 一致性检查仍然保留，不受影响。
PRODUCT_OTA_ENFORCE_VINTF_KERNEL_REQUIREMENTS := false

# ---------------------------------------------------------------
# SELinux Treble labeling: known-violation tracking list
#
# Android 17 enforces the Treble labeling tests (PLATFORM_SEPOLICY_VERSION >= 202604).
# The pinned Android 16 QTI vendor policy labels the platform app
# com.qualcomm.qti.poweroffalarm with the vendor domain vendor_poweroffalarm_app, which the
# new rule rejects. The domain is kept (it needs hal_alarm_qti / vendor_persist_alarm_file),
# and the violation is recorded in the tracking list instead of relabeling the app.
# See system/sepolicy/tests/treble_labeling_tests.md.
PRODUCT_SELINUX_TREBLE_LABELING_TRACKING_LIST_FILE := \
    device/xiaomi/liuqin/sepolicy/tracking_list.yaml

# ---------------------------------------------------------------
# Build-time resource overlay: Miku UI maintainer information
#
# Miku UI's wiki ("如何添加您的编译者信息") documents two steps:
#   1. MIKU_MASTER := <name>            (set in miku_liuqin.mk)  -> ro.miku.maintainer
#   2. DEVICE_PACKAGE_OVERLAYS + a Settings miku_strings.xml override so the
#      maintainer entry links to the maintainer's page (miku_maintainer_uri).
DEVICE_PACKAGE_OVERLAYS += \
    device/xiaomi/liuqin/overlay-miku
