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

#pragma once

#include <aidl/android/hardware/biometrics/fingerprint/ISessionCallback.h>
#include <aidl/android/hardware/biometrics/fingerprint/SensorLocation.h>
#include <android-base/unique_fd.h>

#include <unordered_map>
#include <vector>

#include "Legacy2Aidl.h"
#include "LockoutTracker.h"
#include "fingerprint-xiaomi.h"

namespace aidl::android::hardware::biometrics::fingerprint {

// HAL access and per-user lockout state are confined to Fingerprint's worker.
class FingerprintEngine {
  public:
    FingerprintEngine();
    // A non-owning HAL injection point for tests; it never opens display hardware.
    explicit FingerprintEngine(fingerprint_device_t* device);
    ~FingerprintEngine();

    bool isAvailable() const { return mDevice != nullptr && !mFailed; }
    void markUnavailable() { mFailed = true; }
    int setActiveGroup(int userId);
    int generateChallengeImpl();
    int revokeChallengeImpl(int64_t challenge);
    int enrollImpl(const keymaster::HardwareAuthToken& hat);
    int authenticateImpl(int64_t operationId);
    int cancelImpl();
    int enumerateEnrollmentsImpl();
    int removeEnrollmentsImpl(const std::vector<int32_t>& enrollmentIds);
    int getAuthenticatorIdImpl();
    int invalidateAuthenticatorIdImpl();

    ndk::ScopedAStatus onPointerDownImpl(int32_t pointerId, int32_t x, int32_t y,
                                       float minor, float major);
    ndk::ScopedAStatus onPointerUpImpl(int32_t pointerId);
    ndk::ScopedAStatus onUiReadyImpl();
    SensorLocation getSensorLocation();

    LockoutTracker& lockoutTracker(int userId) { return mLockoutTrackers[userId]; }
    void onAcquired(int32_t result, int32_t vendorCode);
    std::pair<AcquiredInfo, int32_t> convertAcquiredInfo(int32_t code);
    std::pair<Error, int32_t> convertError(int32_t code);

  private:
    static constexpr int32_t FINGERPRINT_ACQUIRED_VENDOR_BASE = 1000;
    static constexpr int32_t FINGERPRINT_ERROR_VENDOR_BASE = 1000;
    fingerprint_device_t* openFingerprintHal();
    void setFingerStatus(bool pressed);

    fingerprint_device_t* mDevice = nullptr;
    bool mOwnsDevice = false;
    bool mFailed = false;
    ::android::base::unique_fd mDisplayFd;
    std::unordered_map<int, LockoutTracker> mLockoutTrackers;
};

}  // namespace aidl::android::hardware::biometrics::fingerprint
