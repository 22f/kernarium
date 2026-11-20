// SPDX-License-Identifier: GPL-2.0-or-later

#include "native_display.hpp"

#include "native_display_state.hpp"
#include "present_pacer.hpp"

#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>
#import <UIKit/UIKit.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>
#ifndef UML_APP_STORE_RUNTIME
#include <cstdio>
#include <cstdlib>
#endif

namespace uml::gpu {
namespace {

NSString* const resizeWillBeginNotification =
    @"org.linux-um-darwin.guest-display-resize-will-begin";
NSString* const resizeDidEndNotification =
    @"org.linux-um-darwin.guest-display-resize-did-end";
constexpr std::int64_t resizeSettleNanoseconds =
    150LL * NSEC_PER_MSEC;

/* The window scene a view's display link belongs to: the view's own, or the
 * application's active (else first) one when the view is not in a window yet.
 * Main thread. */
[[nodiscard]] UIWindowScene* displaySceneFor(UIView* view) noexcept
{
    if (view.window.windowScene != nil)
        return view.window.windowScene;
    UIWindowScene* first = nil;
    for (UIScene* scene in UIApplication.sharedApplication.connectedScenes) {
        if (![scene isKindOfClass:UIWindowScene.class])
            continue;
        if (scene.activationState == UISceneActivationStateForegroundActive)
            return (UIWindowScene*)scene;
        if (first == nil)
            first = (UIWindowScene*)scene;
    }
    return first;
}

#ifndef UML_APP_STORE_RUNTIME
/* The display link's own report, on the frame statistics switch of the other
 * display reports (metal_presentation.mm): what rate it asked for and what iOS
 * gave. */
[[nodiscard]] bool linkReportsEnabled() noexcept
{
    static const bool enabled = std::getenv("UML_GPU_FRAME_STATS") != nullptr;
    return enabled;
}
#endif

} // namespace

struct NativeDisplayState final :
    public NativeDisplayStateCore,
    public std::enable_shared_from_this<NativeDisplayState> {
    NativeDisplayState(
        std::uint32_t width,
        std::uint32_t height,
        std::uint32_t maximumDimension,
        bool autoResizeGuest,
        std::uint32_t refreshRate)
        : NativeDisplayStateCore(
              width, height, maximumDimension, autoResizeGuest)
        , modeRate(refreshRate)
    {
    }

    [[nodiscard]] std::uint32_t refreshRate() const noexcept override
    {
        return modeRate;
    }

    void attachView(void* nativeView);
    void scheduleResize(CGSize size) noexcept;
    void beginResize() noexcept;
    void commitResize(CGSize size) noexcept;
    [[nodiscard]] bool guestScanoutChanged(
        std::uint32_t width,
        std::uint32_t height) noexcept override;
    void resetGuestSession() noexcept override;
    void publishGuestSize(DisplaySize size) noexcept;
    void requestRedraw() noexcept;
    void shutdown() noexcept override;
    /* The display link of a 120 Hz mode (modeRate above 60): it paces the
     * presents, runs only while frames arrive and asks iOS for the rate the
     * guest renders at (present_pacer.hpp). All of it runs on the main
     * thread but requestPacedRedraw, which any thread may call. */
    void requestPacedRedraw() noexcept;
    void wakeLink() noexcept;
    void linkTick(CADisplayLink* ticking) noexcept;
    void applyLinkRange(LinkFrameRate range) noexcept;
    [[nodiscard]] double linkCeiling(UIWindowScene* scene) const noexcept;
    [[nodiscard]] std::uint64_t guestFramesOffered() const noexcept;
    void stopLink() noexcept;
#ifndef UML_APP_STORE_RUNTIME
    void reportLink(double now) noexcept;
#endif

    /* The rate of the one mode the guest sees: 60, or 120 when the screen
     * can show it (NativeDisplay::supportedRefreshRate). */
    const std::uint32_t modeRate;
    /* A frame, or a change of what is shown, that no tick has presented yet,
     * and whether the link is running: the two flags every thread shares. */
    std::atomic_bool frameWaiting {};
    std::atomic_bool linkAwake {};
    std::atomic_bool wakeScheduled {};
    /* Main thread only. */
    CADisplayLink* link;
    id linkTarget;
    __weak UIWindowScene* linkScene;
    PresentPacer pacer;
    LinkFrameRate requestedRange {};
#ifndef UML_APP_STORE_RUNTIME
    double reportStart {};
    double grantedSum {};
    std::uint64_t reportTicks {};
    std::uint64_t reportDraws {};
    std::uint64_t reportWakes {};
    std::uint64_t reportSleeps {};
#endif

    id<MTKViewDelegate> delegate;
    id resizeWillBeginObserver;
    id resizeDidEndObserver;
    __weak MTKView* view;
    std::atomic_bool running {true};
    std::atomic_bool redrawScheduled {};
    std::atomic_uint64_t resizeGeneration {};
    DisplaySize latestSettledHostSize;
};

} // namespace uml::gpu

