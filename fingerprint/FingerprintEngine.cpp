/*
 * Copyright (C) 2022 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "FingerprintEngine.h"
#include "Fingerprint.h"

#include <android-base/logging.h>
#include <android-base/parseint.h>
#include <display/drm/mi_disp.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>

#include <string>

#include "util/Util.h"

using ::android::base::ParseInt;

namespace aidl::android::hardware::biometrics::fingerprint {

FingerprintEngine::FingerprintEngine() : mDevice(openFingerprintHal()), mOwnsDevice(true) {
    if (!mDevice) {
        LOG(ERROR) << "Can't open fingerprint HAL module";
        return;
    }
    mDisplayFd.reset(open("/dev/mi_display/disp_feature", O_RDWR | O_CLOEXEC));
    if (mDisplayFd.get() < 0) PLOG(ERROR) << "Can't open fingerprint display device";
}

FingerprintEngine::FingerprintEngine(fingerprint_device_t* device) : mDevice(device) {}

FingerprintEngine::~FingerprintEngine() {
    if (mOwnsDevice && mDevice && mDevice->common.close) {
        mDevice->common.close(&mDevice->common);
    }
}

int FingerprintEngine::setActiveGroup(int userId) {
    if (!isAvailable()) return -ENODEV;
    const auto path = "/data/vendor_de/" + std::to_string(userId) + "/fpdata/";
    return mDevice->setActiveGroup(mDevice, userId, path.c_str());
}

fingerprint_device_t* FingerprintEngine::openFingerprintHal() {
    const hw_module_t* module = nullptr;
    if (hw_get_module(FINGERPRINT_HARDWARE_MODULE_ID, &module) != 0 || !module ||
        module->module_api_version != FINGERPRINT_MODULE_API_VERSION_2_1 ||
        !module->methods || !module->methods->open) {
        LOG(ERROR) << "No compatible fingerprint HW module";
        return nullptr;
    }

    hw_device_t* device = nullptr;
    if (module->methods->open(module, nullptr, &device) != 0 || !device) return nullptr;

    auto* fpDevice = reinterpret_cast<fingerprint_device_t*>(device);
    if (!fpDevice->set_notify || !fpDevice->setActiveGroup || !fpDevice->generateChallenge ||
        !fpDevice->revokeChallenge || !fpDevice->enroll || !fpDevice->authenticate ||
        !fpDevice->cancel || !fpDevice->enumerate || !fpDevice->remove ||
        !fpDevice->getAuthenticatorId || !fpDevice->invalidateAuthenticatorId ||
        !fpDevice->goodixExtCmd || fpDevice->set_notify(fpDevice, Fingerprint::notify) != 0) {
        LOG(ERROR) << "Invalid fingerprint HAL methods or callback registration failure";
        if (device->close) device->close(device);
        return nullptr;
    }
    return fpDevice;
}

void FingerprintEngine::onAcquired(int32_t result, int32_t vendorCode) {
    if (result != FINGERPRINT_ACQUIRED_VENDOR || vendorCode == 44) {
        setFingerStatus(false);
    }
}

void FingerprintEngine::setFingerStatus(bool pressed) {
    // Even after a HAL failure, turn off LHBM; do not leave the illumination on.
    if (isAvailable()) {
        mDevice->goodixExtCmd(mDevice, COMMAND_FOD_PRESS_STATUS,
                             pressed ? PARAM_FOD_PRESSED : PARAM_FOD_RELEASED);
        mDevice->goodixExtCmd(mDevice, COMMAND_NIT, pressed ? PARAM_NIT_FOD : PARAM_NIT_NONE);
    }
    if (mDisplayFd.get() >= 0) {
        disp_local_hbm_req req = {
                .base = {.flag = 0, .disp_id = MI_DISP_PRIMARY},
                .local_hbm_value = pressed ? LHBM_TARGET_BRIGHTNESS_WHITE_1000NIT
                                          : LHBM_TARGET_BRIGHTNESS_OFF_FINGER_UP,
        };
        if (ioctl(mDisplayFd.get(), MI_DISP_IOCTL_SET_LOCAL_HBM, &req) < 0) {
            PLOG(ERROR) << "Failed to set fingerprint illumination";
        }
    }
}

int FingerprintEngine::generateChallengeImpl() {
    return isAvailable() ? mDevice->generateChallenge(mDevice) : -ENODEV;
}

int FingerprintEngine::revokeChallengeImpl(int64_t challenge) {
    return isAvailable() ? mDevice->revokeChallenge(mDevice, challenge) : -ENODEV;
}

int FingerprintEngine::enrollImpl(const keymaster::HardwareAuthToken& hat) {
    if (!isAvailable()) return -ENODEV;
    hw_auth_token_t authToken{};
    if (hat.mac.size() != sizeof(authToken.hmac)) return -EINVAL;
    translate(hat, authToken);
    return mDevice->enroll(mDevice, &authToken);
}

int FingerprintEngine::authenticateImpl(int64_t operationId) {
    return isAvailable() ? mDevice->authenticate(mDevice, operationId) : -ENODEV;
}

int FingerprintEngine::cancelImpl() {
    return isAvailable() ? mDevice->cancel(mDevice) : -ENODEV;
}

int FingerprintEngine::enumerateEnrollmentsImpl() {
    return isAvailable() ? mDevice->enumerate(mDevice) : -ENODEV;
}

int FingerprintEngine::removeEnrollmentsImpl(const std::vector<int32_t>& enrollmentIds) {
    return isAvailable() ? mDevice->remove(mDevice, enrollmentIds.data(), enrollmentIds.size())
                         : -ENODEV;
}

int FingerprintEngine::getAuthenticatorIdImpl() {
    return isAvailable() ? mDevice->getAuthenticatorId(mDevice) : -ENODEV;
}

int FingerprintEngine::invalidateAuthenticatorIdImpl() {
    return isAvailable() ? mDevice->invalidateAuthenticatorId(mDevice) : -ENODEV;
}

ndk::ScopedAStatus FingerprintEngine::onPointerDownImpl(int32_t /*pointerId*/, int32_t x,
                                                      int32_t y, float /*minor*/,
                                                      float /*major*/) {
    if (isAvailable()) {
        mDevice->goodixExtCmd(mDevice, COMMAND_FOD_PRESS_X, x);
        mDevice->goodixExtCmd(mDevice, COMMAND_FOD_PRESS_Y, y);
        setFingerStatus(true);
    }
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus FingerprintEngine::onPointerUpImpl(int32_t /*pointerId*/) {
    if (isAvailable()) {
        mDevice->goodixExtCmd(mDevice, COMMAND_FOD_PRESS_X, 0);
        mDevice->goodixExtCmd(mDevice, COMMAND_FOD_PRESS_Y, 0);
    }
    setFingerStatus(false);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus FingerprintEngine::onUiReadyImpl() {
    return ndk::ScopedAStatus::ok();
}

SensorLocation FingerprintEngine::getSensorLocation() {
    SensorLocation location;

    auto loc = Fingerprint::cfg().get<std::string>("sensor_location");
    auto isValidStr = false;
    auto dim = Util::split(loc, ":");

    if (dim.size() < 3 or dim.size() > 4) {
        if (!loc.empty()) LOG(WARNING) << "Invalid sensor location input (x:y:radius):" + loc;
        return location;
    } else {
        int32_t x, y, r;
        std::string d = "";
        if (dim.size() >= 3) {
            isValidStr = ParseInt(dim[0], &x) && ParseInt(dim[1], &y) && ParseInt(dim[2], &r);
        }
        if (dim.size() >= 4) {
            d = dim[3];
        }
        if (isValidStr)
            location = {.sensorLocationX = x, .sensorLocationY = y, .sensorRadius = r, .display = d};

        return location;
    }
}

std::pair<AcquiredInfo, int32_t> FingerprintEngine::convertAcquiredInfo(int32_t code) {
    std::pair<AcquiredInfo, int32_t> res;
    if (code > FINGERPRINT_ACQUIRED_VENDOR_BASE) {
        res.first = AcquiredInfo::VENDOR;
        res.second = code - FINGERPRINT_ACQUIRED_VENDOR_BASE;
    } else {
        res.first = (AcquiredInfo)code;
        res.second = 0;
    }
    return res;
}

std::pair<Error, int32_t> FingerprintEngine::convertError(int32_t code) {
    std::pair<Error, int32_t> res;
    if (code > FINGERPRINT_ERROR_VENDOR_BASE) {
        res.first = Error::VENDOR;
        res.second = code - FINGERPRINT_ERROR_VENDOR_BASE;
    } else {
        res.first = (Error)code;
        res.second = 0;
    }
    return res;
}

}  // namespace aidl::android::hardware::biometrics::fingerprint
