#!/usr/bin/env python3
"""Run the production Engine, Session, LockoutTracker and WorkerThread on Linux.

Only Android binder/config/logging/fd interfaces are shimmed. No lifecycle,
callback conversion, cancellation, timer, HAL dispatch or lockout logic is copied.
Native builds of SessionTest.cpp use the real Android interfaces instead.
"""
import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('--sanitize', choices=['address', 'thread'])
args = parser.parse_args()
fp = Path(__file__).resolve().parents[1]
root = fp.parents[3]

with tempfile.TemporaryDirectory(prefix='shennong-fingerprint-test-') as temporary:
    build = Path(temporary)
    def put(name, text):
        path = build / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)

    # Read binder method declarations from the production Session header so the
    # shim stays limited to the platform interface, never its implementation.
    declarations = re.findall(r'ndk::ScopedAStatus\s+\w+\([^;]*?\) override;',
                              (fp / 'include/Session.h').read_text(), re.S)
    methods = '\n'.join('virtual ' + d.replace(' override;', ' = 0;') for d in declarations)
    put('platform.h', r'''
#pragma once
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <vector>
using binder_status_t = int;
constexpr int EX_ILLEGAL_STATE = -5, EX_ILLEGAL_ARGUMENT = -3;
struct AIBinder {};
struct AIBinder_DeathRecipient {
    void (*died)(void*); void (*unlinked)(void*) = nullptr; void* cookie = nullptr;
};
inline auto* AIBinder_DeathRecipient_new(void (*died)(void*)) {
    return new AIBinder_DeathRecipient{died};
}
inline void AIBinder_DeathRecipient_setOnUnlinked(AIBinder_DeathRecipient* r, void (*f)(void*)) {
    r->unlinked = f;
}
inline binder_status_t AIBinder_linkToDeath(AIBinder*, AIBinder_DeathRecipient* r, void* cookie) {
    r->cookie = cookie; return 0;
}
inline void AIBinder_DeathRecipient_delete(AIBinder_DeathRecipient* r) {
    if (r->cookie && r->unlinked) r->unlinked(r->cookie);
    delete r;
}
namespace ndk {
class ScopedAStatus {
    bool good;
  public:
    explicit ScopedAStatus(bool good) : good(good) {}
    static ScopedAStatus ok() { return ScopedAStatus(true); }
    static ScopedAStatus fromExceptionCode(int) { return ScopedAStatus(false); }
    bool isOk() const { return good; }
};
class SharedRefBase : public std::enable_shared_from_this<SharedRefBase> {
  public:
    virtual ~SharedRefBase() = default;
    template<class T> std::shared_ptr<T> ref() { return std::static_pointer_cast<T>(shared_from_this()); }
    template<class T, class... Args> static auto make(Args&&... args) {
        return std::shared_ptr<T>(new T(std::forward<Args>(args)...));
    }
};
}
namespace aidl::android::hardware::keymaster {
enum class HardwareAuthenticatorType : int32_t { NONE = 0, PASSWORD = 1, FINGERPRINT = 2 };
struct HardwareAuthToken {
    int64_t challenge = 0, userId = 0, authenticatorId = 0;
    HardwareAuthenticatorType authenticatorType = HardwareAuthenticatorType::NONE;
    struct { int64_t milliSeconds = 0; } timestamp;
    std::vector<uint8_t> mac;
};
}
namespace aidl::android::hardware::biometrics::common {
struct OperationContext {};
class ICancellationSignal : public ndk::SharedRefBase {
  public: virtual ndk::ScopedAStatus cancel() = 0;
};
using BnCancellationSignal = ICancellationSignal;
}
namespace aidl::android::hardware::biometrics::fingerprint {
using ndk::SharedRefBase;
enum class Error { UNKNOWN, HW_UNAVAILABLE, UNABLE_TO_PROCESS, TIMEOUT, NO_SPACE, CANCELED,
                   UNABLE_TO_REMOVE, VENDOR, BAD_CALIBRATION };
enum class AcquiredInfo { UNKNOWN, GOOD, PARTIAL, INSUFFICIENT, SENSOR_DIRTY, TOO_SLOW, TOO_FAST,
                          VENDOR, START, TOO_DARK, TOO_BRIGHT, IMMOBILE, RETRYING_CAPTURE,
                          LIFT_TOO_SOON, POWER_PRESS };
struct PointerContext { int32_t pointerId = 0; float x = 0, y = 0, minor = 0, major = 0; };
struct SensorLocation {
    int32_t sensorLocationX = 0, sensorLocationY = 0, sensorRadius = 0;
    std::string display;
};
class ISessionCallback : public ndk::SharedRefBase {
  public:
    virtual ndk::ScopedAStatus onChallengeGenerated(int64_t) = 0;
    virtual ndk::ScopedAStatus onChallengeRevoked(int64_t) = 0;
    virtual ndk::ScopedAStatus onAcquired(AcquiredInfo, int32_t) = 0;
    virtual ndk::ScopedAStatus onError(Error, int32_t) = 0;
    virtual ndk::ScopedAStatus onEnrollmentProgress(int32_t, int32_t) = 0;
    virtual ndk::ScopedAStatus onAuthenticationSucceeded(int32_t, const keymaster::HardwareAuthToken&) = 0;
    virtual ndk::ScopedAStatus onAuthenticationFailed() = 0;
    virtual ndk::ScopedAStatus onLockoutTimed(int64_t) = 0;
    virtual ndk::ScopedAStatus onLockoutPermanent() = 0;
    virtual ndk::ScopedAStatus onLockoutCleared() = 0;
    virtual ndk::ScopedAStatus onInteractionDetected() = 0;
    virtual ndk::ScopedAStatus onEnrollmentsEnumerated(const std::vector<int32_t>&) = 0;
    virtual ndk::ScopedAStatus onEnrollmentsRemoved(const std::vector<int32_t>&) = 0;
    virtual ndk::ScopedAStatus onAuthenticatorIdRetrieved(int64_t) = 0;
    virtual ndk::ScopedAStatus onAuthenticatorIdInvalidated(int64_t) = 0;
    virtual ndk::ScopedAStatus onSessionClosed() = 0;
};
using BnSessionCallback = ISessionCallback;
class BnSession : public ndk::SharedRefBase {
  public:
''' + methods + '\n};\n}\n')
    for name in ['fingerprint/BnSession', 'fingerprint/BnSessionCallback',
                 'fingerprint/ISessionCallback', 'fingerprint/SensorLocation',
                 'common/BnCancellationSignal']:
        put('aidl/android/hardware/biometrics/' + name + '.h', '#include "platform.h"\n')
    put('aidl/android/hardware/keymaster/HardwareAuthToken.h', '#include "platform.h"\n')
    put('Fingerprint.h', r'''
#pragma once
#include "fingerprint-xiaomi.h"
#include <string>
namespace aidl::android::hardware::biometrics::fingerprint {
struct HostConfig { template<class T> T get(const char*) { return T{}; } };
struct Fingerprint {
    static HostConfig& cfg() { static HostConfig c; return c; }
    static void notify(const fingerprint_msg_t*);
};
}
''')
    put('android-base/logging.h', r'''
#pragma once
#include <cstdio>
#include <cstdlib>
struct HostLog {
    bool good = true;
    ~HostLog() { if (!good) { std::fputs("Production CHECK failed\n", stderr); std::abort(); } }
    template<class T> HostLog& operator<<(const T&) { return *this; }
};
#define LOG(level) HostLog{}
#define PLOG(level) HostLog{}
#define CHECK(value) HostLog{bool(value)}
#define CHECK_GE(a,b) CHECK((a) >= (b))
''')
    put('android-base/parseint.h', r'''
#pragma once
#include <charconv>
#include <string>
namespace android::base {
template<class T> bool ParseInt(const std::string& text, T* value) {
    auto result = std::from_chars(text.data(), text.data()+text.size(), *value);
    return result.ec == std::errc{} && result.ptr == text.data()+text.size();
}
}
''')
    put('android-base/unique_fd.h', r'''
#pragma once
#include <unistd.h>
namespace android::base {
class unique_fd {
    int fd = -1;
  public:
    ~unique_fd() { reset(); }
    int get() const { return fd; }
    void reset(int value = -1) { if (fd >= 0) close(fd); fd = value; }
};
}
''')
    # libhardware's types and Xiaomi's display UAPI are real production headers;
    # the two unused graphics includes below do not participate in fingerprint.
    put('cutils/native_handle.h', '')
    put('system/graphics.h', '')
    executable = build / 'fingerprint-tests'
    command = [os.environ.get('CXX', 'c++'), '-std=c++20', '-pthread', '-g', '-O1',
               '-Wall', '-Wextra', '-Werror', '-Wno-missing-field-initializers',
               '-DSHENNONG_HOST_TEST', '-include', str(build / 'platform.h')]
    if args.sanitize:
        # Avoid GCC's -O1/libstdc++ regex false-positive uninitialized warnings
        # in Android's shared Util.h while retaining all production warnings.
        command += ['-O0', '-fsanitize=' + args.sanitize, '-fno-omit-frame-pointer']
        if args.sanitize == 'address':
            command += ['-fsanitize=undefined']
    for include in [build, fp / 'include', root / 'hardware/libhardware/include_all',
                    root / 'hardware/interfaces/biometrics/common/thread/include',
                    root / 'hardware/interfaces/biometrics/common/util/include',
                    root / 'kernel/xiaomi/sm8650-modules/qcom/opensource/display-drivers/include/uapi']:
        command += ['-I', str(include)]
    command += [str(fp / path) for path in ['Session.cpp', 'FingerprintEngine.cpp',
                                           'LockoutTracker.cpp', 'tests/SessionTest.cpp']]
    command += [str(root / 'hardware/interfaces/biometrics/common/thread/WorkerThread.cpp'),
                '-o', str(executable)]
    subprocess.run(command, check=True)
    subprocess.run([str(executable)], check=True, timeout=45)
