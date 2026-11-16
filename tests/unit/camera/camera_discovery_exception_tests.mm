// SPDX-License-Identifier: GPL-2.0-or-later
// A discovery that raises an Objective-C exception (an iPad app on a Mac meets one where the platform's own call is
// unavailable): the host presents no cameras, says so on one line, and the process lives, where a C++ handler would not
// have seen the exception and the process would have ended. The platform's raise is stood in by a replacement of the
// discovery session's device list for the length of the call; no camera is opened.
#import <AVFoundation/AVFoundation.h>
#import <objc/runtime.h>

#include "camera.hpp"
#include "require.hpp"

#include <cstdio>
#include <memory>
#include <string>
#include <unistd.h>

using namespace uml::platform;
using uml::testing::require;

namespace {

// Everything `body` writes to standard error.
template <class Body>
std::string standardErrorOf(Body&& body)
{
    std::fflush(stderr);
    const int saved = dup(STDERR_FILENO);
    std::FILE* file = std::tmpfile();
    require(saved >= 0 && file != nullptr, "a place to catch standard error");
    dup2(fileno(file), STDERR_FILENO);
    body();
    std::fflush(stderr);
    dup2(saved, STDERR_FILENO);
    close(saved);
    std::rewind(file);
    std::string text;
    char buffer[512];
    for (std::size_t count; (count = std::fread(buffer, 1, sizeof buffer, file)) > 0;) text.append(buffer, count);
    std::fclose(file);
    return text;
}

std::size_t linesOf(const std::string& text)
{
    std::size_t lines = 0;
    for (const char character : text) lines += character == '\n';
    return lines;
}

void aDiscoveryThatRaisesPresentsNoCameras()
{
    // The session a host makes is an instance of a class of its own (a subclass or a private variant), so the list is
    // replaced on the class the host's session has.
    AVCaptureDeviceDiscoverySession* sample =
        [AVCaptureDeviceDiscoverySession discoverySessionWithDeviceTypes:@[ AVCaptureDeviceTypeBuiltInWideAngleCamera ]
                                                               mediaType:AVMediaTypeVideo
                                                                position:AVCaptureDevicePositionUnspecified];
    Method method = class_getInstanceMethod(object_getClass(sample), @selector(devices));
    require(method != nullptr, "the discovery session lists devices");
    const IMP original = method_getImplementation(method);
    const IMP raising = imp_implementationWithBlock(^NSArray*(id) {
        [NSException raise:@"UMLForcedDiscoveryException" format:@"forced by the test"];
        return nil;
    });
    method_setImplementation(method, raising);
    std::unique_ptr<Camera> camera;
    const std::string raised = standardErrorOf([&] { camera = makeAppleCamera(); });
    method_setImplementation(method, original);
    imp_removeBlock(raising);

    require(camera != nullptr, "a host whose discovery raised still has a camera interface");
    require(linesOf(raised) == 1 &&
                raised.find("Camera: discovery raised UMLForcedDiscoveryException (forced by the test): "
                            "the host presents no cameras\n") == 0,
            "one line says why: " + raised);
    require(camera->cameras().empty() && camera->concurrentSets().empty(), "and it lists no cameras");

    // The replacement is gone: the next discovery is the host's own and raises nothing.
    const std::string quiet = standardErrorOf([&] { camera = makeAppleCamera(); });
    require(camera != nullptr && quiet.find("discovery raised") == std::string::npos, "the host's own discovery is untouched");
}

} // namespace

int main()
{
    aDiscoveryThatRaisesPresentsNoCameras();
    return 0;
}
