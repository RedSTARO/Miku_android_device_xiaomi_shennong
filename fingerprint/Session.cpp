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

#include "Session.h"

#include <aidl/android/hardware/biometrics/common/BnCancellationSignal.h>
#include <android-base/logging.h>
#include <errno.h>

namespace aidl::android::hardware::biometrics::fingerprint {
namespace {
void onClientDeath(void* cookie) {
    auto session = static_cast<std::weak_ptr<Session>*>(cookie)->lock();
    if (session) session->close();
}

void onClientUnlinked(void* cookie) {
    delete static_cast<std::weak_ptr<Session>*>(cookie);
}
}  // namespace

class Session::CancellationSignal : public common::BnCancellationSignal {
  public:
    CancellationSignal(std::weak_ptr<Session> session, std::shared_ptr<Operation> operation)
        : mSession(std::move(session)), mOperation(std::move(operation)) {}

    ndk::ScopedAStatus cancel() override {
        // Set this before queuing cancellation so a start that has not reached
        // the HAL yet can be canceled without ever enabling the sensor.
        if (!mOperation->cancelRequested.exchange(true)) {
            if (auto session = mSession.lock()) {
                session->post([session, operation = mOperation] {
                    session->cancelOperation(operation);
                });
            }
        }
        return ndk::ScopedAStatus::ok();
    }

  private:
    const std::weak_ptr<Session> mSession;
    const std::shared_ptr<Operation> mOperation;
};

Session::Session(int sensorId, int userId, std::shared_ptr<ISessionCallback> cb,
                 FingerprintEngine* engine, WorkerThread* worker)
    : mSensorId(sensorId), mUserId(userId), mCb(std::move(cb)), mEngine(engine), mWorker(worker) {
    CHECK_GE(mSensorId, 0);
    CHECK_GE(mUserId, 0);
    CHECK(mEngine);
    CHECK(mWorker);
    CHECK(mCb);
    mDeathRecipient = AIBinder_DeathRecipient_new(onClientDeath);
    AIBinder_DeathRecipient_setOnUnlinked(mDeathRecipient, onClientUnlinked);
}

Session::~Session() {
    mLockoutTimer.stop();
    if (mDeathRecipient) AIBinder_DeathRecipient_delete(mDeathRecipient);
}

void Session::post(std::function<void()> task) {
    // HAL callbacks may be synchronous and may arrive in bursts. Fingerprint's
    // worker has no queue size cap, so neither callbacks nor pointer-up can be
    // silently dropped, and posting never waits for the worker to finish.
    auto self = ref<Session>();
    CHECK(mWorker->schedule(Callable::from([self, task = std::move(task)] { task(); })))
            << "Fingerprint worker stopped with a live session";
}

void Session::initialize() {
    post([this] {
        if (mClosing) return;
        mReady = mEngine->setActiveGroup(mUserId) == 0;
        if (!mReady) {
            LOG(ERROR) << "Failed to activate fingerprint user " << mUserId;
            return;
        }
        auto& tracker = mEngine->lockoutTracker(mUserId);
        if (tracker.getMode() == LockoutTracker::LockoutMode::kTimed) {
            startLockoutTimer(tracker.getLockoutTimeLeft());
        } else if (tracker.getMode() == LockoutTracker::LockoutMode::kNone) {
            // This user's timer may have elapsed while another user had the HAL.
            mCb->onLockoutCleared();
        }
    });
}

binder_status_t Session::linkToDeath(AIBinder* binder) {
    // The cookie outlives any in-flight death callback without keeping a closed
    // Session alive. NDK invokes onClientUnlinked even if linking fails.
    auto* cookie = new std::weak_ptr<Session>(ref<Session>());
    return AIBinder_linkToDeath(binder, mDeathRecipient, cookie);
}

bool Session::isClosed() {
    return mIsClosed;
}

ndk::ScopedAStatus Session::start(OperationKind kind, std::function<int()> action,
                                 std::shared_ptr<common::ICancellationSignal>* cancellation) {
    std::lock_guard lock(mLifecycleMutex);
    if (mClosing) return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
    bool idle = false;
    if (!mOperationReserved.compare_exchange_strong(idle, true)) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
    }
    auto operation = std::make_shared<Operation>(kind);
    if (cancellation) {
        *cancellation = SharedRefBase::make<CancellationSignal>(ref<Session>(), operation);
    }
    post([this, operation, action = std::move(action)] {
        mOperation = operation;
        if (mClosing || operation->cancelRequested) {
            finishError(Error::CANCELED);
            return;
        }
        if (!mReady || !mEngine->isAvailable()) {
            finishError(Error::HW_UNAVAILABLE);
            return;
        }
        if (operation->kind == OperationKind::Authenticate && checkSensorLockout()) return;
        operation->started = true;
        const int error = action();
        if (error) {
            LOG(ERROR) << "Fingerprint operation failed: " << error;
            finishError(error == -ENODEV ? Error::HW_UNAVAILABLE : Error::UNABLE_TO_PROCESS);
        }
    });
    return ndk::ScopedAStatus::ok();
}

