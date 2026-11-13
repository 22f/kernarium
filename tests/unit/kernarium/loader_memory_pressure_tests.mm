// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The last part of the generated source of kernarium-loader-memory-pressure:
 * the loader's file-local forwarders are exercised by compiling the loader's
 * own source into the same translation unit, between the head and this file
 * (cmake/concatenate_sources.cmake joins them; nothing includes an
 * implementation file). A fake runtime table stands in for the framework.
 *
 *   loader_memory_pressure_tests          what the loader delivers, and to whom
 *   loader_memory_pressure_tests --exit   a report forwarded after exit() has
 *                                         destroyed ordinary statics
 *   loader_memory_pressure_tests --table  which runtime tables the loader takes
 */
#include "KernariumMemoryPressure.hpp"

#include <cstdio>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <dispatch/dispatch.h>
#include <thread>
#include <utility>
#include <vector>

namespace {

kernarium::memory_pressure::Receiver receiver;
bool exitMode = false;

void fakeSet(kernarium_runtime_memory_pressure_callback callback, void* context)
{
    receiver.set(callback, context);
}

[[noreturn]] void fail(const char* message)
{
    std::fprintf(stderr, "loader memory pressure test failed: %s\n", message);
    std::fflush(stderr);
    std::_Exit(1);
}

void check(bool condition, const char* message)
{
    if (!condition)
        fail(message);
}

/* The runtime's thread, as it makes the reports, while the main queue is
 * blocked by the caller: they queue. */
void publishFromRuntimeThread(int count, uml::runtime::PressureLevel level)
{
    std::thread runtime([count, level] {
        for (int index = 0; index < count; ++index)
            receiver.publish({level, uml::runtime::PressureReaction::responding});
    });
    runtime.join();
}

void deliveryTests()
{
    __block std::vector<std::pair<int, int>> heard;
    __block bool onMainQueue = true;
    kernarium_runtime_loader_set_memory_pressure_handler(^(int level, int response) {
        onMainQueue = onMainQueue && NSThread.isMainThread;
        heard.emplace_back(level, response);
    });

    /* The reports reach the handler on the main queue, in order, and none
     * before the queue runs. */
    receiver.publish({uml::runtime::PressureLevel::warn, uml::runtime::PressureReaction::responding});
    receiver.publish({uml::runtime::PressureLevel::warn, uml::runtime::PressureReaction::withheld});
    receiver.publish({uml::runtime::PressureLevel::critical, uml::runtime::PressureReaction::responding});
    receiver.publish({uml::runtime::PressureLevel::normal, uml::runtime::PressureReaction::none});
    check(heard.empty(), "a report reached the handler before the main queue ran");

    dispatch_async(dispatch_get_main_queue(), ^{
        const std::vector<std::pair<int, int>> expected {
            {KERNARIUM_RUNTIME_MEMORY_PRESSURE_WARNING, KERNARIUM_RUNTIME_MEMORY_RESPONSE_STARTED},
            {KERNARIUM_RUNTIME_MEMORY_PRESSURE_WARNING, KERNARIUM_RUNTIME_MEMORY_RESPONSE_WITHHELD},
            {KERNARIUM_RUNTIME_MEMORY_PRESSURE_CRITICAL, KERNARIUM_RUNTIME_MEMORY_RESPONSE_STARTED},
            {KERNARIUM_RUNTIME_MEMORY_PRESSURE_NORMAL, KERNARIUM_RUNTIME_MEMORY_RESPONSE_NONE},
        };
        check(heard == expected, "the reports did not arrive in order, once each");
        check(onMainQueue, "a report reached the handler off the main queue");

        /* One queued block per report, and clearing or replacing the handler
         * does not cancel them: while the main queue is busy (here, this
         * block), a thousand reports queue and all reach the handler they were
         * made for. */
        __block int oldHeard = 0;
        __block int newHeard = 0;
        kernarium_runtime_loader_set_memory_pressure_handler(^(int, int) { ++oldHeard; });
        constexpr int queued = 1000;
        publishFromRuntimeThread(queued, uml::runtime::PressureLevel::warn);
        kernarium_runtime_loader_set_memory_pressure_handler(nil);
        kernarium_runtime_loader_set_memory_pressure_handler(^(int, int) { ++newHeard; });
        check(oldHeard == 0, "a queued report ran while the main queue was busy");
        dispatch_async(dispatch_get_main_queue(), ^{
            check(oldHeard == queued, "a queued report was lost or doubled by the replacement");
            check(newHeard == 0, "a report queued for the old handler reached the new one");
            publishFromRuntimeThread(5, uml::runtime::PressureLevel::critical);
            dispatch_async(dispatch_get_main_queue(), ^{
                check(newHeard == 5, "a report after the replacement missed the new handler");
                check(oldHeard == queued, "a report after the replacement reached the old handler");
                kernarium_runtime_loader_set_memory_pressure_handler(nil);
                receiver.publish({uml::runtime::PressureLevel::critical, uml::runtime::PressureReaction::responding});
                std::puts("loader memory pressure: delivery ok");
                std::fflush(stdout);
                std::_Exit(0);
            });
        });
    });
}

void unusedEntry() {}

/* A table of the given version with every entry set. */
kernarium_runtime_api fullTable(uint32_t version)
{
    kernarium_runtime_api api {};
    api.abi_version = version;
    api.struct_size = sizeof(api);
    api.guest_page_size = 4096U;
    api.guest_flavor = KERNARIUM_RUNTIME_GUEST_LINUX;
    void (*entry)() = unusedEntry;
    constexpr std::size_t first = offsetof(kernarium_runtime_api, run);
    check((sizeof(api) - first) % sizeof(entry) == 0, "the table has an entry that is no function pointer");
    for (std::size_t offset = first; offset < sizeof(api); offset += sizeof(entry))
        std::memcpy(reinterpret_cast<unsigned char*>(&api) + offset, &entry, sizeof(entry));
    return api;
}

/* The loader takes the table of its own version and refuses one of any other,
 * a table too short for the entries it reads, and one that lacks an entry
 * added since version 39. */
void tableTests()
{
    kernarium_runtime_api api = fullTable(KERNARIUM_RUNTIME_API_VERSION);
    check(validApi(&api, KERNARIUM_RUNTIME_GUEST_LINUX), "the table of the loader's version was refused");
    check(!validApi(&api, KERNARIUM_RUNTIME_GUEST_ANDROID), "a table of another guest flavor was taken");
    check(!validApi(nullptr, KERNARIUM_RUNTIME_GUEST_LINUX), "a null table was taken");

    for (const uint32_t version : {39U, 40U, KERNARIUM_RUNTIME_API_VERSION + 1U}) {
        api = fullTable(version);
        check(!validApi(&api, KERNARIUM_RUNTIME_GUEST_LINUX), "a table of another version was taken");
    }

    api = fullTable(KERNARIUM_RUNTIME_API_VERSION);
    api.struct_size = offsetof(kernarium_runtime_api, set_memory_pressure_callback);
    check(!validApi(&api, KERNARIUM_RUNTIME_GUEST_LINUX), "a table without the last entry was taken");

    api = fullTable(KERNARIUM_RUNTIME_API_VERSION);
    api.plan_camera_reserve = nullptr;
    check(!validApi(&api, KERNARIUM_RUNTIME_GUEST_LINUX), "a table without plan_camera_reserve was taken");

    api = fullTable(KERNARIUM_RUNTIME_API_VERSION);
    api.set_memory_pressure_callback = nullptr;
    check(!validApi(&api, KERNARIUM_RUNTIME_GUEST_LINUX), "a table without set_memory_pressure_callback was taken");

    std::puts("loader table: ok");
    std::fflush(stdout);
}

} // namespace

