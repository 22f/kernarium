// SPDX-License-Identifier: Apache-2.0
//
// The GNSS HAL's engine: a session on the host's virtual receiver. start()
// holds a wakelock, opens the receiver (which powers it on the host) and runs a
// thread that parses what it writes; stop() joins that thread, powers the
// receiver off and lets the wakelock go. A session whose receiver goes away by
// itself (it closed or failed) ends there: the thread lets the wakelock go and
// reports session_end and engine_off once, and the stop() that follows reports
// nothing more. The receiver is not opened again; the next start() opens it.
//
// Nothing here calls the framework. Every report, and every status, is posted
// to the engine's Reporter (gnss_reporter.hpp), whose thread makes the calls in
// the order they were posted and holds no lock of the HAL while it does. So the
// statuses of a session come in order with its locations (engine_on and
// session_begin before the first, session_end and engine_off after the last,
// none of an earlier session after a later one's session_begin), and no entry
// point of the engine waits for the framework. Because the calls are made
// afterwards, stop() returns before the framework has heard session_end, and
// close() drops what is still queued.
//
// The wakelock is the one decision of this file that is not obvious. The
// framework holds a partial wakelock only while it handles each report
// (GnssLocationProvider.postWithWakeLockHeld, at most 30 s), and the HAL
// contract offers gnssAcquireWakelockCb for the same purpose, "while handling
// GNSS events". Real receivers need nothing more, because the chip wakes the
// application processor for each fix. The host's receiver wakes a suspended
// guest once an epoch (darwin/device/gnss/README.md, "Suspend"), so a guest
// that suspends itself with its display off would suspend and wake every
// second for as long as a client listens. The engine therefore holds a
// wakelock for the whole session, from start() to stop(), as the product holds
// one while an adb host is connected. It is a libpower wakelock of this
// process: the framework's "GPS" lock is owned by system_server and would
// outlive a HAL that died mid-session, but this one dies with its holder. It
// holds back only Android's own autosuspend; a suspend the host asks for still
// suspends the guest.
//
// The header has no Android dependency (the operating system is behind Env, the
// framework behind Sink), so a host test compiles it with fakes.
#pragma once

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "gnss_reporter.hpp"
#include "location_policy.hpp"
#include "nmea.hpp"

namespace andarium::gnss {

// The values of IGnssCallback.GnssStatusValue.
enum class Status : int32_t { session_begin = 1, session_end = 2, engine_on = 3, engine_off = 4 };

// What the engine needs from the operating system.
struct Env {
    // The receiver, opened non-blocking: a descriptor, or -errno. Opening it
    // powers the receiver on the host, and closing it powers it off.
    std::function<int()> open_device;
    // A wakelock held until the pointer is released, or null when none can be
    // had (the session then runs without one and the guest may bounce).
    std::function<std::shared_ptr<void>()> hold_wakelock;
    std::function<int64_t()> realtime_ns;
    std::function<int64_t()> boottime_ns;
    std::function<void(const std::string&)> log;
};

// What the engine reports. Every call is made on the engine's reporter thread,
// one at a time in the order the engine posted them, with no lock of the engine
// held: a sink may take locks of the framework, and may even call back into the
// engine, without blocking an entry point of the HAL.
struct Sink {
    // Made once, first: tells the framework what the receiver is.
    std::function<void()> ready;
    std::function<void(Status)> status;
    std::function<void(const Location&)> location;
    // One NMEA sentence with the time of its fix, in Unix milliseconds.
    std::function<void(int64_t, const std::string&)> nmea;
    // The satellites in view: always none, since the host has no sky to count.
    std::function<void()> sv_status;
};

// The last position the session reported, for IGnssDebug.
struct DebugFix {
    nmea::Position position;
    int64_t boottime_ns = 0;
};

class Engine {
public:
    static constexpr int kDefaultIntervalMs = 1000;

    Engine(Env env, Sink sink) : env_(std::move(env)), sink_(std::make_shared<const Sink>(std::move(sink))) {
        if (sink_->ready) post(Reporter::Kind::essential, [] (const Sink& sink) { sink.ready(); });
    }

