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

#include <stdint.h>

#define LOCKOUT_TIMED_THRESHOLD 5
#define LOCKOUT_TIMED_DURATION 10000
#define LOCKOUT_PERMANENT_THRESHOLD 20

namespace aidl::android::hardware::biometrics::fingerprint {

class LockoutTracker {
  public:
    LockoutTracker() = default;

    enum class LockoutMode : int8_t { kNone = 0, kTimed, kPermanent };

    void reset(bool dueToTimeout = false);
    LockoutMode getMode();
    void addFailedAttempt();
    int64_t getLockoutTimeLeft();

  private:
    int32_t mFailedCount = 0;
    int64_t mLockoutTimedStart = 0;
    LockoutMode mCurrentMode = LockoutMode::kNone;
};

}  // namespace aidl::android::hardware::biometrics::fingerprint