/* Registered first by the head: runs after the loader's statics were
 * destroyed, when the pressure queue may still call into the loader. */
extern "C" void loaderTestAfterStaticDestruction(void)
{
    if (!exitMode)
        return;
    std::fputs("forwarding a report after static destruction\n", stdout);
    std::fflush(stdout);
    forwardMemoryPressure(
        nullptr,
        KERNARIUM_RUNTIME_MEMORY_PRESSURE_WARNING,
        KERNARIUM_RUNTIME_MEMORY_RESPONSE_STARTED);
    std::fputs("loader memory pressure: exit ok\n", stdout);
    std::fflush(stdout);
    std::_Exit(0);
}

int main(int argc, char** argv)
{
    @autoreleasepool {
        if (argc > 1 && std::strcmp(argv[1], "--table") == 0) {
            tableTests();
            return 0;
        }
        static kernarium_runtime_api api {};
        api.set_memory_pressure_callback = fakeSet;
        selectedApi.store(&api);
        if (argc > 1 && std::strcmp(argv[1], "--exit") == 0) {
            exitMode = true;
            int captured = 42;
            kernarium_runtime_loader_set_memory_pressure_handler(^(int, int) { (void)captured; });
            /* Exits with the handler set: the status is the handler's, 0, and
             * 2 if it never ran. */
            std::exit(2);
        }
        deliveryTests();
    }
    dispatch_main();
}
