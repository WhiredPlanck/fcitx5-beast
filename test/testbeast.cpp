#include "../src/beast.h"
#include <fcitx-config/rawconfig.h>
#include <fcitx-utils/event.h>
#include <fcitx-utils/eventdispatcher.h>
#include <fcitx-utils/log.h>
#include <fcitx/addonmanager.h>
#include <fcitx/instance.h>

#include <atomic>
#include <string>
#include <thread>

#include "testutil.h"

using namespace fcitx;
namespace http = boost::beast::http;
using local = boost::asio::local::stream_protocol;
using tcp = boost::asio::ip::tcp;

FCITX_DEFINE_STATIC_ADDON_REGISTRY(getStaticAddon)
FCITX_IMPORT_ADDON_FACTORY(getStaticAddon, beast);

namespace {

const std::string configTarget = "/config/addon/beast";
const std::string defaultSocket = "/tmp/fcitx5.sock";
const std::string anotherSocket = "/tmp/fcitx5-another.sock";
constexpr int defaultPort = 32489;
constexpr int movedPort = 32490;

const local::endpoint unixEndpoint{defaultSocket};
const local::endpoint anotherEndpoint{anotherSocket};
const tcp::endpoint defaultEndpoint{boost::asio::ip::make_address("127.0.0.1"),
                                    defaultPort};
const tcp::endpoint movedEndpoint{boost::asio::ip::make_address("127.0.0.1"),
                                  movedPort};

// Applies a config write on the fcitx thread, as the daemon and its hosts do.
// The restart it causes may have to wait for the requests in flight, so it can
// only finish once the event loop has run.
void reconfigure(EventDispatcher &dispatcher, Beast *beast,
                 BeastCommunication communication,
                 const std::string &socketPath, int port) {
    RawConfig raw;
    beast->getConfig()->dumpDescription(raw);
    BeastConfig config;
    config.communication.setValue(communication);
    config.unix_socket.mutableValue()->path.setValue(socketPath);
    config.tcp.mutableValue()->port.setValue(port);
    config.save(raw);
    onEventLoop(dispatcher, [beast, raw] { beast->setConfig(raw); });
}

} // namespace

int main() {
    char arg0[] = "testbeast";
    char arg1[] = "--disable=all";
    char arg2[] = "--enable=beast";
    char *argv[] = {arg0, arg1, arg2};
    Log::setLogRule("default=5");
    TestConfigHome configHome;

    Instance instance(FCITX_ARRAY_SIZE(argv), argv);
    instance.addonManager().registerDefaultLoader(&getStaticAddon());
    EventDispatcher dispatcher;
    dispatcher.attach(&instance.eventLoop());

    instance.initialize();
    auto beast = dynamic_cast<Beast *>(instance.addonManager().addon("beast"));
    CHECK(beast);

    // Written on the server thread and read here.
    std::atomic<bool> setterCalled{false};
    std::atomic<bool> remoteCalled{false};

    beast->setConfigSetter([&](const char *, const char *) {
        setterCalled = true;
        return "";
    });
    beast->setRemoteHandler([&](const std::string_view,
                                const char *) -> std::pair<bool, std::string> {
        remoteCalled = true;
        return {true, ""};
    });

    std::thread loop([&] { instance.eventLoop().exec(); });
    CHECK(waitUntil([&] { return serving(unixEndpoint, configTarget); }));

    // A hook keeps its own state between requests: it is invoked through the
    // installed callable, not through a copy taken per request.
    beast->setConfigGetter(
        [calls = 0](const char *) mutable { return std::to_string(++calls); });
    const auto first = request(unixEndpoint, http::verb::get, configTarget);
    const auto second = request(unixEndpoint, http::verb::get, configTarget);
    CHECK(first && first->body == "1");
    CHECK(second && second->body == "2");
    beast->setConfigGetter([](const char *) { return std::string(); });

    CHECK(!setterCalled);
    CHECK(request(unixEndpoint, http::verb::post, configTarget).has_value());
    CHECK(setterCalled);

    CHECK(!remoteCalled);
    CHECK(request(unixEndpoint, http::verb::post, "/remote/c").has_value());
    CHECK(remoteCalled);

    reconfigure(dispatcher, beast, BeastCommunication::UnixSocket,
                anotherSocket, defaultPort);
    CHECK(waitUntil([&] { return serving(anotherEndpoint, configTarget); }));
    CHECK(!request(unixEndpoint, http::verb::get, configTarget));

    reconfigure(dispatcher, beast, BeastCommunication::TCP, anotherSocket,
                defaultPort);
    CHECK(waitUntil([&] { return serving(defaultEndpoint, configTarget); }));
    CHECK(!request(anotherEndpoint, http::verb::get, configTarget));

    CHECK(request(defaultEndpoint, http::verb::get, configTarget).has_value());

    setterCalled = false;
    CHECK(request(defaultEndpoint, http::verb::post, configTarget).has_value());
    CHECK(setterCalled);

    remoteCalled = false;
    CHECK(request(defaultEndpoint, http::verb::post, "/remote/c").has_value());
    CHECK(remoteCalled);

    reconfigure(dispatcher, beast, BeastCommunication::TCP, anotherSocket,
                movedPort);
    CHECK(waitUntil([&] { return serving(movedEndpoint, configTarget); }));
    CHECK(!request(defaultEndpoint, http::verb::get, configTarget));

    dispatcher.schedule([&instance] { instance.eventLoop().exit(); });
    loop.join();
    return 0;
}
