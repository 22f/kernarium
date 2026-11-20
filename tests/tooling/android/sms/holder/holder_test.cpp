// SPDX-License-Identifier: GPL-2.0-or-later
//
// Drives the text message holder (android/lineage-24/overlay/sms/listener/main.cpp), compiled as it is with its main
// renamed holder_main, on the host. test_sms_holder.py builds and runs it. The holder's world is the one init gives
// it and the platform's calls it makes, stood in for here:
//   - the handoff socket (/dev/socket/kernarium_sms) is what init makes: a unix stream socket that is BOUND and NOT
//     listening (init does not listen on a stream socket; the daemon does), handed over by android_get_control_socket;
//   - the vsock socket of port 5560 is a unix stream socket at a path of the test (socket, bind and getsockopt are
//     interposed for it and for the peer's credentials; listen, poll, accept, sendmsg and recv are the host's own);
//   - the properties it sets, the peer's user and SELinux context, and its log lines are the test's to read and set.
// The scenarios run one after the other against one holder thread; the first one that does not hold ends the test.

#include <dlfcn.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

int holder_main();  // main.cpp's main, renamed by -Dmain=holder_main

namespace {

std::mutex gLock;
std::vector<std::string> gProperties;
std::string gLastError;
std::atomic<int> gVsockFd{-1};
std::atomic<int> gHandoffFd{-1};
std::atomic<unsigned> gPeerUid{1000};
std::string gPeerContext = "u:r:kernarium_sms:s0";
std::string gVsockPath;
std::atomic<bool> gReturned{false};
std::atomic<int> gStatus{-1};

template <typename Function>
Function realFunction(const char* name) {
    return reinterpret_cast<Function>(dlsym(RTLD_NEXT, name));
}

long long nowMillis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

template <typename Condition>
bool until(int millis, Condition condition) {
    const long long end = nowMillis() + millis;
    for (;;) {
        if (condition()) return true;
        if (nowMillis() >= end) return false;
        usleep(10 * 1000);
    }
}

[[noreturn]] void fail(const char* scenario, const std::string& what) {
    std::string tail;
    {
        std::lock_guard<std::mutex> guard(gLock);
        if (!gLastError.empty()) tail = "; the holder's last error line: '" + gLastError + "'";
    }
    if (gReturned.load()) tail += "; the holder returned status " + std::to_string(gStatus.load());
    std::fprintf(stderr, "FAIL %s: %s%s\n", scenario, what.c_str(), tail.c_str());
    std::fflush(stderr);
    _exit(1);
}

void pass(const char* scenario, const char* what) { std::printf("PASS %s: %s\n", scenario, what); }

int connectTo(const std::string& path, int* error = nullptr) {
    const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);
    if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        if (error != nullptr) *error = errno;
        close(fd);
        return -1;
    }
    return fd;
}

// What a descriptor-passing peer gets from the holder: the byte and the descriptor, or the end of the connection.
struct Received {
    bool closed = false;
    bool timedOut = false;
    char byte = 0;
    int descriptor = -1;
};

Received receive(int connection, int millis) {
    Received result;
    pollfd waiting{connection, POLLIN, 0};
    if (poll(&waiting, 1, millis) <= 0) {
        result.timedOut = true;
        return result;
    }
    char byte = 0;
    iovec vector{&byte, 1};
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))] = {};
    msghdr message{};
    message.msg_iov = &vector;
    message.msg_iovlen = 1;
    message.msg_control = control;
    message.msg_controllen = sizeof(control);
    const ssize_t count = recvmsg(connection, &message, 0);
    if (count <= 0) {
        result.closed = true;
        return result;
    }
    result.byte = byte;
    for (cmsghdr* header = CMSG_FIRSTHDR(&message); header != nullptr; header = CMSG_NXTHDR(&message, header)) {
        if (header->cmsg_level == SOL_SOCKET && header->cmsg_type == SCM_RIGHTS) {
            std::memcpy(&result.descriptor, CMSG_DATA(header), sizeof(int));
        }
    }
    return result;
}

size_t propertyCount() {
    std::lock_guard<std::mutex> guard(gLock);
    return gProperties.size();
}

}  // namespace

