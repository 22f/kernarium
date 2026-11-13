// SPDX-License-Identifier: GPL-2.0-or-later
// The key-value observer the camera's pressure watch uses (apple/key_path_observer.h), on a stand-in object: no camera
// is opened. Key-value observing runs the handler on the thread that changed the value and does not wait for it in
// removeObserver:, so a session that stops while the handler runs must not free the block the handler is running from.
#import "key_path_observer.h"

#include "require.hpp"

#include <atomic>
#include <dispatch/dispatch.h>
#include <thread>

using uml::testing::require;

@interface StandIn : NSObject
@property(atomic) NSNumber* value;
@end
@implementation StandIn
@end

namespace {

// What the handler under test touches after it has been paused is file-scope state, never its own captures: a handler
// that is run from a freed block must fail the test, not read freed memory.
std::atomic<bool> destroyed {false};
std::atomic<bool> sawDestroyed {false};
std::atomic<int> handled {0};
std::atomic<int> lastValue {0};
dispatch_semaphore_t entered, proceed, finished;

struct Sentinel {
    ~Sentinel() { destroyed = true; }
};

void reset()
{
    destroyed = false;
    sawDestroyed = false;
    handled = 0;
    lastValue = 0;
    entered = dispatch_semaphore_create(0);
    proceed = dispatch_semaphore_create(0);
    finished = dispatch_semaphore_create(0);
}

bool wait(dispatch_semaphore_t semaphore)
{
    return dispatch_semaphore_wait(semaphore, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)) == 0;
}

// R1 (recheck6): the notification is paused inside the handler, the stop's hop invalidates the observer and drops
// the session's reference, and the handler goes on. The block it runs from lives until it returns (the reviewer's
// r6_observer under AddressSanitizer reported a heap use after free here).
void aStopDuringTheHandlerDoesNotFreeTheHandlerItRuns()
{
    reset();
    StandIn* subject = [StandIn new];
    UMLKeyPathObserver* observer;
    {
        auto sentinel = std::make_shared<Sentinel>();
        observer = [[UMLKeyPathObserver alloc] initWithObject:subject
                                                      keyPath:@"value"
                                                      handler:^(id) {
            (void)sentinel;
            dispatch_semaphore_signal(entered);
            if (!wait(proceed)) std::abort();
            sawDestroyed = destroyed.load();
            ++handled;
            dispatch_semaphore_signal(finished);
        }];
    }
    require(!destroyed, "the observer owns the handler");
    std::thread notifier([&] { subject.value = @1; });
    require(wait(entered), "the handler started");
    [observer invalidate];
    observer = nil;
    require(!destroyed, "the handler that is running is still alive after the stop and the release");
    dispatch_semaphore_signal(proceed);
    require(wait(finished), "the handler finished");
    notifier.join();
    require(handled == 1 && !sawDestroyed, "it ran to its end from a live block");
    require(destroyed, "and the block is freed once the handler has returned");
}

void theHandlerGetsTheNewValueAndNothingAfterInvalidate()
{
    reset();
    StandIn* subject = [StandIn new];
    UMLKeyPathObserver* observer = [[UMLKeyPathObserver alloc] initWithObject:subject
                                                                      keyPath:@"value"
                                                                      handler:^(id value) {
        lastValue = [static_cast<NSNumber*>(value) intValue];
        ++handled;
    }];
    subject.value = @7;
    require(handled == 1 && lastValue == 7, "the handler gets the new value");
    [observer invalidate];
    subject.value = @8;
    require(handled == 1 && lastValue == 7, "and nothing once the observer is invalidated");
    [observer invalidate];
    require(subject.value.intValue == 8, "a second invalidate is harmless");
}

void aHandlerMayInvalidateItsOwnObserver()
{
    reset();
    StandIn* subject = [StandIn new];
    __block UMLKeyPathObserver* observer = nil;
    observer = [[UMLKeyPathObserver alloc] initWithObject:subject keyPath:@"value" handler:^(id) {
        ++handled;
        [observer invalidate];
    }];
    // A handler that is called with a lock held would deadlock here.
    std::thread notifier([&] {
        subject.value = @1;
        subject.value = @2;
    });
    notifier.join();
    require(handled == 1, "the handler ran once and invalidated itself");
    observer = nil;
}

void aReleasedObserverIsRemoved()
{
    reset();
    StandIn* subject = [StandIn new];
    @autoreleasepool {
        UMLKeyPathObserver* observer = [[UMLKeyPathObserver alloc] initWithObject:subject
                                                                          keyPath:@"value"
                                                                          handler:^(id) { ++handled; }];
        subject.value = @1;
        require(handled == 1, "observing");
        (void)observer;
    }
    subject.value = @2;
    require(handled == 1, "an observer that is released stops observing (no handler, no crash)");
}

// Many changes race a stop: no handler starts after invalidate returns.
void noHandlerStartsAfterInvalidateReturns()
{
    for (int round = 0; round < 50; ++round) {
        reset();
        StandIn* subject = [StandIn new];
        UMLKeyPathObserver* observer = [[UMLKeyPathObserver alloc] initWithObject:subject
                                                                          keyPath:@"value"
                                                                          handler:^(id) { ++handled; }];
        std::atomic<bool> go {false};
        std::thread writer([&] {
            while (!go) std::this_thread::yield();
            for (int index = 0; index < 200; ++index) subject.value = @(index);
        });
        go = true;
        std::this_thread::yield();
        [observer invalidate];
        const int atInvalidate = handled;
        writer.join();
        require(handled == atInvalidate, "no handler starts once invalidate has returned");
    }
}

} // namespace

int main()
{
    @autoreleasepool {
        aStopDuringTheHandlerDoesNotFreeTheHandlerItRuns();
        theHandlerGetsTheNewValueAndNothingAfterInvalidate();
        aHandlerMayInvalidateItsOwnObserver();
        aReleasedObserverIsRemoved();
        noHandlerStartsAfterInvalidateReturns();
    }
    return 0;
}
