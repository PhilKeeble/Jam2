#pragma once
#include <chrono>
#include <thread>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace jam2::test {
// A normal Windows millisecond sleep can add an unintended timer-tick impairment.
class ProxyPollWait final {
public:
#ifdef _WIN32
    ProxyPollWait() : timer_(CreateWaitableTimerExW(nullptr, nullptr,
        CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_MODIFY_STATE | SYNCHRONIZE)) {}
    ~ProxyPollWait() { if (timer_) CloseHandle(timer_); }
    bool wait() noexcept {
        LARGE_INTEGER due{}; due.QuadPart = -5000;
        return timer_ && SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE) &&
            WaitForSingleObject(timer_, 1000) == WAIT_OBJECT_0;
    }
private:
    HANDLE timer_ = nullptr;
#else
    ProxyPollWait() = default;
    bool wait() noexcept { std::this_thread::sleep_for(std::chrono::microseconds(500)); return true; }
#endif
public:
    ProxyPollWait(const ProxyPollWait&) = delete;
    ProxyPollWait& operator=(const ProxyPollWait&) = delete;
};
}
