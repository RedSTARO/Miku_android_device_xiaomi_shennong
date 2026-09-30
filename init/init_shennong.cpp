/*
 * Copyright (c) 2025, The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <cstdlib>
#include <fstream>
#include <string.h>
#include <unistd.h>
#include <vector>

#include <android-base/properties.h>
#define _REALLY_INCLUDE_SYS__SYSTEM_PROPERTIES_H_
#include <sys/_system_properties.h>
#include <sys/sysinfo.h>

#include "property_service.h"
#include "vendor_init.h"

using android::base::GetProperty;
using std::string;

// List of partitions to override props
static const string source_partitions[] = {
    "",
    "bootimage.",
    "odm.",
    "product.",
    "system.",
    "system_dlkm.",
    "system_ext.",
    "vendor.",
    "vendor_dlkm."
};

void property_override(char const prop[], char const value[]) {
    auto pi = (prop_info*) __system_property_find(prop);

    if (pi != nullptr)
        __system_property_update(pi, value, strlen(value));
    else
        __system_property_add(prop, strlen(prop), value, strlen(value));
}

void set_build_prop(const string &prop, const string &value) {
    property_override(prop.c_str(), value.c_str());
}

void set_ro_build_prop(const string &prop, const string &value) {
    string prop_name;
    for (const string &source : source_partitions) {
        prop_name = "ro.product." + source + prop;
        property_override(prop_name.c_str(), value.c_str());
    }
}

void vendor_load_properties() {
    // Detect variant and override properties
    string sku = GetProperty("ro.boot.hardware.sku", "");
    string hwversion = GetProperty("ro.boot.hwversion", "");

    // Xiaomi 14 Pro Ti, see /odm/etc/shennong_${ro.boot.hwversion}.prop on stock
    bool ti_satellite = hwversion == "1.9.8" || hwversion == "1.9.9";
    bool ti = ti_satellite || hwversion == "1.0.6" || hwversion == "1.1.6" ||
              hwversion == "1.1.7" || hwversion == "1.2.6" || hwversion == "1.9.6";
    string model = ti_satellite ? "2311BPN23C" : "23116PN5BC";

    if (ti)
        set_build_prop("ro.product.marketname", "Xiaomi 14 Pro Ti");
    if (ti_satellite)
        set_build_prop("ro.product.model_for_attestation", model);

    // Override device specific props
    set_ro_build_prop("device", sku);

    // Set device specific infomation
    set_ro_build_prop("model", model);
    set_ro_build_prop("name", "shennong");

    // Override hardware revision
    set_build_prop("ro.boot.hardware.revision", sku);
}
