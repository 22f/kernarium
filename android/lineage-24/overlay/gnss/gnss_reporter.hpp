// SPDX-License-Identifier: Apache-2.0
//
// The one thread that calls the framework. The framework's callbacks are
// synchronous binder calls, and the framework answers one of them while it
// holds locks of its own, some of which its threads also hold while they call
// into this HAL (it registers a status listener under its listener lock and
// asks the HAL to collect satellite status from there). A HAL thread that
// waits for the framework while it holds a lock one of its own entry points
// takes can therefore close a cycle with a framework thread that waits for
// that entry point. So no entry point, and no thread that holds a lock of the
// HAL, calls the framework: they post what the framework is to hear, and this
// thread, which holds no lock of the HAL while it calls, makes the calls in
// the order they were posted.
//
// Posting never waits for a delivery. Closing, and destroying, never wait for
// one either: a callback already in flight finishes on its own, and nothing
// queued behind it is delivered. The header has no Android dependency, so a
// host test compiles it with callbacks that block.
#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

namespace andarium::gnss {

class Reporter {
public:
    // A status, or the first announcement of a callback, is never dropped. A
    // report (a location, the sentences of an epoch, the satellites in view) is
    // the newest of its kind that matters: when the framework is slow, the
    // oldest are dropped past kMaxReports.
    enum class Kind { essential, report };

    static constexpr std::size_t kMaxReports = 64;

    Reporter() : state_(std::make_shared<State>()) {
        thread_ = std::thread([state = state_] { run(state); });
    }

    // Never waits for a callback in flight: the thread then ends by itself once
    // that callback returns, holding what it needs to do so.
    ~Reporter() {
        bool delivering = false;
        {
            std::lock_guard lock(state_->mutex);
            state_->closed = true;
            state_->queue.clear();
            state_->reports = 0;
            delivering = state_->delivering;
        }
        state_->wake.notify_all();
        if (delivering) {
            thread_.detach();
        } else {
            thread_.join();
        }
    }

    Reporter(const Reporter&) = delete;
    Reporter& operator=(const Reporter&) = delete;

    // Queues `deliver` after everything posted before it. Ignored once closed.
    void post(Kind kind, std::function<void()> deliver) {
        {
            std::lock_guard lock(state_->mutex);
            if (state_->closed) return;
            if (kind == Kind::report) {
                if (state_->reports >= kMaxReports) drop_oldest_report(*state_);
                ++state_->reports;
            }
            state_->queue.push_back(Item{kind, std::move(deliver)});
        }
        state_->wake.notify_one();
    }

    // Drops what is queued and delivers nothing more. At most the callback
    // already in flight is still running when this returns.
    void close() {
        {
            std::lock_guard lock(state_->mutex);
            state_->closed = true;
            state_->queue.clear();
            state_->reports = 0;
        }
        state_->wake.notify_all();
    }

private:
    struct Item {
        Kind kind;
        std::function<void()> deliver;
    };

    struct State {
        std::mutex mutex;
        std::condition_variable wake;
        std::deque<Item> queue;
        std::size_t reports = 0;
        bool closed = false;
        bool delivering = false;
    };

    static void drop_oldest_report(State& state) {
        for (auto it = state.queue.begin(); it != state.queue.end(); ++it) {
            if (it->kind == Kind::report) {
                state.queue.erase(it);
                --state.reports;
                return;
            }
        }
    }

    // Holds nothing of the HAL, and `state->mutex` never while it calls out.
    static void run(const std::shared_ptr<State>& state) {
        std::unique_lock lock(state->mutex);
        for (;;) {
            state->wake.wait(lock, [&] { return state->closed || !state->queue.empty(); });
            if (state->closed) return;
            Item item = std::move(state->queue.front());
            state->queue.pop_front();
            if (item.kind == Kind::report) --state->reports;
            state->delivering = true;
            lock.unlock();
            item.deliver();
            item.deliver = nullptr;
            lock.lock();
            state->delivering = false;
        }
    }

    std::shared_ptr<State> state_;
    std::thread thread_;
};

}  // namespace andarium::gnss