@interface UmlIOSMetalViewDelegate : NSObject <MTKViewDelegate>
- (instancetype)initWithState:
    (std::weak_ptr<uml::gpu::NativeDisplayState>)state;
@end

@interface UmlIOSDisplayLinkTarget : NSObject
- (instancetype)initWithState:
    (std::weak_ptr<uml::gpu::NativeDisplayState>)state;
- (void)tick:(CADisplayLink*)ticking;
@end

@protocol UmlIOSGuestResizeLifecycle <NSObject>
@property(nonatomic, readonly) BOOL umlGuestResizeInProgress;
@end

@protocol UmlIOSGuestDisplayGeometry <NSObject>
- (void)umlSetGuestDisplayWidth:(uint32_t)width height:(uint32_t)height;
@end

@implementation UmlIOSDisplayLinkTarget {
    std::weak_ptr<uml::gpu::NativeDisplayState> _state;
}

- (instancetype)initWithState:
    (std::weak_ptr<uml::gpu::NativeDisplayState>)state
{
    self = [super init];
    if (self != nil)
        _state = std::move(state);
    return self;
}

- (void)tick:(CADisplayLink*)ticking
{
    if (auto state = _state.lock())
        state->linkTick(ticking);
}

@end

@implementation UmlIOSMetalViewDelegate {
    std::weak_ptr<uml::gpu::NativeDisplayState> _state;
}

- (instancetype)initWithState:
    (std::weak_ptr<uml::gpu::NativeDisplayState>)state
{
    self = [super init];
    if (self != nil)
        _state = std::move(state);
    return self;
}

- (void)mtkView:(MTKView*)view drawableSizeWillChange:(CGSize)size
{
    (void)view;
    if (auto state = _state.lock()) {
        state->presentation.drawableSizeChanged();
        state->scheduleResize(size);
    }
}

- (void)drawInMTKView:(MTKView*)view
{
    if (auto state = _state.lock())
        state->presentation.draw((__bridge void*)view);
}

@end

