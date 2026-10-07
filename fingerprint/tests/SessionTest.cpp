/*
 * Copyright (C) 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include "Fingerprint.h"
#include "Session.h"
#include <aidl/android/hardware/biometrics/fingerprint/BnSessionCallback.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <future>
#include <limits>
#include <mutex>
#include <thread>

using namespace aidl::android::hardware::biometrics;
using namespace aidl::android::hardware::biometrics::fingerprint;
using namespace std::chrono_literals;

static_assert(offsetof(fingerprint_msg_t, data) == 8);
static_assert(sizeof(fingerprint_template_list_t) == 5 * sizeof(uint32_t));
static_assert(offsetof(fingerprint_authenticated_t, hat) == 4);

#define REQUIRE(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); std::abort(); \
} } while (false)

// Engine initialization links this symbol even when a test injects a HAL. Test
// callbacks deliberately go to their owning Session instead of a global HAL.
void Fingerprint::notify(const fingerprint_msg_t*) {}
#ifdef SHENNONG_HOST_TEST
extern "C" int hw_get_module(const char*, const hw_module_t**) { return -ENOENT; }
#endif

struct Callback : BnSessionCallback {
    std::atomic<int> errors = 0, canceled = 0, unavailable = 0, success = 0;
    std::atomic<int> failed = 0, timed = 0, permanent = 0, cleared = 0, closed = 0;
    std::atomic<int> enumerations = 0, removals = 0;
    std::atomic<int64_t> authenticatorId = 0;
    std::vector<int32_t> ids;
    std::function<void()> errorHook;
    ndk::ScopedAStatus onChallengeGenerated(int64_t) override { return ndk::ScopedAStatus::ok(); }
    ndk::ScopedAStatus onChallengeRevoked(int64_t) override { return ndk::ScopedAStatus::ok(); }
    ndk::ScopedAStatus onAcquired(AcquiredInfo, int32_t) override { return ndk::ScopedAStatus::ok(); }
    ndk::ScopedAStatus onError(Error error, int32_t) override {
        if (errorHook) errorHook();
        ++errors;
        if (error == Error::CANCELED) ++canceled;
        if (error == Error::HW_UNAVAILABLE) ++unavailable;
        return ndk::ScopedAStatus::ok();
    }
    ndk::ScopedAStatus onEnrollmentProgress(int32_t, int32_t) override { return ndk::ScopedAStatus::ok(); }
    ndk::ScopedAStatus onAuthenticationSucceeded(int32_t, const keymaster::HardwareAuthToken&) override {
        ++success; return ndk::ScopedAStatus::ok();
    }
    ndk::ScopedAStatus onAuthenticationFailed() override { ++failed; return ndk::ScopedAStatus::ok(); }
    ndk::ScopedAStatus onLockoutTimed(int64_t) override { ++timed; return ndk::ScopedAStatus::ok(); }
    ndk::ScopedAStatus onLockoutPermanent() override { ++permanent; return ndk::ScopedAStatus::ok(); }
    ndk::ScopedAStatus onLockoutCleared() override { ++cleared; return ndk::ScopedAStatus::ok(); }
    ndk::ScopedAStatus onInteractionDetected() override { return ndk::ScopedAStatus::ok(); }
    ndk::ScopedAStatus onEnrollmentsEnumerated(const std::vector<int32_t>& value) override {
        ids = value; ++enumerations; return ndk::ScopedAStatus::ok();
    }
    ndk::ScopedAStatus onEnrollmentsRemoved(const std::vector<int32_t>& value) override {
        ids = value; ++removals; return ndk::ScopedAStatus::ok();
    }
    ndk::ScopedAStatus onAuthenticatorIdRetrieved(int64_t id) override {
        authenticatorId = id; return ndk::ScopedAStatus::ok();
    }
    ndk::ScopedAStatus onAuthenticatorIdInvalidated(int64_t id) override {
        authenticatorId = id; return ndk::ScopedAStatus::ok();
    }
    ndk::ScopedAStatus onSessionClosed() override { ++closed; return ndk::ScopedAStatus::ok(); }
};

struct FakeDevice : fingerprint_device_t {
    std::weak_ptr<Session> session;
    std::atomic<int> authentications = 0, enrollments = 0, cancellations = 0, releases = 0;
    std::atomic<bool> inCancel = false, deferCancel = false;
    std::atomic<int> cancelError = 0;
    hw_auth_token_t enrolledToken{};
    int activeUser = -1;
    std::shared_future<void> cancelGate;
    std::promise<void>* cancelEntered = nullptr;

    FakeDevice() : fingerprint_device_t{} {
        setActiveGroup = [](fingerprint_device_t* device, uint32_t user, const char*) -> uint32_t {
            static_cast<FakeDevice*>(device)->activeUser = user; return 0;
        };
        authenticate = [](fingerprint_device_t* device, uint64_t) -> uint32_t {
            ++static_cast<FakeDevice*>(device)->authentications; return 0;
        };
        enroll = [](fingerprint_device_t* device, const hw_auth_token_t* token) -> uint32_t {
            auto* fake = static_cast<FakeDevice*>(device);
            fake->enrolledToken = *token; ++fake->enrollments; return 0;
        };
        cancel = [](fingerprint_device_t* device) -> uint32_t {
            auto* fake = static_cast<FakeDevice*>(device);
            ++fake->cancellations;
            fake->inCancel = true;
            if (!fake->deferCancel) fake->error(FINGERPRINT_ERROR_CANCELED);
            if (fake->cancelEntered) fake->cancelEntered->set_value();
            if (fake->cancelGate.valid()) fake->cancelGate.wait();
            fake->inCancel = false;
            return fake->cancelError;
        };
        goodixExtCmd = [](fingerprint_device_t* device, int32_t command, int32_t value) -> uint64_t {
            if (command == COMMAND_FOD_PRESS_STATUS && value == PARAM_FOD_RELEASED) {
                ++static_cast<FakeDevice*>(device)->releases;
            }
            return 0;
        };
        getAuthenticatorId = [](fingerprint_device_t* device) -> uint64_t {
            fingerprint_msg_t msg{};
            msg.type = FINGERPRINT_AUTHENTICATOR_ID_RETRIEVED;
            msg.data.extend.data = INT64_C(0x1234567887654321);
            static_cast<FakeDevice*>(device)->emit(msg);
            return 0;
        };
        invalidateAuthenticatorId = getAuthenticatorId;
        enumerate = [](fingerprint_device_t* device) -> uint32_t {
            fingerprint_msg_t msg{};
            msg.type = FINGERPRINT_TEMPLATE_ENUMERATING;
            msg.data.enumerated.fids[0] = 7;
            msg.data.enumerated.fids[1] = 19;
            msg.data.enumerated.fids[2] = 23;
            static_cast<FakeDevice*>(device)->emit(msg);
            return 0;
        };
        remove = [](fingerprint_device_t* device, const int32_t* ids, uint32_t count) -> uint64_t {
            fingerprint_msg_t msg{};
            msg.type = FINGERPRINT_TEMPLATE_REMOVED;
            for (uint32_t i = 0; i < count && i < FINGERPRINT_MAX_TEMPLATES; ++i) {
                msg.data.removed.fids[i] = ids[i];
            }
            static_cast<FakeDevice*>(device)->emit(msg);
            return 0;
        };
    }
    void emit(const fingerprint_msg_t& msg) {
        if (auto target = session.lock()) target->notify(&msg);
    }
    void error(fingerprint_error_t code) {
        fingerprint_msg_t msg{}; msg.type = FINGERPRINT_ERROR; msg.data.error = code; emit(msg);
    }
    void match(bool success) {
        fingerprint_msg_t msg{}; msg.type = FINGERPRINT_AUTHENTICATED;
        msg.data.authenticated.finger.fid = success ? 7 : 0; emit(msg);
    }
};

// Runs the real worker but can pause one producer immediately before it enqueues
// a task, to reproduce binder/death-recipient/worker admission races.
class ControlledWorker : public WorkerThread {
  public:
    ControlledWorker() : WorkerThread(std::numeric_limits<size_t>::max()) {}
    std::atomic<bool> intercept = false;
    std::promise<void> intercepted;
    std::shared_future<void> gate;
    bool schedule(std::unique_ptr<Callable> task) override {
        if (intercept.exchange(false)) {
            intercepted.set_value();
            gate.wait();
        }
        return WorkerThread::schedule(std::move(task));
    }
};

void drain(WorkerThread& worker) {
    // The first queued task may itself enqueue a synchronous vendor callback.
    for (int i = 0; i < 3; ++i) {
        std::promise<void> done; auto ready = done.get_future();
        REQUIRE(worker.schedule(Callable::from([&done] { done.set_value(); })));
        REQUIRE(ready.wait_for(2s) == std::future_status::ready);
    }
}

struct Fixture {
    FakeDevice device;
    FingerprintEngine engine{&device};
    ControlledWorker worker;
    std::shared_ptr<Callback> callback = ndk::SharedRefBase::make<Callback>();
    std::shared_ptr<Session> session;
    Fixture() { open(0); }
    void open(int user) {
        session = ndk::SharedRefBase::make<Session>(5, user, callback, &engine, &worker);
        device.session = session;
        session->initialize(); drain(worker);
    }
    ~Fixture() {
        device.deferCancel = false;
        if (!session->isClosed()) { session->close(); drain(worker); }
        REQUIRE(session->isClosed());
        session.reset();
    }
    std::shared_ptr<common::ICancellationSignal> authenticate() {
        std::shared_ptr<common::ICancellationSignal> signal;
        REQUIRE(session->authenticate(42, &signal).isOk());
        drain(worker); return signal;
    }
};

void initializationAndUnavailable() {
    LockoutTracker tracker;
    REQUIRE(tracker.getMode() == LockoutTracker::LockoutMode::kNone);
    REQUIRE(tracker.getLockoutTimeLeft() == 0);
    FingerprintEngine engine(nullptr);
    REQUIRE(!engine.isAvailable());
    REQUIRE(engine.setActiveGroup(0) == -ENODEV);
    REQUIRE(engine.authenticateImpl(0) == -ENODEV);
    REQUIRE(engine.cancelImpl() == -ENODEV);
    REQUIRE(engine.onPointerUpImpl(0).isOk());
    ControlledWorker worker;
    auto cb = ndk::SharedRefBase::make<Callback>();
    auto session = ndk::SharedRefBase::make<Session>(5, 0, cb, &engine, &worker);
    REQUIRE(!session->isClosed());
    session->initialize();
    std::shared_ptr<common::ICancellationSignal> signal;
    REQUIRE(session->authenticate(0, &signal).isOk());
    drain(worker); REQUIRE(cb->unavailable == 1);
    session->close(); drain(worker); REQUIRE(cb->closed == 1);
}

void cancelBeforeStart() {
    Fixture f;
    std::promise<void> release, entered;
    auto gate = release.get_future().share();
    REQUIRE(f.worker.schedule(Callable::from([&entered, gate] { entered.set_value(); gate.wait(); })));
    entered.get_future().wait();
    std::shared_ptr<common::ICancellationSignal> signal;
    REQUIRE(f.session->authenticate(42, &signal).isOk());
    signal->cancel(); signal->cancel();
    release.set_value(); drain(f.worker);
    REQUIRE(f.device.authentications == 0);
    REQUIRE(f.device.cancellations == 0);
    REQUIRE(f.callback->canceled == 1);
}

void cancelWhileRunningAndStaleSignal() {
    Fixture f;
    auto old = f.authenticate();
    f.callback->errorHook = [&f] { REQUIRE(!f.device.inCancel); };
    old->cancel(); old->cancel(); drain(f.worker);
    REQUIRE(f.device.cancellations == 1);
    REQUIRE(f.callback->canceled == 1);
    auto next = f.authenticate();
    old->cancel(); drain(f.worker);
    REQUIRE(f.device.cancellations == 1);
    f.device.match(true); drain(f.worker);
    next->cancel(); drain(f.worker);
    REQUIRE(f.callback->success == 1);
    REQUIRE(f.device.cancellations == 1);
}

void synchronousCancelDoesNotFinishBeforeVendorReturns() {
    Fixture f;
    auto signal = f.authenticate();
    std::promise<void> entered, release;
    f.device.cancelEntered = &entered;
    f.device.cancelGate = release.get_future().share();
    signal->cancel(); entered.get_future().wait();
    REQUIRE(f.callback->canceled == 0);
    std::shared_ptr<common::ICancellationSignal> next;
    REQUIRE(!f.session->authenticate(99, &next).isOk());
    release.set_value(); drain(f.worker);
    REQUIRE(f.callback->canceled == 1);
    f.device.cancelEntered = nullptr;
}

void closeWaitsForCancelAndRejectsOldEvents() {
    Fixture f;
    auto oldSession = f.session;
    auto signal = f.authenticate();
    f.device.deferCancel = true;
    f.session->close(); f.session->close(); drain(f.worker);
    REQUIRE(f.callback->closed == 0);
    REQUIRE(f.device.cancellations == 1);
    f.device.error(FINGERPRINT_ERROR_CANCELED); drain(f.worker);
    REQUIRE(f.callback->closed == 1);
    f.open(10);
    f.device.deferCancel = false;
    auto next = f.authenticate();
    const auto releases = f.device.releases.load();
    fingerprint_msg_t stale{}; stale.type = FINGERPRINT_AUTHENTICATED;
    stale.data.authenticated.finger.fid = 7;
    oldSession->notify(&stale); oldSession->onPointerUp(0); oldSession->close(); signal->cancel();
    drain(f.worker);
    REQUIRE(f.device.releases == releases);
    REQUIRE(f.callback->success == 0);
    f.device.match(true); drain(f.worker); REQUIRE(f.callback->success == 1);
}

void cancelFailureCannotStartAnotherHardwareOperation() {
    Fixture f;
    auto signal = f.authenticate();
    f.device.cancelError = EIO;
    signal->cancel(); drain(f.worker);
    REQUIRE(f.callback->unavailable == 1);
    REQUIRE(f.callback->canceled == 0);
    auto next = f.authenticate();
    REQUIRE(f.device.authentications == 1);
    REQUIRE(f.callback->unavailable == 2);
}

void fullAuthenticatorIdAndTemplateLists() {
    Fixture f;
    REQUIRE(f.session->getAuthenticatorId().isOk()); drain(f.worker);
    REQUIRE(f.callback->authenticatorId == INT64_C(0x1234567887654321));
    REQUIRE(f.session->enumerateEnrollments().isOk()); drain(f.worker);
    REQUIRE(f.callback->enumerations == 1);
    REQUIRE(f.callback->ids == std::vector<int32_t>({7, 19, 23}));
    REQUIRE(f.session->removeEnrollments({7, 19, 23}).isOk()); drain(f.worker);
    REQUIRE(f.callback->removals == 1);
    REQUIRE(f.callback->ids == std::vector<int32_t>({7, 19, 23}));
}

void successClearsFailuresAndUsersAreIndependent() {
    Fixture f;
    auto signal = f.authenticate();
    for (int i = 0; i < 4; ++i) f.device.match(false);
    f.device.match(true); drain(f.worker);
    REQUIRE(f.callback->timed == 0);
    auto next = f.authenticate(); f.device.match(false); drain(f.worker);
    REQUIRE(f.callback->timed == 0);
    for (int i = 0; i < 4; ++i) f.device.match(false);
    drain(f.worker); REQUIRE(f.callback->timed == 1);
    REQUIRE(f.device.cancellations == 1);
    f.session->close(); drain(f.worker);
    f.open(10);
    auto other = f.authenticate();
    REQUIRE(f.device.authentications == 3);
    REQUIRE(f.callback->timed == 1);
    f.device.match(true); drain(f.worker);
}

void enrollmentTokenAndCancellation() {
    Fixture f;
    keymaster::HardwareAuthToken token;
    token.mac.assign(32, 0xab); token.challenge = 123;
    std::shared_ptr<common::ICancellationSignal> signal;
    REQUIRE(f.session->enroll(token, &signal).isOk()); drain(f.worker);
    REQUIRE(f.device.enrollments == 1);
    REQUIRE(f.device.enrolledToken.version == 0);
    REQUIRE(f.device.enrolledToken.challenge == 123);
    REQUIRE(f.device.enrolledToken.hmac[31] == 0xab);
    signal->cancel(); drain(f.worker); REQUIRE(f.callback->canceled == 1);
    token.mac.push_back(0xab);
    REQUIRE(f.session->enroll(token, &signal).isOk()); drain(f.worker);
    REQUIRE(f.device.enrollments == 1);
    REQUIRE(f.callback->errors == 2);
}

void admissionIsSerializedWithClose() {
    Fixture f;
    std::promise<void> release;
    f.worker.gate = release.get_future().share();
    f.worker.intercept = true;
    std::thread starter([&f] {
        std::shared_ptr<common::ICancellationSignal> signal;
        REQUIRE(f.session->authenticate(42, &signal).isOk());
    });
    f.worker.intercepted.get_future().wait();
    std::thread closer([&f] { f.session->close(); });
    REQUIRE(f.callback->closed == 0);
    release.set_value(); starter.join(); closer.join(); drain(f.worker);
    REQUIRE(f.callback->closed == 1);
    REQUIRE(f.session->isClosed());
}

void lateCloseCleanupCannotTouchNewSession() {
    Fixture f;
    auto signal = f.authenticate();
    std::promise<void> release;
    f.worker.gate = release.get_future().share();
    f.worker.intercept = true;
    auto oldSession = f.session;
    std::thread closer([oldSession] { oldSession->close(); });
    f.worker.intercepted.get_future().wait();
    // close() has set mClosing, but its cleanup task is not on the worker yet.
    // A terminal callback is therefore allowed to finish/close the old session.
    f.device.match(true); drain(f.worker);
    REQUIRE(f.callback->closed == 1);
    f.open(10);
    auto next = f.authenticate();
    const auto releases = f.device.releases.load();
    release.set_value(); closer.join(); drain(f.worker);
    REQUIRE(f.device.releases == releases);
    REQUIRE(!f.session->isClosed());
    f.device.match(true); drain(f.worker);
}

void lockoutExpiryAndClosedSessionLifetime() {
    Fixture live;
    auto signal = live.authenticate();
    for (int i = 0; i < 5; ++i) live.device.match(false);
    drain(live.worker); REQUIRE(live.callback->timed == 1);
    const auto clearedBeforeExpiry = live.callback->cleared.load();

    Fixture switched;
    auto oldSignal = switched.authenticate();
    for (int i = 0; i < 5; ++i) switched.device.match(false);
    drain(switched.worker); REQUIRE(switched.callback->timed == 1);
    std::weak_ptr<Session> oldSession = switched.session;
    std::weak_ptr<Callback> oldCallback = switched.callback;
    switched.session->close(); drain(switched.worker);
    switched.callback = ndk::SharedRefBase::make<Callback>();
    switched.open(10);
    REQUIRE(oldSession.expired());
    REQUIRE(oldCallback.expired());
    const auto otherUserCleared = switched.callback->cleared.load();

    // Exercise the actual production ten-second timeout. The old callback has
    // already been destroyed; ASan also detects any attempted delayed access.
    const auto deadline = std::chrono::steady_clock::now() + 12s;
    while (live.callback->cleared == clearedBeforeExpiry &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(10ms);
    }
    drain(live.worker); drain(switched.worker);
    REQUIRE(live.callback->cleared == clearedBeforeExpiry + 1);
    REQUIRE(switched.callback->cleared == otherUserCleared);
    auto afterTimeout = live.authenticate();
    REQUIRE(live.device.authentications == 2);
    live.device.match(true); drain(live.worker);
}

void timerStopAndRestart() {
    std::atomic<int> expired = 0;
    SessionTimer timer;
    timer.start(1h, [&] { ++expired; });
    timer.stop();
    REQUIRE(expired == 0);
    std::promise<void> fired;
    timer.start(0ms, [&] { ++expired; fired.set_value(); });
    REQUIRE(fired.get_future().wait_for(2s) == std::future_status::ready);
    timer.stop(); REQUIRE(expired == 1);
}

int main() {
    initializationAndUnavailable();
    cancelBeforeStart();
    cancelWhileRunningAndStaleSignal();
    synchronousCancelDoesNotFinishBeforeVendorReturns();
    closeWaitsForCancelAndRejectsOldEvents();
    cancelFailureCannotStartAnotherHardwareOperation();
    fullAuthenticatorIdAndTemplateLists();
    successClearsFailuresAndUsersAreIndependent();
    enrollmentTokenAndCancellation();
    admissionIsSerializedWithClose();
    lateCloseCleanupCannotTouchNewSession();
    lockoutExpiryAndClosedSessionLifetime();
    timerStopAndRestart();
    std::puts("13 fingerprint lifecycle/ABI regression tests passed");
}