bool Session::isAcquisition() const {
    return mOperation && (mOperation->kind == OperationKind::Authenticate ||
                          mOperation->kind == OperationKind::Enroll);
}

void Session::cancelOperation(const std::shared_ptr<Operation>& operation) {
    if (operation != mOperation || !isAcquisition() || operation->cancelSent) return;
    operation->cancelRequested = true;
    mEngine->onPointerUpImpl(0);
    if (!operation->started) {
        finishError(Error::CANCELED);
        return;
    }
    operation->cancelSent = true;
    // Goodix cancel(true) emits Error::CANCELED synchronously before returning
    // from its TA cancellation. notify() queues that event so it is only handled
    // after this call returns. Do not acknowledge cancellation here or permit a
    // new request before the vendor terminal event has been processed.
    const int error = mEngine->cancelImpl();
    if (error) {
        LOG(ERROR) << "Fingerprint cancel failed: " << error;
        // The previous hardware operation is not known to be stopped. Never
        // start another request on this device and misattribute its callbacks.
        mEngine->markUnavailable();
        finishError(Error::HW_UNAVAILABLE);
    }
}

void Session::finishOperation(std::function<void()> callback) {
    if (isAcquisition()) mEngine->onPointerUpImpl(0);
    mOperation.reset();
    mOperationReserved = false;
    callback();
    if (mClosing) finishClose();
}

void Session::finishError(Error error, int vendorCode) {
    finishOperation([this, error, vendorCode] { mCb->onError(error, vendorCode); });
}

ndk::ScopedAStatus Session::generateChallenge() {
    return start(OperationKind::GenerateChallenge, [this] { return mEngine->generateChallengeImpl(); });
}

ndk::ScopedAStatus Session::revokeChallenge(int64_t challenge) {
    return start(OperationKind::RevokeChallenge,
                 [this, challenge] { return mEngine->revokeChallengeImpl(challenge); });
}

ndk::ScopedAStatus Session::enroll(const keymaster::HardwareAuthToken& hat,
                                 std::shared_ptr<common::ICancellationSignal>* out) {
    return start(OperationKind::Enroll, [this, hat] { return mEngine->enrollImpl(hat); }, out);
}

ndk::ScopedAStatus Session::authenticate(int64_t operationId,
                                       std::shared_ptr<common::ICancellationSignal>* out) {
    return start(OperationKind::Authenticate,
                 [this, operationId] { return mEngine->authenticateImpl(operationId); }, out);
}

ndk::ScopedAStatus Session::detectInteraction(std::shared_ptr<common::ICancellationSignal>* out) {
    return start(OperationKind::DetectInteraction, [] { return -ENOTSUP; }, out);
}

ndk::ScopedAStatus Session::enumerateEnrollments() {
    return start(OperationKind::Enumerate, [this] { return mEngine->enumerateEnrollmentsImpl(); });
}

ndk::ScopedAStatus Session::removeEnrollments(const std::vector<int32_t>& enrollmentIds) {
    return start(OperationKind::Remove,
                 [this, enrollmentIds] { return mEngine->removeEnrollmentsImpl(enrollmentIds); });
}

ndk::ScopedAStatus Session::getAuthenticatorId() {
    return start(OperationKind::GetAuthenticatorId,
                 [this] { return mEngine->getAuthenticatorIdImpl(); });
}

ndk::ScopedAStatus Session::invalidateAuthenticatorId() {
    return start(OperationKind::InvalidateAuthenticatorId,
                 [this] { return mEngine->invalidateAuthenticatorIdImpl(); });
}

ndk::ScopedAStatus Session::resetLockout(const keymaster::HardwareAuthToken& hat) {
    return start(OperationKind::ResetLockout, [this, hat] {
        if (hat.mac.size() != sizeof(hw_auth_token_t{}.hmac)) return -EINVAL;
        stopLockoutTimer();
        mEngine->lockoutTracker(mUserId).reset();
        finishOperation([this] { mCb->onLockoutCleared(); });
        return 0;
    });
}

ndk::ScopedAStatus Session::close() {
    std::lock_guard lock(mLifecycleMutex);
    if (!mClosing.exchange(true)) {
        post([this] {
            // A terminal callback already on the worker may have closed this
            // session before this cleanup task was enqueued. A new user can
            // now own the engine, so never touch the hardware in that case.
            if (mIsClosed) return;
            stopLockoutTimer();
            mEngine->onPointerUpImpl(0);
            if (isAcquisition()) {
                cancelOperation(mOperation);
            } else if (!mOperation) {
                finishClose();
            }
            // An outstanding non-cancellable operation must also reach its
            // terminal callback before another user's session can take over.
        });
    }
    return ndk::ScopedAStatus::ok();
}

