/*
 * Copyright (C) 2020 The Android Open Source Project
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

#include <aidl/android/hardware/biometrics/fingerprint/BnSession.h>
#include <aidl/android/hardware/biometrics/fingerprint/ISessionCallback.h>
#include <atomic>
#include <functional>
#include <mutex>
#include "SessionTimer.h"

#include "FingerprintEngine.h"
#include "thread/WorkerThread.h"

#include "Legacy2Aidl.h"

namespace aidl::android::hardware::biometrics::fingerprint {

namespace common = aidl::android::hardware::biometrics::common;
namespace keymaster = aidl::android::hardware::keymaster;

class Session : public BnSession {
  public:
    Session(int sensorId, int userId, std::shared_ptr<ISessionCallback> cb,
            FingerprintEngine* engine, WorkerThread* worker);
    ~Session() override;

    void initialize();

    ndk::ScopedAStatus generateChallenge() override;

    ndk::ScopedAStatus revokeChallenge(int64_t challenge) override;

    ndk::ScopedAStatus enroll(const keymaster::HardwareAuthToken& hat,
                              std::shared_ptr<common::ICancellationSignal>* out) override;

    ndk::ScopedAStatus authenticate(int64_t operationId,
                                    std::shared_ptr<common::ICancellationSignal>* out) override;

    ndk::ScopedAStatus detectInteraction(
            std::shared_ptr<common::ICancellationSignal>* out) override;

    ndk::ScopedAStatus enumerateEnrollments() override;

    ndk::ScopedAStatus removeEnrollments(const std::vector<int32_t>& enrollmentIds) override;

    ndk::ScopedAStatus getAuthenticatorId() override;

    ndk::ScopedAStatus invalidateAuthenticatorId() override;

    ndk::ScopedAStatus resetLockout(const keymaster::HardwareAuthToken& hat) override;

    ndk::ScopedAStatus close() override;

    ndk::ScopedAStatus onPointerDown(int32_t pointerId, int32_t x, int32_t y, float minor,
                                     float major) override;

    ndk::ScopedAStatus onPointerUp(int32_t pointerId) override;

    ndk::ScopedAStatus onUiReady() override;

    ndk::ScopedAStatus authenticateWithContext(
            int64_t operationId, const common::OperationContext& context,
            std::shared_ptr<common::ICancellationSignal>* out) override;

    ndk::ScopedAStatus enrollWithContext(
            const keymaster::HardwareAuthToken& hat, const common::OperationContext& context,
            std::shared_ptr<common::ICancellationSignal>* out) override;

    ndk::ScopedAStatus detectInteractionWithContext(
            const common::OperationContext& context,
            std::shared_ptr<common::ICancellationSignal>* out) override;

    ndk::ScopedAStatus onPointerDownWithContext(const PointerContext& context) override;

    ndk::ScopedAStatus onPointerUpWithContext(const PointerContext& context) override;

    ndk::ScopedAStatus onContextChanged(const common::OperationContext& context) override;

    ndk::ScopedAStatus onPointerCancelWithContext(const PointerContext& context) override;

    ndk::ScopedAStatus setIgnoreDisplayTouches(bool shouldIgnore) override;

    binder_status_t linkToDeath(AIBinder* binder);

    bool isClosed();

    void notify(const fingerprint_msg_t* msg);
  private:
    enum class OperationKind {
        GenerateChallenge, RevokeChallenge, Enroll, Authenticate, DetectInteraction,
        Enumerate, Remove, GetAuthenticatorId, InvalidateAuthenticatorId, ResetLockout,
    };
    struct Operation {
        explicit Operation(OperationKind kind) : kind(kind) {}
        const OperationKind kind;
        std::atomic<bool> cancelRequested = false;
        bool started = false;
        bool cancelSent = false;
        LockoutTracker::LockoutMode pendingLockout = LockoutTracker::LockoutMode::kNone;
    };
    class CancellationSignal;

    void post(std::function<void()> task);
    ndk::ScopedAStatus start(OperationKind kind, std::function<int()> action,
                            std::shared_ptr<common::ICancellationSignal>* cancellation = nullptr);
    void cancelOperation(const std::shared_ptr<Operation>& operation);
    void finishOperation(std::function<void()> callback);
    void finishError(Error error, int vendorCode = 0);
    void finishClose();
    void handleNotify(const fingerprint_msg_t& msg);
    bool isAcquisition() const;
    bool checkSensorLockout();
    void sendLockout(LockoutTracker::LockoutMode mode);
    void startLockoutTimer(int64_t timeout);
    void stopLockoutTimer();
    void lockoutTimerExpired(uint64_t generation);

    const int32_t mSensorId;
    const int32_t mUserId;
    const std::shared_ptr<ISessionCallback> mCb;
    FingerprintEngine* const mEngine;
    WorkerThread* const mWorker;

    // Only these flags are shared with binder/death-recipient threads. All
    // remaining session state, HAL calls and callbacks run on the single worker.
    std::atomic<bool> mClosing = false;
    std::atomic<bool> mIsClosed = false;
    std::atomic<bool> mOperationReserved = false;
    // Serialize operation admission/queue insertion with close(), including
    // calls arriving concurrently from a binder death recipient.
    std::mutex mLifecycleMutex;
    bool mReady = false;
    std::shared_ptr<Operation> mOperation;
    SessionTimer mLockoutTimer;
    uint64_t mTimerGeneration = 0;
    AIBinder_DeathRecipient* mDeathRecipient = nullptr;
};

}  // namespace aidl::android::hardware::biometrics::fingerprint
