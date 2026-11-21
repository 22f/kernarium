// SPDX-License-Identifier: GPL-2.0-or-later

#include "forwarder.hpp"
#include "named_tests.hpp"
#include "vsock_test_guest.hpp"
#include "watchdog.hpp"

#include <optional>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>

namespace {

using namespace uml::vsock;
using namespace uml::vsock::testing;

std::optional<ForwardSpecification> parse(std::string_view text)
{
    std::string error;
    auto result = parseForward(text, error);
    require(result.has_value() == error.empty());
    return result;
}

/* The guest's endpoint, borrowed for a forwarder that must not own it. */
std::shared_ptr<Endpoint> borrowed(Guest& guest)
{
    return std::shared_ptr<Endpoint>(&guest.endpoint(), [](Endpoint*) {});
}

ForwardSpecification forward(std::uint16_t hostPort, std::uint32_t guestPort)
{
    return {
        .hostAddress = "127.0.0.1",
        .hostPort = hostPort,
        .guestPort = guestPort,
    };
}

void parsesTheForwardGrammar()
{
    const auto plain = parse("5555-5000");
    require(plain.has_value());
    require(plain->hostAddress == "127.0.0.1" && plain->hostPort == 5555 &&
        plain->guestPort == 5000);
    const auto chosen = parse("127.0.0.1:0-5555");
    require(chosen.has_value() && chosen->hostPort == 0 && chosen->guestPort == 5555);
    const auto other = parse("127.0.0.2:8000-1");
    require(other.has_value() && other->hostAddress == "127.0.0.2");
    const auto high = parse("65535-4294967294");
    require(high.has_value() && high->guestPort == 4294967294U);
}

void refusesWhatIsNotLoopback()
{
    for (const char* text : {
             "0.0.0.0:1-2", "192.0.2.1:1-2", "203.0.113.9:1-2", "localhost:1-2",
             "[::1]:1-2", "::1:1-2", "8.8.8.8:0-5555"}) {
        std::string error;
        require(!parseForward(text, error).has_value(), text);
        require(error.find("loopback") != std::string::npos, text);
    }
}

void refusesMalformedForwards()
{
    for (const char* text : {
             "", "abc", "5555", "5555-", "-5555", "70000-1", "1-0", "1-4294967295",
             "1-4294967296", "x-1", "1-x", "1:2-3-4", "127.0.0.1:-5", "-1-5"}) {
        std::string error;
        require(!parseForward(text, error).has_value(), text);
        require(!error.empty());
    }
}

int connectLoopback(std::uint16_t port)
{
    const int descriptor = ::socket(AF_INET, SOCK_STREAM, 0);
    require(descriptor >= 0);
    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_len = sizeof(address);
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    require(::connect(descriptor, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    return descriptor;
}

bool readable(int descriptor, int milliseconds)
{
    pollfd waiting {.fd = descriptor, .events = POLLIN, .revents = 0};
    return ::poll(&waiting, 1, milliseconds) > 0;
}

/* Whether the forwarder ended the client's stream within `milliseconds`. */
bool endsWithin(int descriptor, int milliseconds)
{
    if (!readable(descriptor, milliseconds))
        return false;
    char byte = 0;
    return ::recv(descriptor, &byte, 1, 0) <= 0;
}

/* A connection the guest accepts, one request and its reply over it, and the
 * client's close, which the guest hears as a shutdown. */
void exchangeWith(Guest& guest, std::uint16_t hostPort, std::uint32_t guestPort)
{
    const int client = connectLoopback(hostPort);
    const auto request = guest.expect();
    require(request.header.op == protocol::opRequest);
    require(request.header.destinationPort == guestPort);
    guest.transmit(guestHeader(
        protocol::opResponse, guestPort, request.header.sourcePort));
    const char hello[] = "hello guest";
    require(::send(client, hello, sizeof(hello), 0) == static_cast<ssize_t>(sizeof(hello)));
    const auto data = guest.expect();
    require(data.header.op == protocol::opReadWrite);
    require(data.payload.size() == sizeof(hello));
    const char reply[] = "hello host";
    guest.transmit(
        guestHeader(
            protocol::opReadWrite, guestPort, request.header.sourcePort,
            sizeof(reply)),
        std::span(reinterpret_cast<const std::uint8_t*>(reply), sizeof(reply)));
    char received[64] {};
    require(readable(client, 5000));
    require(::recv(client, received, sizeof(received), 0) == static_cast<ssize_t>(sizeof(reply)));
    require(std::strcmp(received, reply) == 0);
    ::close(client);
    require(guest.expectNotCredit().header.op == protocol::opShutdown);
}

/* A client whose connection the listener's host has completed: one byte sent
 * over it is acknowledged, which only a connection in the accept queue does,
 * or the forwarder has already ended the stream. */
int connectCompleted(std::uint16_t port)
{
    const int client = connectLoopback(port);
    const char byte = 'x';
    (void)::send(client, &byte, 1, 0);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    for (;;) {
        int unacknowledged = 1;
        socklen_t length = sizeof(unacknowledged);
        if (::getsockopt(client, SOL_SOCKET, SO_NWRITE, &unacknowledged, &length) != 0 ||
            unacknowledged == 0 || readable(client, 0))
            return client;
        require(std::chrono::steady_clock::now() < deadline, "the connection was never completed");
    }
}

void forwardsAConnectionToTheGuestPort()
{
    uml::testing::Watchdog watchdog("forwarder", std::chrono::seconds(60));
    Guest guest;
    Forwarder forwarder(borrowed(guest), {forward(0, 5555)});
    require(forwarder.port(0) != 0);
    const int client = connectLoopback(forwarder.port(0));
    const auto request = guest.expect();
    require(request.header.op == protocol::opRequest);
    require(request.header.destinationPort == 5555);
    guest.transmit(guestHeader(
        protocol::opResponse, 5555, request.header.sourcePort));
    const char hello[] = "hello guest";
    require(::send(client, hello, sizeof(hello), 0) == static_cast<ssize_t>(sizeof(hello)));
    const auto data = guest.expect();
    require(data.header.op == protocol::opReadWrite);
    require(data.payload.size() == sizeof(hello));
    const char reply[] = "hello host";
    guest.transmit(
        guestHeader(
            protocol::opReadWrite, 5555, request.header.sourcePort, sizeof(reply)),
        std::span(reinterpret_cast<const std::uint8_t*>(reply), sizeof(reply)));
    char received[64] {};
    require(readable(client, 5000));
    require(::recv(client, received, sizeof(received), 0) == static_cast<ssize_t>(sizeof(reply)));
    require(std::strcmp(received, reply) == 0);
    /* The client closes, and the guest is told the stream ended. */
    ::close(client);
    const auto shutdown = guest.expectNotCredit();
    require(shutdown.header.op == protocol::opShutdown);
}

void closesAClientTheGuestRefuses()
{
    uml::testing::Watchdog watchdog("forwarder refusal", std::chrono::seconds(60));
    Guest guest;
    Forwarder forwarder(borrowed(guest), {forward(0, 7)});
    const int client = connectLoopback(forwarder.port(0));
    const auto request = guest.expect();
    guest.transmit(guestHeader(protocol::opReset, 7, request.header.sourcePort));
    guest.pump();
    require(readable(client, 5000));
    char byte = 0;
    require(::recv(client, &byte, 1, 0) <= 0);
    ::close(client);
}

void stopsPromptlyWhileAConnectionWaitsForTheGuest()
{
    uml::testing::Watchdog watchdog("forwarder stop", std::chrono::seconds(60));
    Guest guest;
    const auto started = std::chrono::steady_clock::now();
    int client = -1;
    {
        Forwarder forwarder(borrowed(guest), {forward(0, 9)});
        client = connectLoopback(forwarder.port(0));
        /* The guest never answers. */
        const auto request = guest.expect();
        require(request.header.op == protocol::opRequest);
    }
    require(std::chrono::steady_clock::now() - started < std::chrono::seconds(5));
    ::close(client);
}

void servesASecondGuestOnTheSamePortAfterDetach()
{
    uml::testing::Watchdog watchdog("forwarder reattach", std::chrono::seconds(60));
    Guest first;
    Forwarder forwarder(borrowed(first), {forward(0, 5555)});
    const auto port = forwarder.port(0);
    exchangeWith(first, port, 5555);
    forwarder.detach();
    Guest second;
    forwarder.attach(borrowed(second));
    require(forwarder.port(0) == port);
    exchangeWith(second, port, 5555);
}

void closesAConnectionThatArrivesWhileNoGuestIsAttached()
{
    uml::testing::Watchdog watchdog("forwarder gap", std::chrono::seconds(60));
    std::optional<Guest> first;
    first.emplace();
    Forwarder forwarder(borrowed(*first), {forward(0, 5555)});
    const auto port = forwarder.port(0);
    exchangeWith(*first, port, 5555);
    forwarder.detach();
    /* The listener stays bound, accepts, and closes the connection at once;
     * nothing reaches the guest that was detached. */
    int client = connectLoopback(port);
    require(endsWithin(client, 5000));
    ::close(client);
    require(first->quiet());
    /* The detached guest may go; nothing of it is kept. */
    first.reset();
    client = connectLoopback(port);
    require(endsWithin(client, 5000));
    ::close(client);
    Guest second;
    forwarder.attach(borrowed(second));
    require(forwarder.port(0) == port);
    exchangeWith(second, port, 5555);
}

void aConnectionFromTheGapNeverReachesTheNextGuest()
{
    uml::testing::Watchdog watchdog("forwarder gap client", std::chrono::seconds(60));
    Guest guest;
    /* Detached from the start. */
    Forwarder forwarder(nullptr, {forward(0, 5555)});
    const auto port = forwarder.port(0);
    /* A client that connected before a guest was attached belongs to the gap,
     * however late the forwarder gets to it: it is closed, and the guest
     * attached next never hears of it. The next guest is attached before the
     * client is waited for, so that the forwarder may not have looked at the
     * connection yet. */
    for (int round = 0; round < 300; ++round) {
        const int gap = connectCompleted(port);
        forwarder.attach(borrowed(guest));
        require(endsWithin(gap, 5000));
        ::close(gap);
        require(guest.quiet());
        forwarder.detach();
    }
    forwarder.attach(borrowed(guest));
    exchangeWith(guest, port, 5555);
}

void detachEndsAnOpeningTheGuestNeverAnswers()
{
    uml::testing::Watchdog watchdog("forwarder detach", std::chrono::seconds(60));
    Guest guest;
    Forwarder forwarder(borrowed(guest), {forward(0, 9)});
    const int client = connectLoopback(forwarder.port(0));
    /* The guest never answers. */
    const auto request = guest.expect();
    require(request.header.op == protocol::opRequest);
    const auto started = std::chrono::steady_clock::now();
    forwarder.detach();
    require(std::chrono::steady_clock::now() - started < Forwarder::acceptTimeout / 2);
    /* The device gives the open up, and the client reads its end. */
    guest.pump();
    require(endsWithin(client, 5000));
    require(guest.expectNotCredit().header.op == protocol::opReset);
    ::close(client);
}

void refusesAPortInUse()
{
    Guest guest;
    Forwarder first(borrowed(guest), {forward(0, 1)});
    bool refused = false;
    try {
        Forwarder second(borrowed(guest), {forward(first.port(0), 2)});
    } catch (const std::runtime_error&) {
        refused = true;
    }
    require(refused);
}

} // namespace

int main()
{
    static constexpr uml::testing::NamedTest tests[] = {
        {"parses-the-forward-grammar", parsesTheForwardGrammar},
        {"refuses-what-is-not-loopback", refusesWhatIsNotLoopback},
        {"refuses-malformed-forwards", refusesMalformedForwards},
        {"forwards-a-connection-to-the-guest-port", forwardsAConnectionToTheGuestPort},
        {"closes-a-client-the-guest-refuses", closesAClientTheGuestRefuses},
        {"stops-promptly-while-a-connection-waits-for-the-guest",
         stopsPromptlyWhileAConnectionWaitsForTheGuest},
        {"serves-a-second-guest-on-the-same-port-after-detach",
         servesASecondGuestOnTheSamePortAfterDetach},
        {"closes-a-connection-that-arrives-while-no-guest-is-attached",
         closesAConnectionThatArrivesWhileNoGuestIsAttached},
        {"a-connection-from-the-gap-never-reaches-the-next-guest",
         aConnectionFromTheGapNeverReachesTheNextGuest},
        {"detach-ends-an-opening-the-guest-never-answers",
         detachEndsAnOpeningTheGuestNeverAnswers},
        {"refuses-a-port-in-use", refusesAPortInUse},
    };
    return uml::testing::runNamedTests(tests);
}
