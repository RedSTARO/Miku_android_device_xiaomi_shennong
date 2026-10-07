/*
 * Copyright (C) 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace aidl::android::hardware::biometrics::fingerprint {

// The owner starts/stops this timer from its worker thread. The expiry action must
// only enqueue work, never wait for that worker. stop() joins the timer before
// returning; already queued expiry work must additionally check its generation.
class SessionTimer {
  public:
    ~SessionTimer() { stop(); }

    void start(std::chrono::milliseconds delay, std::function<void()> action) {
        stop();
        {
            std::lock_guard lock(mMutex);
            mStopped = false;
        }
        mThread = std::thread([this, delay, action = std::move(action)] {
            std::unique_lock lock(mMutex);
            if (mCondition.wait_for(lock, delay, [this] { return mStopped; })) return;
            lock.unlock();
            action();
        });
    }

    void stop() {
        {
            std::lock_guard lock(mMutex);
            mStopped = true;
        }
        mCondition.notify_all();
        if (mThread.joinable()) mThread.join();
    }

  private:
    std::mutex mMutex;
    std::condition_variable mCondition;
    bool mStopped = true;
    std::thread mThread;
};

}  // namespace aidl::android::hardware::biometrics::fingerprint