namespace uml::gpu {

void NativeDisplayState::attachView(void* nativeView)
{
    if (nativeView == nullptr)
        throw std::invalid_argument("the iOS native display needs an MTKView");
    MTKView* metalView = (__bridge MTKView*)nativeView;
    if (![metalView isKindOfClass:MTKView.class])
        throw std::invalid_argument("the iOS native display view is not MTKView");
    auto state = shared_from_this();
    presentation.setRedrawCallback([weak = weak_from_this()] {
        if (auto locked = weak.lock())
            locked->requestRedraw();
    });
    const auto configure = ^{
        metalView.device = (__bridge id<MTLDevice>)state->presentation.device();
        metalView.colorPixelFormat = MTLPixelFormatBGRA8Unorm;
        metalView.clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 1.0);
        metalView.framebufferOnly = YES;
        metalView.enableSetNeedsDisplay = YES;
        metalView.paused = YES;
        metalView.autoResizeDrawable = NO;
        UmlIOSMetalViewDelegate* viewDelegate =
            [[UmlIOSMetalViewDelegate alloc] initWithState:state];
        state->view = metalView;
        state->delegate = viewDelegate;
        metalView.delegate = viewDelegate;
        const auto guestSize = state->geometryPolicy.activeGuestSize();
        if ([metalView respondsToSelector:
                @selector(umlSetGuestDisplayWidth:height:)]) {
            [(id<UmlIOSGuestDisplayGeometry>)metalView
                umlSetGuestDisplayWidth:guestSize.width
                height:guestSize.height];
        }
        NSNotificationCenter* center = NSNotificationCenter.defaultCenter;
        auto weak = state->weak_from_this();
        state->resizeWillBeginObserver = [center
            addObserverForName:resizeWillBeginNotification
                        object:metalView
                         queue:NSOperationQueue.mainQueue
                    usingBlock:^(NSNotification*) {
                        if (auto locked = weak.lock())
                            locked->beginResize();
                    }];
        state->resizeDidEndObserver = [center
            addObserverForName:resizeDidEndNotification
                        object:metalView
                         queue:NSOperationQueue.mainQueue
                    usingBlock:^(NSNotification*) {
                        if (auto locked = weak.lock())
                            locked->commitResize(metalView.drawableSize);
                    }];
        const bool transitionActive =
            [metalView respondsToSelector:@selector(umlGuestResizeInProgress)] &&
            [(id<UmlIOSGuestResizeLifecycle>)metalView
                umlGuestResizeInProgress];
        if (transitionActive)
            state->beginResize();
        else
            state->commitResize(metalView.drawableSize);
        [metalView draw];
    };
    if ([NSThread isMainThread])
        configure();
    else
        dispatch_sync(dispatch_get_main_queue(), configure);
}

void NativeDisplayState::beginResize() noexcept
{
    std::lock_guard lock(resizeLifecycleMutex);
    resizeInProgress.store(true, std::memory_order_release);
    resizeGeneration.fetch_add(1, std::memory_order_acq_rel);
}

void NativeDisplayState::commitResize(CGSize size) noexcept
{
    std::lock_guard lock(resizeLifecycleMutex);
    resizeGeneration.fetch_add(1, std::memory_order_acq_rel);
    const auto width = displayDimension(
        size.width, resizeNotifier.maximumDimension());
    const auto height = displayDimension(
        size.height, resizeNotifier.maximumDimension());
    if (width && height &&
        running.load(std::memory_order_acquire)) {
        latestSettledHostSize = {.width = *width, .height = *height};
        resizeNotifier.requestCommitted(*width, *height);
        resizeCommitPending.store(true, std::memory_order_release);
    }
    resizeInProgress.store(false, std::memory_order_release);
}

void NativeDisplayState::scheduleResize(CGSize size) noexcept
{
    const auto width = displayDimension(
        size.width, resizeNotifier.maximumDimension());
    const auto height = displayDimension(
        size.height, resizeNotifier.maximumDimension());
    if (!width || !height)
        return;
    std::uint64_t generation;
    {
        std::lock_guard lock(resizeLifecycleMutex);
        if (resizeInProgress.load(std::memory_order_acquire))
            return;
        generation =
            resizeGeneration.fetch_add(1, std::memory_order_acq_rel) + 1U;
    }
    auto weak = weak_from_this();
    dispatch_after(
        dispatch_time(DISPATCH_TIME_NOW, resizeSettleNanoseconds),
        dispatch_get_main_queue(),
        ^{
            if (auto state = weak.lock()) {
                std::lock_guard lock(state->resizeLifecycleMutex);
                if (state->running.load(std::memory_order_acquire) &&
                    !state->resizeInProgress.load(std::memory_order_acquire) &&
                    state->resizeGeneration.load(std::memory_order_acquire) ==
                        generation) {
                    state->latestSettledHostSize = {
                        .width = *width,
                        .height = *height,
                    };
                    state->resizeNotifier.request(*width, *height);
                }
            }
        });
}

