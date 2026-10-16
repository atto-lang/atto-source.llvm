#include "scoped_tcp_boundary.hpp"
#include <iostream>
#include <stdexcept>
#include <string>

#if defined(__unix__) || defined(__APPLE__)
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

using namespace oreslang::permissions;

#if defined(__unix__) || defined(__APPLE__)
int main() {
    const int listener = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) throw std::runtime_error("loopback listener socket failed");
    struct Guard { int fd; ~Guard() { if (fd >= 0) ::close(fd); } } server{listener};
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = 0;
    if (::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) != 1 ||
        ::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
        ::listen(listener, 2) != 0)
        throw std::runtime_error("loopback setup failed");
    socklen_t length = sizeof(address);
    if (::getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length) != 0)
        throw std::runtime_error("loopback port lookup failed");
    const std::string endpoint = "127.0.0.1:" + std::to_string(ntohs(address.sin_port));
    const ScopedPolicy denyAll;
    const auto grant = ScopedPolicy::from_host({Permission::Net},
        {{Permission::Net, endpoint, false}});
    const auto denied = ScopedPolicy::from_host({Permission::Net},
        {{Permission::Net, "*", false}, {Permission::Net, "127.0.0.1", true}});
    auto mustDeny = [&](const ScopedPolicy& policy,
                        const std::string& requested, const std::string& selected) {
        try {
            const int leaked = ScopedTcpBoundary::connect_selected_ipv4(
                policy, requested, selected);
            ::close(leaked);
        } catch (const std::domain_error&) { return; }
        throw std::runtime_error("denied host endpoint was connected");
    };
    mustDeny(denyAll, endpoint, endpoint);
    mustDeny(denied, endpoint, endpoint);
    mustDeny(grant, "127.1:" + std::to_string(ntohs(address.sin_port)), endpoint);
    mustDeny(grant, endpoint, "127.0.0.2:" + std::to_string(ntohs(address.sin_port)));
    try {
        (void)ScopedTcpBoundary::connect_selected_ipv4(grant, endpoint, endpoint, 0);
        throw std::runtime_error("invalid timeout did not reject");
    } catch (const std::invalid_argument&) { }

    Guard client{ScopedTcpBoundary::connect_selected_ipv4(grant, endpoint, endpoint, 2000)};
    if (client.fd < 0) throw std::runtime_error("allowed TCP connect failed");
    Guard accepted{::accept(listener, nullptr, nullptr)};
    if (accepted.fd < 0) throw std::runtime_error("loopback accept failed");
    sockaddr_in peer{};
    socklen_t peerLen = sizeof(peer);
    if (::getpeername(client.fd, reinterpret_cast<sockaddr*>(&peer), &peerLen) != 0 ||
        peer.sin_addr.s_addr != address.sin_addr.s_addr ||
        peer.sin_port != address.sin_port)
        throw std::runtime_error("socket peer differs from checked numeric destination");
    std::cout << "NATIVE_PINNED_TCP_HOST_BOUNDARY_PASS\n";
}
#else
int main() {
    const ScopedPolicy denyAll;
    try { (void)ScopedTcpBoundary::connect_selected_ipv4(
        denyAll, "127.0.0.1:443", "127.0.0.1:443"); }
    catch (const std::domain_error&) {
        std::cout << "NATIVE_PINNED_TCP_UNSUPPORTED_PLATFORM_DENY_PASS\n";
        return 0;
    }
    throw std::runtime_error("unsupported platform unexpectedly connected");
}
#endif
