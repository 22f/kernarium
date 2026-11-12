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

// The framework's binder threads call these entry points, any of them at any
// time. `mutex_` guards only the engine pointer and the settings: it is never
// held while an engine runs or is torn down, and nothing here calls the
// framework (the engine's reporter does, from a thread of its own), so a
// framework thread that calls an entry point while it holds a lock of its own
// can never wait on a thread of this HAL that waits for the framework.
class Hal {
public:
    enum class StartResult { started, no_callback, no_receiver };

    explicit Hal(Env env) : env_(std::move(env)) {}

    ~Hal() { close(); }

    // The framework's setCallback: a new engine for the new callback replaces
    // the old one, which ends silently. The engine tells the callback what the
    // receiver is (`sink.ready`) before anything else.
    void set_callback(Sink sink) {
        auto fresh = std::make_shared<Engine>(env_, std::move(sink));
        std::shared_ptr<Engine> old;
        {
            std::lock_guard lock(mutex_);
            fresh->set_min_interval_ms(interval_ms_);
            fresh->set_nmea(nmea_);
            fresh->set_sv_status(sv_status_);
            old = std::move(engine_);
            engine_ = std::move(fresh);
        }
        if (old) old->close();
    }

    StartResult start() {
        const auto engine = current();
        if (!engine) return StartResult::no_callback;
        return engine->start() ? StartResult::started : StartResult::no_receiver;
    }

    void stop() {
        if (const auto engine = current()) engine->stop();
    }

    // The framework's close(): the session ends silently and the callback is
    // dropped, with what was still queued for it.
    void close() {
        std::shared_ptr<Engine> old;
        {
            std::lock_guard lock(mutex_);
            old = std::move(engine_);
        }
        if (old) old->close();
    }

    // False when the interval is not positive.
    bool set_position_mode(int interval_ms) {
        if (interval_ms <= 0) return false;
        std::shared_ptr<Engine> engine;
        {
            std::lock_guard lock(mutex_);
            interval_ms_ = interval_ms;
            engine = engine_;
        }
        if (engine) engine->set_min_interval_ms(interval_ms);
        return true;
    }

    void set_sv_status(bool on) {
        std::shared_ptr<Engine> engine;
        {
            std::lock_guard lock(mutex_);
            sv_status_ = on;
            engine = engine_;
        }
        if (engine) engine->set_sv_status(on);
    }

    void set_nmea(bool on) {
        std::shared_ptr<Engine> engine;
        {
            std::lock_guard lock(mutex_);
            nmea_ = on;
            engine = engine_;
        }
        if (engine) engine->set_nmea(on);
    }

    std::optional<DebugFix> last_fix() {
        const auto engine = current();
        if (!engine) return std::nullopt;
        return engine->last_fix();
    }

private:
    std::shared_ptr<Engine> current() {
        std::lock_guard lock(mutex_);
        return engine_;
    }

    const Env env_;
    // Guards the members below, and nothing else.
    std::mutex mutex_;
    std::shared_ptr<Engine> engine_;
    int interval_ms_ = Engine::kDefaultIntervalMs;
    bool nmea_ = false;
    bool sv_status_ = false;
};

}  // namespace andarium::gnss