void NativeDisplayState::publishGuestSize(DisplaySize size) noexcept
{
    auto weak = weak_from_this();
    dispatch_async(dispatch_get_main_queue(), ^{
        if (auto state = weak.lock()) {
            if (!state->running.load(std::memory_order_acquire))
                return;
            MTKView* nativeView = state->view;
            if ([nativeView respondsToSelector:
                    @selector(umlSetGuestDisplayWidth:height:)]) {
                [(id<UmlIOSGuestDisplayGeometry>)nativeView
                    umlSetGuestDisplayWidth:size.width
                    height:size.height];
            }
        }
    });
}

bool NativeDisplayState::guestScanoutChanged(
    std::uint32_t width,
    std::uint32_t height) noexcept
{
    DisplaySize guestSize;
    bool changed = false;
    bool publishFrame = false;
    {
        std::lock_guard lock(resizeLifecycleMutex);
        const auto& framebuffer = presentation.framebuffer();
        const bool presentationChanged =
            framebuffer.width() != width || framebuffer.height() != height;
        if (presentationChanged) {
            if (!presentation.resize(width, height, false))
                return false;
            publishFrame = true;
        }
        changed = geometryPolicy.guestScanoutChanged(width, height);
        guestSize = geometryPolicy.activeGuestSize();
    }
    if (changed)
        publishGuestSize(guestSize);
    if (changed || publishFrame) {
        /* The UIKit coordinate update above reaches the main queue before the
         * redraw requested here can expose the new aspect ratio. */
        presentation.publishCpuFrame();
    }
    return true;
}

void NativeDisplayState::resetGuestSession() noexcept
{
    DisplaySize guestSize;
    {
        std::lock_guard lock(resizeLifecycleMutex);
        /* Discard any pre-reset readiness byte before restoring the newest
         * settled host geometry below. Otherwise the transport could later
         * consume a stale size and advertise it as a post-reset resize. */
        (void)resizeNotifier.drain();
        if (latestSettledHostSize.width != 0 &&
            latestSettledHostSize.height != 0) {
            (void)geometryPolicy.hostSizeSettled(
                latestSettledHostSize.width,
                latestSettledHostSize.height);
        }
        const auto restored = geometryPolicy.settledHostSize();
        const auto& framebuffer = presentation.framebuffer();
        const DisplaySize presented {
            .width = framebuffer.width(),
            .height = framebuffer.height(),
        };
        const bool presentationReady = presented == restored ||
            presentation.resize(restored.width, restored.height, false);
        geometryPolicy.resetGuestSession(
            presentationReady ? restored : presented);
        guestSize = geometryPolicy.activeGuestSize();
        resizeGeneration.fetch_add(1, std::memory_order_acq_rel);
        resizeCommitPending.store(false, std::memory_order_release);
    }
    publishGuestSize(guestSize);
}

