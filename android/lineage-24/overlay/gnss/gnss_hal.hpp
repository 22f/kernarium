// SPDX-License-Identifier: Apache-2.0
//
// The state behind the AIDL IGnss of the host's virtual receiver: the engine of
// the framework's current callback, and the position mode and toggles that
// outlive one callback. Gnss.cpp is only the AIDL adapter over this class, so
// the entry points the framework's binder threads call, and the locks they
// take, live in a header a host test compiles with fakes.
#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

#include "gnss_engine.hpp"

namespace andarium::gnss {

class Hal {
public:
    enum class StartResult { started, no_callback, no_receiver };

    explicit Hal(Env env) : env_(std::move(env)) {}

    // The framework's setCallback: a new engine for the new callback replaces
    // the old one, and `announce` tells the framework what the receiver is.
    void set_callback(Sink sink, const std::function<void()>& announce) {
        std::lock_guard lock(mutex_);
        engine_.reset();
        engine_ = std::make_unique<Engine>(env_, std::move(sink));
        engine_->set_min_interval_ms(interval_ms_);
        engine_->set_nmea(nmea_);
        engine_->set_sv_status(sv_status_);
        if (announce) announce();
    }

    StartResult start() {
        std::lock_guard lock(mutex_);
        if (!engine_) return StartResult::no_callback;
        return engine_->start() ? StartResult::started : StartResult::no_receiver;
    }

    void stop() {
        std::lock_guard lock(mutex_);
        if (engine_) engine_->stop();
    }

    void close() {
        std::lock_guard lock(mutex_);
        engine_.reset();
    }

    // False when the interval is not positive.
    bool set_position_mode(int interval_ms) {
        if (interval_ms <= 0) return false;
        std::lock_guard lock(mutex_);
        interval_ms_ = interval_ms;
        if (engine_) engine_->set_min_interval_ms(interval_ms_);
        return true;
    }

    void set_sv_status(bool on) {
        std::lock_guard lock(mutex_);
        sv_status_ = on;
        if (engine_) engine_->set_sv_status(on);
    }

    void set_nmea(bool on) {
        std::lock_guard lock(mutex_);
        nmea_ = on;
        if (engine_) engine_->set_nmea(on);
    }

    std::optional<DebugFix> last_fix() {
        std::lock_guard lock(mutex_);
        if (!engine_) return std::nullopt;
        return engine_->last_fix();
    }

private:
    const Env env_;
    // Guards the members below.
    std::mutex mutex_;
    std::unique_ptr<Engine> engine_;
    int interval_ms_ = Engine::kDefaultIntervalMs;
    bool nmea_ = false;
    bool sv_status_ = false;
};

}  // namespace andarium::gnss
