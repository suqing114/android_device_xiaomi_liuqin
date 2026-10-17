# SPDX-FileCopyrightText: 2023 The LineageOS Project
# SPDX-License-Identifier: Apache-2.0

TARGET_MIKU_BUILD_VARIANT := UNOFFICIAL
MIKU_GAPPS := true

$(call inherit-product, device/xiaomi/liuqin/device.mk)
$(call inherit-product, vendor/miku/build/product/miku_product_tab.mk)

PRODUCT_NAME := miku_liuqin
PRODUCT_DEVICE := liuqin
PRODUCT_BRAND := Xiaomi
PRODUCT_MODEL := Xiaomi Pad 6 Pro
PRODUCT_MANUFACTURER := Xiaomi