    // Never waits for the framework: what is queued is dropped.
    ~Engine() {
        std::lock_guard control(control_);
        stop_locked(false);
    }

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // Begins a session. False when the receiver cannot be opened, or after
    // close(). A session already running is ended first, without statuses, as
    // the framework's start() after a start() restarts.
    bool start() {
        std::lock_guard control(control_);
        if (closed_) return false;
        stop_locked(false);
        std::shared_ptr<void> wakelock;
        if (env_.hold_wakelock) wakelock = env_.hold_wakelock();
        if (!wakelock) log("no wakelock: the guest may suspend during the session");
        const int device = env_.open_device();
        if (device < 0) {
            log("cannot open the receiver: errno " + std::to_string(-device));
            return false;
        }
        int pipe_fds[2];
        if (pipe(pipe_fds) != 0) {
            ::close(device);
            log("cannot make the wake pipe: errno " + std::to_string(errno));
            return false;
        }
        for (const int descriptor : pipe_fds) {
            fcntl(descriptor, F_SETFD, FD_CLOEXEC);
            fcntl(descriptor, F_SETFL, fcntl(descriptor, F_GETFL) | O_NONBLOCK);
        }
        parser_.reset();
        pacer_.reset();
        end_reported_ = false;
        {
            std::lock_guard guard(shared_);
            wakelock_ = std::move(wakelock);
            last_fix_.reset();
        }
        device_ = device;
        wake_read_ = pipe_fds[0];
        wake_write_ = pipe_fds[1];
        running_ = true;
        notify(Status::engine_on);
        notify(Status::session_begin);
        thread_ = std::thread([this] { run(); });
        return true;
    }

    // Ends the session: no report of it is made after session_end.
    void stop() {
        std::lock_guard control(control_);
        stop_locked(true);
    }

    // Ends the session silently and for good: nothing queued is delivered, and
    // start() fails from now on. The framework's close().
    void close() {
        std::lock_guard control(control_);
        closed_ = true;
        stop_locked(false);
        reporter_.close();
    }

    // The interval of the framework's setPositionMode, in milliseconds.
    void set_min_interval_ms(int interval_ms) { interval_ms_ = interval_ms > 0 ? interval_ms : kDefaultIntervalMs; }
    void set_nmea(bool on) { nmea_on_ = on; }
    void set_sv_status(bool on) { sv_status_on_ = on; }

    // True from start() until stop(), or until the receiver went away and the
    // session reported its end.
    bool running() const {
        std::lock_guard control(control_);
        return running_ && !end_reported_;
    }

    std::optional<DebugFix> last_fix() const {
        std::lock_guard guard(shared_);
        return last_fix_;
    }

private:
    void log(const std::string& message) const {
        if (env_.log) env_.log(message);
    }

    // Queues a call to the framework: it is made later, on the reporter's
    // thread, from a copy of the sink that this closure keeps alive.
    template <typename Call>
    void post(Reporter::Kind kind, Call call) {
        reporter_.post(kind, [sink = sink_, call = std::move(call)] { call(*sink); });
    }

    void notify(Status status) {
        if (!sink_->status) return;
        post(Reporter::Kind::essential, [status](const Sink& sink) { sink.status(status); });
    }

    // Joins the thread, closes the receiver and releases the wakelock. With
    // `notify_framework` the framework is told the session and the engine ended,
    // unless the thread already told it.
    // The caller holds `control_`, or is the destructor.
    void stop_locked(bool notify_framework) {
        if (!running_) return;
        running_ = false;
        const char byte = 0;
        // The pipe is non-blocking and one byte is all the thread needs.
        (void)!::write(wake_write_, &byte, 1);
        if (thread_.joinable()) thread_.join();
        ::close(device_);
        ::close(wake_read_);
        ::close(wake_write_);
        device_ = wake_read_ = wake_write_ = -1;
        {
            std::lock_guard guard(shared_);
            wakelock_.reset();
        }
        if (notify_framework) report_end();
    }

