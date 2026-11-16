// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The application's hold of the guest's GPU requests (kernarium_runtime_set_gpu_hold) as the runtime runs it
 * (KernariumGpuHold.hpp), over the real request gate, the real camera control (`BackgroundVideo`) and the real run loop's
 * Lifecycle: that a guest that restarts while a hold is pending does not boot behind the gate.
 *
 * The times are the application's, halved: a drain of 0.5 s for a request that runs and a camera bound of 1 s.
 */
#include "KernariumGpuHold.hpp"
#include "KernariumRestartLoop.hpp"
#include "camera/background_video.hpp"
#include "require.hpp"
#include "vhost_user/request_gate.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using kernarium::gpu_hold::Gate;
using kernarium::gpu_hold::Timing;
using uml::camera::BackgroundVideo;
using uml::testing::require;
using Clock = BackgroundVideo::Clock;

constexpr Timing halfTiming {500ms, 1000ms};

/* The in-process GPU service as the gate uses it, over the real request gate. `beforeClosing` lets a row decide when the
 * call that closes the gate gets to do it. */
class FakeGpu final {
public:
    bool holdRequests(std::chrono::milliseconds budget) noexcept
    {
        if (beforeClosing) beforeClosing();
        std::int64_t unset = -1;
        closedAt.compare_exchange_strong(unset, Clock::now().time_since_epoch().count());
        return gate.hold(budget);
    }
    void releaseRequests() noexcept { gate.release(); }
    [[nodiscard]] bool requestsHeld() const noexcept { return gate.held(); }

    uml::gpu::RequestGate gate;
    std::function<void()> beforeClosing;
    /* When the first call that closes the gate began, in the clock's own count; -1 before. */
    std::atomic<std::int64_t> closedAt {-1};

    [[nodiscard]] std::chrono::milliseconds closedAfter(Clock::time_point edge) const
    {
        const auto at = closedAt.load();
        require(at >= 0, "the gate was never closed");
        return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::time_point(Clock::duration(at)) - edge);
    }
};

/* What the runtime keeps and the gate works on. */
struct Runtime final {
    explicit Runtime(Timing timing = {}) : gate(mutex, active, lifecycle, timing) { active = &gpu; }
    std::mutex mutex;
    FakeGpu gpu;
    FakeGpu* active {};
    kernarium::restart::Lifecycle lifecycle;
    Gate<FakeGpu> gate;
};

const auto saysNothing = [](const std::string&) {};

/* ---- A guest that restarts while the hold waits for the camera does not boot behind the gate. */

class RestartingGuest final : public kernarium::restart::Machine {
public:
    RestartingGuest()
    {
        runtime.gpu.beforeClosing = [this] {
            const auto until = Clock::now() + 5s;
            while (!letTheGateClose_.load() && Clock::now() < until) std::this_thread::sleep_for(1ms);
        };
    }
    ~RestartingGuest() override
    {
        letTheGateClose_.store(true);
        if (hold_.joinable()) hold_.join();
        video.detach();
    }

    /* The application asks for the hold; it waits for the guest's camera first. */
    void askForTheHold()
    {
        hold_ = std::thread([this] { (void)runtime.gate.set(kernarium::gpu_hold::withCamera, &video, saysNothing); });
    }
    void letTheGateClose()
    {
        letTheGateClose_.store(true);
        hold_.join();
    }

    bool startBoot(unsigned number) override
    {
        started.store(number + 1);
        return true;
    }
    int enter() override
    {
        if (started.load() == 1) {
            video.attach([] {});
            askForTheHold();
            /* The guest's camera layer answers, and the guest restarts. */
            std::uint64_t request = 0;
            const auto until = Clock::now() + 5s;
            while (!video.take(request) && Clock::now() < until) std::this_thread::sleep_for(1ms);
            require(request != 0, "the hold ended the guest's video");
            video.ended(request, 1);
            return 0;
        }
        /* The restarted guest's first GPU request. */
        enteredWithGateHeld = runtime.gpu.requestsHeld();
        enteredAdmitted = bool(runtime.gpu.gate.enter());
        return 0;
    }
    bool runEnd(kernarium::restart::RunEnd& end) override
    {
        end.kind = started.load() == 1 ? kernarium::restart::RunEnd::Kind::restart : kernarium::restart::RunEnd::Kind::powerOff;
        return true;
    }
    /* The old guest's camera goes with it, which ends the wait that was for it. */
    void stopBoot() noexcept override { video.detach(); }
    void resetLink() noexcept override {}
    bool stopRequested() noexcept override { return false; }
    bool bootHeld() noexcept override { return runtime.lifecycle.held(); }
    void waitWhileHeld(std::chrono::milliseconds slice) override
    {
        waits.fetch_add(1);
        runtime.lifecycle.waitWhileHeld(slice);
    }
    void pause(std::chrono::milliseconds delay) override { std::this_thread::sleep_for(delay); }
    kernarium::restart::Clock::time_point now() noexcept override { return kernarium::restart::Clock::now(); }
    void say(const std::string&) noexcept override {}
    void restarting(unsigned) noexcept override {}
    bool consoleAnswers() noexcept override { return true; }
    void booted(unsigned, bool) noexcept override {}

    Runtime runtime {halfTiming};
    BackgroundVideo video;
    std::atomic<unsigned> started {};
    std::atomic<unsigned> waits {};
    bool enteredWithGateHeld {};
    bool enteredAdmitted {};

private:
    std::thread hold_;
    std::atomic<bool> letTheGateClose_ {};
};

void aGuestThatRestartsWhileTheHoldWaitsForTheCameraBootsAfterTheRelease()
{
    RestartingGuest guest;
    int result = -1;
    std::thread loop([&] {
        result = kernarium::restart::run(guest, {.consolePoll = 1ms, .consoleBound = 20ms, .heldPoll = 1ms});
    });
    const auto until = Clock::now() + 5s;
    /* The loop has the guest restarted and waits at the boot guard, with the hold asked for and the gate still open. */
    while (guest.waits.load() < 3 && Clock::now() < until) std::this_thread::sleep_for(1ms);
    require(guest.waits.load() >= 3, "the restarted guest's boot waits for the hold that is pending");
    require(!guest.runtime.gpu.requestsHeld(), "the gate has not closed yet: the hold waits for the old guest's camera");
    /* The hold's camera wait ended with the old guest's camera; its close lands now, while the boot is held back. */
    guest.letTheGateClose();
    require(guest.runtime.gpu.requestsHeld(), "the gate is closed");
    require(guest.started.load() == 1, "and the restarted guest has not begun to boot behind it");
    /* The application returns. */
    (void)guest.runtime.gate.set(kernarium::gpu_hold::released, &guest.video, saysNothing);
    loop.join();
    require(result == 0, "the launch ends");
    require(guest.started.load() == 2, "the restarted guest boots once the requests are given back");
    require(!guest.enteredWithGateHeld && guest.enteredAdmitted, "and its GPU requests are answered");
}

} // namespace

int main(int argc, char** argv)
{
    /* A row's name on the command line runs that row alone. */
    const char* const only = argc > 1 ? argv[1] : nullptr;
#define ROW(name) \
    if (only == nullptr || std::string(only) == #name) name()
    ROW(aGuestThatRestartsWhileTheHoldWaitsForTheCameraBootsAfterTheRelease);
#undef ROW
    return 0;
}