void Session::finishClose() {
    if (mIsClosed.exchange(true)) return;
    stopLockoutTimer();
    mEngine->onPointerUpImpl(0);
    if (mDeathRecipient) {
        AIBinder_DeathRecipient_delete(mDeathRecipient);
        mDeathRecipient = nullptr;
    }
    mCb->onSessionClosed();
}

ndk::ScopedAStatus Session::onPointerDown(int32_t pointerId, int32_t x, int32_t y,
                                        float minor, float major) {
    post([this, pointerId, x, y, minor, major] {
        if (!mClosing && isAcquisition() && !mOperation->cancelRequested) {
            mEngine->onPointerDownImpl(pointerId, x, y, minor, major);
        }
    });
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::onPointerUp(int32_t pointerId) {
    post([this, pointerId] {
        if (!mIsClosed) mEngine->onPointerUpImpl(pointerId);
    });
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::onUiReady() {
    post([this] {
        if (!mClosing && isAcquisition()) mEngine->onUiReadyImpl();
    });
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::authenticateWithContext(
        int64_t operationId, const common::OperationContext& /*context*/,
        std::shared_ptr<common::ICancellationSignal>* out) {
    return authenticate(operationId, out);
}

ndk::ScopedAStatus Session::enrollWithContext(const keymaster::HardwareAuthToken& hat,
                                              const common::OperationContext& /*context*/,
                                              std::shared_ptr<common::ICancellationSignal>* out) {
    return enroll(hat, out);
}

ndk::ScopedAStatus Session::detectInteractionWithContext(
        const common::OperationContext& /*context*/,
        std::shared_ptr<common::ICancellationSignal>* out) {
    return detectInteraction(out);
}

ndk::ScopedAStatus Session::onPointerDownWithContext(const PointerContext& context) {
    return onPointerDown(context.pointerId, context.x, context.y, context.minor, context.major);
}

ndk::ScopedAStatus Session::onPointerUpWithContext(const PointerContext& context) {
    return onPointerUp(context.pointerId);
}

ndk::ScopedAStatus Session::onContextChanged(const common::OperationContext& /*context*/) {
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::onPointerCancelWithContext(const PointerContext& context) {
    return onPointerUp(context.pointerId);
}

ndk::ScopedAStatus Session::setIgnoreDisplayTouches(bool /*shouldIgnore*/) {
    return ndk::ScopedAStatus::ok();
}

void Session::notify(const fingerprint_msg_t* msg) {
    // The vendor may reuse the message memory as soon as its callback returns.
    post([this, message = *msg] {
        if (!mIsClosed) handleNotify(message);
    });
}

void Session::handleNotify(const fingerprint_msg_t& msg) {
    if (!mOperation) return;
    const auto kind = mOperation->kind;
    switch (msg.type) {
        case FINGERPRINT_ERROR: {
            if (mOperation->pendingLockout != LockoutTracker::LockoutMode::kNone) {
                const auto mode = mOperation->pendingLockout;
                finishOperation([this, mode] { sendLockout(mode); });
            } else {
                const auto [error, vendorCode] = mEngine->convertError(msg.data.error);
                finishError(error, vendorCode);
            }
            break;
        }
        case FINGERPRINT_ACQUIRED: {
            if (!isAcquisition() || mOperation->cancelRequested) break;
            const auto [info, vendorCode] = mEngine->convertAcquiredInfo(msg.data.acquired.acquired_info);
            mEngine->onAcquired(static_cast<int32_t>(info), vendorCode);
            // Goodix sends vendor messages while illumination is still needed.
            if (info != AcquiredInfo::VENDOR) mCb->onAcquired(info, vendorCode);
            break;
        }
        case FINGERPRINT_TEMPLATE_ENROLLING: {
            if (kind != OperationKind::Enroll || mOperation->cancelSent) break;
            const auto& result = msg.data.enroll;
            if (result.samples_remaining == 0) {
                finishOperation([this, &result] { mCb->onEnrollmentProgress(result.fid, 0); });
            } else {
                mCb->onEnrollmentProgress(result.fid, result.samples_remaining);
            }
            break;
        }
        case FINGERPRINT_AUTHENTICATED: {
            if (kind != OperationKind::Authenticate || mOperation->cancelSent) break;
            mEngine->onPointerUpImpl(0);
            const auto& result = msg.data.authenticated;
            if (result.finger.fid != 0) {
                keymaster::HardwareAuthToken token;
                translate(result.hat, token);
                stopLockoutTimer();
                mEngine->lockoutTracker(mUserId).reset();
                finishOperation([this, &result, &token] {
                    mCb->onAuthenticationSucceeded(result.finger.fid, token);
                });
            } else {
                mCb->onAuthenticationFailed();
                mEngine->lockoutTracker(mUserId).addFailedAttempt();
                checkSensorLockout();
            }
            break;
        }
        case FINGERPRINT_TEMPLATE_REMOVED:
        case FINGERPRINT_TEMPLATE_ENUMERATING: {
            const bool removing = msg.type == FINGERPRINT_TEMPLATE_REMOVED;
            if (kind != (removing ? OperationKind::Remove : OperationKind::Enumerate)) break;
            const auto& result = removing ? msg.data.removed : msg.data.enumerated;
            std::vector<int32_t> enrollments;
            for (const auto fid : result.fids) {
                if (fid != 0) enrollments.push_back(fid);
            }
            finishOperation([this, removing, &enrollments] {
                if (removing) mCb->onEnrollmentsRemoved(enrollments);
                else mCb->onEnrollmentsEnumerated(enrollments);
            });
            break;
        }
        case FINGERPRINT_CHALLENGE_GENERATED:
            if (kind == OperationKind::GenerateChallenge) {
                finishOperation([this, &msg] { mCb->onChallengeGenerated(msg.data.extend.data); });
            }
            break;
        case FINGERPRINT_CHALLENGE_REVOKED:
            if (kind == OperationKind::RevokeChallenge) {
                finishOperation([this, &msg] { mCb->onChallengeRevoked(msg.data.extend.data); });
            }
            break;
        case FINGERPRINT_AUTHENTICATOR_ID_RETRIEVED:
            if (kind == OperationKind::GetAuthenticatorId) {
                const int64_t authenticatorId = msg.data.extend.data;
                finishOperation([this, authenticatorId] {
                    mCb->onAuthenticatorIdRetrieved(authenticatorId);
                });
            }
            break;
        case FINGERPRINT_AUTHENTICATOR_ID_INVALIDATED:
            if (kind == OperationKind::InvalidateAuthenticatorId) {
                finishOperation([this, &msg] {
                    mCb->onAuthenticatorIdInvalidated(msg.data.extend.data);
                });
            }
            break;
        default:
            LOG(WARNING) << "Ignoring fingerprint message " << msg.type;
    }
}

void Session::sendLockout(LockoutTracker::LockoutMode mode) {
    if (mode == LockoutTracker::LockoutMode::kPermanent) {
        mCb->onLockoutPermanent();
    } else {
        const auto remaining = mEngine->lockoutTracker(mUserId).getLockoutTimeLeft();
        mCb->onLockoutTimed(std::max<int64_t>(0, remaining));
    }
}

bool Session::checkSensorLockout() {
    auto& tracker = mEngine->lockoutTracker(mUserId);
    const auto mode = tracker.getMode();
    if (mode == LockoutTracker::LockoutMode::kNone) return false;
    if (mode == LockoutTracker::LockoutMode::kTimed) {
        startLockoutTimer(tracker.getLockoutTimeLeft());
    } else {
        stopLockoutTimer();
    }
    if (mOperation->started) {
        // Stop the vendor authentication before delivering the terminal lockout
        // callback, otherwise the framework could start a new hardware request.
        mOperation->pendingLockout = mode;
        cancelOperation(mOperation);
    } else {
        finishOperation([this, mode] { sendLockout(mode); });
    }
    return true;
}

void Session::startLockoutTimer(int64_t timeout) {
    stopLockoutTimer();
    const uint64_t generation = mTimerGeneration;
    const std::weak_ptr<Session> session = ref<Session>();
    auto* worker = mWorker;
    mLockoutTimer.start(std::chrono::milliseconds(std::max<int64_t>(0, timeout)),
                       [session, worker, generation] {
        // Do not retain a Session on the timer thread: its destructor joins
        // this timer. The worker acquires ownership only when running the task.
        CHECK(worker->schedule(Callable::from([session, generation] {
            if (auto self = session.lock()) self->lockoutTimerExpired(generation);
        })));
    });
}

void Session::stopLockoutTimer() {
    ++mTimerGeneration;
    mLockoutTimer.stop();
}

void Session::lockoutTimerExpired(uint64_t generation) {
    if (mClosing || generation != mTimerGeneration) return;
    auto& tracker = mEngine->lockoutTracker(mUserId);
    if (tracker.getMode() == LockoutTracker::LockoutMode::kPermanent) return;
    const auto remaining = tracker.getLockoutTimeLeft();
    if (remaining > 0) {
        startLockoutTimer(remaining);
        return;
    }
    stopLockoutTimer();
    tracker.reset(true);  // A timeout preserves failures; a successful match does not.
    mCb->onLockoutCleared();
}

}  // namespace aidl::android::hardware::biometrics::fingerprint