    // Tells the framework the session and the engine ended, once per session.
    void report_end() {
        if (end_reported_.exchange(true)) return;
        notify(Status::session_end);
        notify(Status::engine_off);
    }

    void run() {
        char buffer[512];
        // True when stop() ended the loop; stop() then reports the end itself.
        bool asked_to_stop = false;
        for (;;) {
            pollfd fds[2] = {{device_, POLLIN, 0}, {wake_read_, POLLIN, 0}};
            if (poll(fds, 2, -1) < 0) {
                if (errno == EINTR) continue;
                log("poll failed: errno " + std::to_string(errno));
                break;
            }
            if (fds[1].revents != 0) {
                asked_to_stop = true;
                break;
            }
            if ((fds[0].revents & POLLIN) != 0) {
                const ssize_t count = read(device_, buffer, sizeof(buffer));
                if (count < 0) {
                    if (errno == EINTR || errno == EAGAIN) continue;
                    log("read failed: errno " + std::to_string(errno));
                    break;
                }
                if (count == 0) {
                    log("the receiver closed");
                    break;
                }
                for (const nmea::Epoch& epoch : parser_.feed(std::string_view(buffer, static_cast<size_t>(count)))) {
                    report(epoch);
                }
            } else if ((fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
                log("the receiver failed");
                break;
            }
        }
        // A session with no receiver behind it has nothing to keep awake for.
        {
            std::lock_guard guard(shared_);
            wakelock_.reset();
        }
        // The receiver went away by itself: the framework is told, with no lock
        // of the engine held, and the stop() that follows finds it told.
        if (!asked_to_stop) report_end();
    }

    void report(const nmea::Epoch& epoch) {
        const int64_t realtime = env_.realtime_ns();
        const int64_t boottime = env_.boottime_ns();
        if (sv_status_on_ && sink_->sv_status) {
            post(Reporter::Kind::report, [](const Sink& sink) { sink.sv_status(); });
        }
        if (nmea_on_ && sink_->nmea) {
            const int64_t at_ms = epoch.utc_ms != 0 ? epoch.utc_ms : realtime / 1'000'000;
            post(Reporter::Kind::report, [at_ms, sentences = listener_sentences(epoch)](const Sink& sink) {
                for (const std::string& sentence : sentences) sink.nmea(at_ms, sentence);
            });
        }
        if (epoch.position.fix_class == nmea::FixClass::none) return;
        if (!pacer_.admit(epoch.position.utc_ms, boottime, interval_ms_)) return;
        const auto location = make_location(epoch.position, realtime, boottime);
        if (!location) return;
        {
            std::lock_guard guard(shared_);
            last_fix_ = DebugFix{epoch.position, boottime};
        }
        if (sink_->location) {
            post(Reporter::Kind::report, [fix = *location](const Sink& sink) { sink.location(fix); });
        }
    }

    Env env_;
    std::shared_ptr<const Sink> sink_;
    // Serializes start(), stop() and close(); the session's thread never takes
    // it, and nothing under it calls the framework.
    mutable std::mutex control_;
    bool running_ = false;
    bool closed_ = false;
    // Set once the framework has been told this session ended, by the thread or
    // by stop(); whichever comes first does the telling.
    std::atomic<bool> end_reported_{false};
    int device_ = -1;
    int wake_read_ = -1;
    int wake_write_ = -1;
    std::thread thread_;
    // The parser and the pacer belong to the session's thread while it runs.
    nmea::Parser parser_;
    ReportPacer pacer_;
    std::atomic<int> interval_ms_{kDefaultIntervalMs};
    std::atomic<bool> nmea_on_{false};
    std::atomic<bool> sv_status_on_{false};
    // The wakelock and the last fix, which the thread and the entry points share.
    mutable std::mutex shared_;
    std::shared_ptr<void> wakelock_;
    std::optional<DebugFix> last_fix_;
    // Declared last: it ends first, so no call to the framework is made while
    // the rest of the engine is going away.
    Reporter reporter_;
};

}  // namespace andarium::gnss
