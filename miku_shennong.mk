#
# Copyright (C) 2023 The Android Open Source Project
# Copyright (C) 2026 Miku UI
#
# SPDX-License-Identifier: Apache-2.0
#

# Inherit from products. Most specific first.
$(call inherit-product, $(SRC_TARGET_DIR)/product/core_64_bit_only.mk)
$(call inherit-product, $(SRC_TARGET_DIR)/product/full_base_telephony.mk)

# Inherit from shennong device.
$(call inherit-product, device/xiaomi/shennong/device.mk)

# Maintainer (read by vendor/miku/config/versioning.mk, so set it before inheriting)
MIKU_MASTER ?= RedSTARO

# Inherit Miku UI configurations
$(call inherit-product, vendor/miku/build/product/miku_product_phone.mk)

## Device identifier
PRODUCT_BRAND := Xiaomi
PRODUCT_DEVICE := shennong
PRODUCT_MANUFACTURER := Xiaomi
PRODUCT_NAME := miku_shennong
PRODUCT_MODEL := 23116PN5BC

PRODUCT_BUILD_PROP_OVERRIDES += \
    BuildDesc=$(call normalize-path-list, "shennong-user 15 AQ3A.240627.003 OS2.0.217.0.VNBCNXM release-keys")

# Stock fingerprint. BUILD_FINGERPRINT gives every partition its ro.<part>.build.fingerprint
# (and the AVB descriptors), but Miku UI's build/soong, unlike LineageOS', never emits a plain
# ro.build.fingerprint (scripts/gen_build_prop.py). Without it init derives one at boot from
# ro.product.name (= miku_shennong) and releasetools writes that same derived value into the
# OTA metadata, so set the property here as well. Both are kept in one variable to stay in sync.
SHENNONG_BUILD_FINGERPRINT := Xiaomi/shennong/shennong:15/AQ3A.240627.003/OS2.0.217.0.VNBCNXM:user/release-keys

BUILD_FINGERPRINT := $(SHENNONG_BUILD_FINGERPRINT)

PRODUCT_SYSTEM_PROPERTIES += \
    ro.build.fingerprint=$(SHENNONG_BUILD_FINGERPRINT)

# GMS
PRODUCT_GMS_CLIENTID_BASE := android-xiaomi
