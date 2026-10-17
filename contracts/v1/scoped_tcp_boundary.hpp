#pragma once
#include "scoped_permission_policy.hpp"
#include <chrono>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

#if defined(__unix__) || defined(__APPLE__)
#include <arpa/inet.h>
#include <cerrno>
#include <climits>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace oreslang::permissions {
// Trusted HOST-ONLY numeric TCP socket adapter. Not exposed to guest code and
// not wired to compiler lowering. Callers must supply the exact DNS-selected
// address, which is pinned in sockaddr and never resolved again.
class ScopedTcpBoundary final {
public:
    static int connect_selected_ipv4(const ScopedPolicy& policy,
                                     std::string_view requested,
                                     std::string_view selected,
                                     int timeout_ms = 15000) {
        if (timeout_ms < 1 || timeout_ms > 30000)
            throw std::invalid_argument("invalid TCP connect timeout");
        // Check BEFORE allocating a socket or touching the network.
        if (!policy.allows_network_connection(requested, selected))
            throw std::domain_error("selected network endpoint denied");
#if defined(__unix__) || defined(__APPLE__)
        const auto colon = selected.rfind(':');
        if (colon == selected.npos || colon == 0 || colon + 1 == selected.size() ||
            selected.size() - (colon + 1) > 5)
            throw std::domain_error("invalid selected endpoint");
        // The trusted adapter independently rejects long numeric ports
        // before unsigned arithmetic; policy admission alone is not a
        // substitute for validating the actual socket endpoint.
        const std::string host(selected.substr(0, colon));
        unsigned port = 0;
        for (std::size_t i = colon + 1; i < selected.size(); ++i) {
            if (selected[i] < '0' || selected[i] > '9')
                throw std::domain_error("invalid selected port");
            port = port * 10 + static_cast<unsigned>(selected[i] - '0');
            if (port > 65535) throw std::domain_error("invalid selected port");
        }
        if (port == 0) throw std::domain_error("invalid selected port");
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<std::uint16_t>(port));
        if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1)
            throw std::domain_error("selected address must be numeric IPv4");
        char canonical[INET_ADDRSTRLEN]{};
        if (!::inet_ntop(AF_INET, &addr.sin_addr, canonical, sizeof(canonical)) ||
            host != canonical)
            throw std::domain_error("selected IPv4 address is not canonical");

        struct Descriptor {
            int fd;
            ~Descriptor() { if (fd >= 0) ::close(fd); }
            int release() { const int old = fd; fd = -1; return old; }
        } socket{::socket(AF_INET, SOCK_STREAM, 0)};
        if (socket.fd < 0)
            throw std::system_error(errno, std::generic_category(), "socket");
        int flags = ::fcntl(socket.fd, F_GETFL, 0);
        if (flags < 0 || ::fcntl(socket.fd, F_SETFL, flags | O_NONBLOCK) < 0)
            throw std::system_error(errno, std::generic_category(), "nonblocking socket");

        const int connected = ::connect(socket.fd,
            reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
        if (connected != 0) {
            if (errno != EINPROGRESS)
                throw std::system_error(errno, std::generic_category(), "TCP connect");
            const auto deadline = std::chrono::steady_clock::now() +
                std::chrono::milliseconds(timeout_ms);
            pollfd pending{socket.fd, POLLOUT, 0};
            for (;;) {
                const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - std::chrono::steady_clock::now()).count();
                if (remaining <= 0) throw std::system_error(
                    std::make_error_code(std::errc::timed_out), "TCP connect timeout");
                int ready = ::poll(&pending, 1, static_cast<int>(remaining));
                if (ready < 0 && errno == EINTR) continue;
                if (ready < 0)
                    throw std::system_error(errno, std::generic_category(), "TCP connect poll");
                if (ready == 0) throw std::system_error(
                    std::make_error_code(std::errc::timed_out), "TCP connect timeout");
                int failure = 0;
                socklen_t size = sizeof(failure);
                if (::getsockopt(socket.fd, SOL_SOCKET, SO_ERROR, &failure, &size) < 0)
                    throw std::system_error(errno, std::generic_category(), "TCP connect status");
                if (failure)
                    throw std::system_error(failure, std::generic_category(), "TCP connect");
                break;
            }
        }
        if (::fcntl(socket.fd, F_SETFL, flags) < 0)
            throw std::system_error(errno, std::generic_category(), "TCP connect restore flags");
        return socket.release();  // Trusted host owns returned fd and MUST close.
#else
        throw std::runtime_error("native pinned TCP is unsupported on this platform");
#endif
    }
};
} // namespace oreslang::permissions