// ---- The platform's calls that the holder makes and the host does not have, and the ones that are redirected.

extern "C" void holder_test_log(char level, const char* format, ...) {
    char line[512];
    va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(line, sizeof(line), format, arguments);
    va_end(arguments);
    std::lock_guard<std::mutex> guard(gLock);
    if (level == 'E') gLastError = line;
    std::fprintf(stderr, "holder %c: %s\n", level, line);
}

extern "C" int android_get_control_socket(const char* name) {
    return std::strcmp(name, "kernarium_sms") == 0 ? gHandoffFd.load() : -1;
}

extern "C" int getpeercon(int, char** context) {
    std::lock_guard<std::mutex> guard(gLock);
    *context = strdup(gPeerContext.c_str());
    return 0;
}

extern "C" void freecon(char* context) { std::free(context); }

namespace android {
namespace base {
bool SetProperty(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> guard(gLock);
    gProperties.push_back(key + "=" + value);
    return true;
}
}  // namespace base
}  // namespace android

extern "C" int accept4(int fd, sockaddr* address, socklen_t* length, int flags) {
    const int connection = accept(fd, address, length);
    if (connection < 0) return -1;
    if ((flags & SOCK_CLOEXEC) != 0) fcntl(connection, F_SETFD, FD_CLOEXEC);
    if ((flags & SOCK_NONBLOCK) != 0) fcntl(connection, F_SETFL, fcntl(connection, F_GETFL) | O_NONBLOCK);
    return connection;
}

// A vsock socket is a unix stream socket at the test's path; every other socket is the host's.
extern "C" int socket(int domain, int type, int protocol) {
    static const auto real = realFunction<int (*)(int, int, int)>("socket");
    if (domain != AF_VSOCK) return real(domain, type, protocol);
    const int fd = real(AF_UNIX, type & ~(SOCK_CLOEXEC | SOCK_NONBLOCK), 0);
    if (fd < 0) return fd;
    if ((type & SOCK_CLOEXEC) != 0) fcntl(fd, F_SETFD, FD_CLOEXEC);
    if ((type & SOCK_NONBLOCK) != 0) fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    gVsockFd.store(fd);
    return fd;
}

extern "C" int bind(int fd, const sockaddr* address, socklen_t length) {
    static const auto real = realFunction<int (*)(int, const sockaddr*, socklen_t)>("bind");
    if (fd != gVsockFd.load()) return real(fd, address, length);
    sockaddr_un local{};
    local.sun_family = AF_UNIX;
    std::strncpy(local.sun_path, gVsockPath.c_str(), sizeof(local.sun_path) - 1);
    return real(fd, reinterpret_cast<const sockaddr*>(&local), sizeof(local));
}