void NativeDisplayState::requestRedraw() noexcept
{
    if (modeRate > 60U) {
        requestPacedRedraw();
        return;
    }
    if (!running.load(std::memory_order_acquire) ||
        redrawScheduled.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    auto state = shared_from_this();
    dispatch_async(dispatch_get_main_queue(), ^{
        state->redrawScheduled.store(false, std::memory_order_release);
        if (!state->running.load(std::memory_order_acquire))
            return;
        MTKView* nativeView = state->view;
        if (nativeView != nil)
            [nativeView draw];
    });
}

void NativeDisplayState::requestPacedRedraw() noexcept
{
    if (!running.load(std::memory_order_acquire))
        return;
    /* A running link presents it at its next tick, with no work on the main
     * queue; a sleeping one is woken there, once however many frames arrive
     * before it runs. The two flags are sequentially consistent so that a
     * frame never falls between a tick that decides to sleep and this check:
     * the tick looks again after it stops the link. */
    frameWaiting.store(true, std::memory_order_seq_cst);
    if (linkAwake.load(std::memory_order_seq_cst) ||
        wakeScheduled.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    auto state = shared_from_this();
    dispatch_async(dispatch_get_main_queue(), ^{
        state->wakeScheduled.store(false, std::memory_order_release);
        state->wakeLink();
    });
}

std::uint64_t NativeDisplayState::guestFramesOffered() const noexcept
{
    const DisplayStatistics counts = presentation.statistics();
    return counts.guestFrames + counts.cpuFallbackFrames;
}

double NativeDisplayState::linkCeiling(UIWindowScene* scene) const noexcept
{
    /* The fastest rate the screen can show now. Low Power Mode holds an
     * iPhone to 60 whatever the screen says, and the guest is told to render
     * no faster (the application's thermal reports). */
    double ceiling = static_cast<double>(modeRate);
    const NSInteger maximum = scene.screen.maximumFramesPerSecond;
    if (maximum > 0)
        ceiling = std::min(ceiling, static_cast<double>(maximum));
    if (NSProcessInfo.processInfo.lowPowerModeEnabled)
        ceiling = std::min(ceiling, 60.0);
    return ceiling;
}

void NativeDisplayState::applyLinkRange(LinkFrameRate range) noexcept
{
    if (range == requestedRange)
        return;
    requestedRange = range;
    link.preferredFrameRateRange =
        CAFrameRateRangeMake(range.minimum, range.maximum, range.preferred);
}

void NativeDisplayState::wakeLink() noexcept
{
    if (!running.load(std::memory_order_acquire) ||
        linkAwake.load(std::memory_order_seq_cst)) {
        return;
    }
    MTKView* nativeView = view;
    if (nativeView == nil)
        return;
    /* The link belongs to the scene the view is in: a view that moved to
     * another scene, and so another screen, is followed when the link next
     * wakes. */
    UIWindowScene* scene = displaySceneFor(nativeView);
    if (scene == nil)
        return;
    if (link == nil || linkScene != scene) {
        [link invalidate];
        if (linkTarget == nil) {
            linkTarget = [[UmlIOSDisplayLinkTarget alloc]
                initWithState:weak_from_this()];
        }
        link = [scene displayLinkWithTarget:linkTarget
                                   selector:@selector(tick:)];
        if (link == nil)
            return;
        link.paused = YES;
        [link addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
        linkScene = scene;
        requestedRange = {};
    }
    applyLinkRange(pacer.wake(
        CACurrentMediaTime(), guestFramesOffered(), linkCeiling(scene)));
    linkAwake.store(true, std::memory_order_seq_cst);
    link.paused = NO;
#ifndef UML_APP_STORE_RUNTIME
    ++reportWakes;
#endif
}

void NativeDisplayState::linkTick(CADisplayLink* ticking) noexcept
{
    if (!running.load(std::memory_order_acquire))
        return;
    /* A frame that arrives from here on is a frame for the next tick, so the
     * flag is taken before the decision is made. */
    const bool waiting = frameWaiting.exchange(false, std::memory_order_seq_cst);
    const double now = ticking.timestamp;
    const PresentPacer::Decision decision = pacer.tick(
        now, guestFramesOffered(), waiting, linkCeiling(linkScene));
    if (decision.range)
        applyLinkRange(*decision.range);
    if (decision.present) {
        MTKView* nativeView = view;
        if (nativeView != nil)
            [nativeView draw];
#ifndef UML_APP_STORE_RUNTIME
        ++reportDraws;
#endif
    }
#ifndef UML_APP_STORE_RUNTIME
    ++reportTicks;
    grantedSum += ticking.targetTimestamp - ticking.timestamp;
    reportLink(now);
#endif
    if (!decision.sleep)
        return;
    linkAwake.store(false, std::memory_order_seq_cst);
    if (frameWaiting.load(std::memory_order_seq_cst)) {
        /* A frame arrived while the link was deciding, and its request saw a
         * running link: the link stays and presents it at the next tick. */
        linkAwake.store(true, std::memory_order_seq_cst);
        return;
    }
    ticking.paused = YES;
#ifndef UML_APP_STORE_RUNTIME
    ++reportSleeps;
#endif
}

void NativeDisplayState::stopLink() noexcept
{
    /* Main thread. */
    linkAwake.store(false, std::memory_order_seq_cst);
    [link invalidate];
    link = nil;
    linkTarget = nil;
    linkScene = nil;
}

#ifndef UML_APP_STORE_RUNTIME
void NativeDisplayState::reportLink(double now) noexcept
{
    if (!linkReportsEnabled())
        return;
    if (reportStart == 0.0) {
        reportStart = now;
        return;
    }
    const double span = now - reportStart;
    if (span < 1.0)
        return;
    std::fprintf(
        stderr,
        "Display link: asked %.0f-%.0f (preferred %.0f), ticks %.1f/s at "
        "%.2f ms, draws %.1f/s, wakes %llu, sleeps %llu\n",
        static_cast<double>(requestedRange.minimum),
        static_cast<double>(requestedRange.maximum),
        static_cast<double>(requestedRange.preferred),
        static_cast<double>(reportTicks) / span,
        reportTicks != 0 ? grantedSum * 1000.0 / static_cast<double>(reportTicks)
                         : 0.0,
        static_cast<double>(reportDraws) / span,
        static_cast<unsigned long long>(reportWakes),
        static_cast<unsigned long long>(reportSleeps));
    reportStart = now;
    reportTicks = 0;
    reportDraws = 0;
    reportWakes = 0;
    reportSleeps = 0;
    grantedSum = 0.0;
}
#endif

void NativeDisplayState::shutdown() noexcept
{
    if (!running.exchange(false, std::memory_order_acq_rel))
        return;
    {
        std::lock_guard lock(resizeLifecycleMutex);
        resizeInProgress.store(true, std::memory_order_release);
        resizeGeneration.fetch_add(1, std::memory_order_acq_rel);
    }
    presentation.shutdown();
    auto state = shared_from_this();
    dispatch_async(dispatch_get_main_queue(), ^{
        state->stopLink();
        MTKView* nativeView = state->view;
        if (nativeView.delegate == state->delegate)
            nativeView.delegate = nil;
        NSNotificationCenter* center = NSNotificationCenter.defaultCenter;
        if (state->resizeWillBeginObserver != nil)
            [center removeObserver:state->resizeWillBeginObserver];
        if (state->resizeDidEndObserver != nil)
            [center removeObserver:state->resizeDidEndObserver];
        state->view = nil;
        state->delegate = nil;
        state->resizeWillBeginObserver = nil;
        state->resizeDidEndObserver = nil;
    });
}

std::uint32_t NativeDisplay::supportedRefreshRate(
    std::uint32_t requested) noexcept
{
    if (requested != 120U)
        return 60U;
    __block NSInteger maximum = 60;
    const auto read = ^{
        maximum = displaySceneFor(nil).screen.maximumFramesPerSecond;
    };
    if ([NSThread isMainThread])
        read();
    else
        dispatch_sync(dispatch_get_main_queue(), read);
    return maximum > 60 ? 120U : 60U;
}

NativeDisplay::NativeDisplay(
    std::uint32_t width,
    std::uint32_t height,
    uml::input::vnc::VncInputSink,
    void* nativeView,
    uml::input::vnc::PointerMode,
    std::uint32_t maximumDimension,
    bool autoResizeGuest,
    std::uint32_t refreshRate)
{
    auto state = std::make_shared<NativeDisplayState>(
        width,
        height,
        maximumDimension,
        autoResizeGuest,
        supportedRefreshRate(refreshRate));
    state->attachView(nativeView);
    state_ = std::move(state);
}

} // namespace uml::gpu