extern "C" int getsockopt(int fd, int level, int name, void* value, socklen_t* length) {
    static const auto real = realFunction<int (*)(int, int, int, void*, socklen_t*)>("getsockopt");
    if (level != SOL_SOCKET || name != SO_PEERCRED) return real(fd, level, name, value, length);
    ucred credentials{};
    credentials.uid = gPeerUid.load();
    std::memcpy(value, &credentials, sizeof(credentials));
    *length = sizeof(credentials);
    return 0;
}

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: holder_test DIRECTORY\n");
        return 2;
    }
    signal(SIGPIPE, SIG_IGN);
    const std::string directory = argv[1];
    const std::string handoffPath = directory + "/handoff";
    gVsockPath = directory + "/vsock";

    // The socket init makes for the holder: created, bound, and nothing more.
    const int handoff = socket(AF_UNIX, SOCK_STREAM, 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::strncpy(address.sun_path, handoffPath.c_str(), sizeof(address.sun_path) - 1);
    if (bind(handoff, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        std::perror("bind of the handoff socket");
        return 2;
    }
    gHandoffFd.store(handoff);

    std::thread([] {
        gStatus.store(holder_main());
        gReturned.store(true);
    }).detach();

    // S1: init does not listen, so the holder does: it must stay in its loop on a socket that is bound and not
    // listening (on Linux such a socket polls as hung up, which ended the holder at once; macOS reports nothing for
    // it, so there the missing listen shows in S3, which finds the handoff socket refusing connections).
    until(400, [] { return gReturned.load(); });
    if (gReturned.load()) fail("S1", "the holder ended on the handoff socket that init made (bound, not listening)");
    pass("S1", "the holder keeps running on the handoff socket as init makes it");

    // S2: a connection that waits on the vsock port starts the waker, through the control property.
    int host = -1;
    until(3000, [&] { return (host = connectTo(gVsockPath)) >= 0; });
    if (host < 0) fail("S2", "the vsock socket of the holder does not take a connection");
    if (!until(3000, [] { return propertyCount() >= 1; })) fail("S2", "no waker was started for a waiting connection");
    {
        std::lock_guard<std::mutex> guard(gLock);
        if (gProperties[0] != "ctl.start=kernarium_sms_wake") fail("S2", "the property set is " + gProperties[0]);
    }
    pass("S2", "a waiting connection sets ctl.start=kernarium_sms_wake");

    // S3: the service connects to the handoff socket and is passed the listening socket, which is the vsock one.
    int error = 0;
    const int service = connectTo(handoffPath, &error);
    if (service < 0) {
        fail("S3", std::string("the handoff socket takes no connection: ") + std::strerror(error));
    }
    const Received passed = receive(service, 3000);
    if (passed.timedOut || passed.closed || passed.byte != 'L' || passed.descriptor < 0) {
        fail("S3", "the service was not passed the listening socket");
    }
    pollfd waiting{passed.descriptor, POLLIN, 0};
    if (poll(&waiting, 1, 3000) <= 0) fail("S3", "the passed socket does not show the waiting connection");
    const int accepted = accept(passed.descriptor, nullptr, nullptr);
    if (accepted < 0) fail("S3", "the passed socket does not accept the waiting connection");
    char ping[4] = {'p', 'i', 'n', 'g'};
    char echo[4] = {};
    if (write(host, ping, 4) != 4 || read(accepted, echo, 4) != 4 || std::memcmp(ping, echo, 4) != 0) {
        fail("S3", "the accepted connection is not the host's");
    }
    close(accepted);
    close(host);
    pass("S3", "the service is passed the listening socket and accepts the host's connection from it");

    // S4: a second connection on the handoff socket while a session is attached gets nothing.
    const int second = connectTo(handoffPath);
    if (second < 0) fail("S4", "the handoff socket takes no second connection");
    const Received refused = receive(second, 3000);
    if (!refused.closed || refused.descriptor >= 0) fail("S4", "a second connection was not closed without a descriptor");
    close(second);
    pass("S4", "a second connection during a session is closed and passed nothing");

    // S5: the session ends with the service's connection, and the next waiting connection wakes the waker again.
    close(service);
    const int next = connectTo(gVsockPath);
    if (next < 0) fail("S5", "the vsock socket takes no connection after the session");
    if (!until(5000, [] { return propertyCount() >= 2; })) fail("S5", "no second waker after the session ended");
    pass("S5", "after the session ends the next waiting connection starts the waker again");

    // S6 and S7: only the system user in the service's own domain is passed the socket.
    gPeerUid.store(1001);
    const int stranger = connectTo(handoffPath);
    if (stranger < 0) fail("S6", "the handoff socket takes no connection from another user");
    const Received other = receive(stranger, 3000);
    if (!other.closed || other.descriptor >= 0) fail("S6", "a connection of another user was passed the socket");
    close(stranger);
    gPeerUid.store(1000);
    {
        std::lock_guard<std::mutex> guard(gLock);
        gPeerContext = "u:r:shell:s0";
    }
    const int wrongDomain = connectTo(handoffPath);
    if (wrongDomain < 0) fail("S7", "the handoff socket takes no connection from another domain");
    const Received foreign = receive(wrongDomain, 3000);
    if (!foreign.closed || foreign.descriptor >= 0) fail("S7", "a connection of another domain was passed the socket");
    close(wrongDomain);
    pass("S6", "a connection of another user is closed and passed nothing");
    pass("S7", "a connection of another domain is closed and passed nothing");

    if (gReturned.load()) fail("S8", "the holder ended during the scenarios");
    pass("S8", "the holder ran through every scenario");
    close(next);
    std::printf("PASS\n");
    std::fflush(stdout);
    _exit(0);
}
