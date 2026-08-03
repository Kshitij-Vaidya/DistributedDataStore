#include "novacache/net/socket.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

namespace novacache::net {
namespace {

[[noreturn]] void throw_socket_error(const char* operation) {
    throw std::system_error(errno, std::generic_category(), operation);
}

void configure_no_sigpipe(const int descriptor) {
#if defined(SO_NOSIGPIPE)
    const int enabled = 1;
    if (::setsockopt(descriptor, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) != 0) {
        throw_socket_error("setsockopt(SO_NOSIGPIPE)");
    }
#else
    static_cast<void>(descriptor);
#endif
}

class AddressInfo {
  public:
    AddressInfo(const std::string& host, const std::string& service, const int flags) {
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;
        hints.ai_flags = flags;

        const char* const node = host.empty() ? nullptr : host.c_str();
        const int status = ::getaddrinfo(node, service.c_str(), &hints, &addresses_);
        if (status != 0) {
            throw std::runtime_error(std::string{"getaddrinfo: "} + ::gai_strerror(status));
        }
    }

    ~AddressInfo() { ::freeaddrinfo(addresses_); }

    AddressInfo(const AddressInfo&) = delete;
    AddressInfo& operator=(const AddressInfo&) = delete;

    [[nodiscard]] const addrinfo* get() const noexcept { return addresses_; }

  private:
    addrinfo* addresses_ = nullptr;
};

[[nodiscard]] int create_descriptor(const addrinfo& address) {
    const int descriptor = ::socket(address.ai_family, address.ai_socktype, address.ai_protocol);
    if (descriptor >= 0) {
        try {
            configure_no_sigpipe(descriptor);
        } catch (...) {
            static_cast<void>(::close(descriptor));
            throw;
        }
    }
    return descriptor;
}

} // namespace

Socket::Socket(const int descriptor) noexcept : descriptor_(descriptor) {}

Socket::~Socket() { close(); }

Socket::Socket(Socket&& other) noexcept : descriptor_(std::exchange(other.descriptor_, -1)) {}

Socket& Socket::operator=(Socket&& other) noexcept {
    if (this != &other) {
        close();
        descriptor_ = std::exchange(other.descriptor_, -1);
    }
    return *this;
}

Socket Socket::connect(const std::string_view host, const std::uint16_t port) {
    const AddressInfo addresses{std::string{host}, std::to_string(port), 0};
    int last_error = ECONNREFUSED;

    for (const addrinfo* address = addresses.get(); address != nullptr;
         address = address->ai_next) {
        const int descriptor = create_descriptor(*address);
        if (descriptor < 0) {
            last_error = errno;
            continue;
        }

        bool connected = false;
        for (;;) {
            if (::connect(descriptor, address->ai_addr, address->ai_addrlen) == 0) {
                connected = true;
                break;
            }
            if (errno == EINTR) {
                continue;
            }
            if (errno == EISCONN) {
                connected = true;
                break;
            }
            last_error = errno;
            break;
        }
        if (connected) {
            return Socket{descriptor};
        }
        static_cast<void>(::close(descriptor));
    }

    throw std::system_error(last_error, std::generic_category(), "connect");
}

Socket Socket::listen(const std::string_view host, const std::uint16_t port, const int backlog) {
    if (backlog <= 0) {
        throw std::invalid_argument("socket backlog must be positive");
    }

    const AddressInfo addresses{std::string{host}, std::to_string(port), AI_PASSIVE};
    int last_error = EADDRNOTAVAIL;

    for (const addrinfo* address = addresses.get(); address != nullptr;
         address = address->ai_next) {
        const int descriptor = create_descriptor(*address);
        if (descriptor < 0) {
            last_error = errno;
            continue;
        }

        const int enabled = 1;
        if (::setsockopt(descriptor, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)) != 0) {
            last_error = errno;
            static_cast<void>(::close(descriptor));
            continue;
        }
        if (::bind(descriptor, address->ai_addr, address->ai_addrlen) != 0 ||
            ::listen(descriptor, backlog) != 0) {
            last_error = errno;
            static_cast<void>(::close(descriptor));
            continue;
        }
        return Socket{descriptor};
    }

    throw std::system_error(last_error, std::generic_category(), "bind/listen");
}

Socket Socket::accept() const {
    for (;;) {
        const int descriptor = ::accept(descriptor_, nullptr, nullptr);
        if (descriptor >= 0) {
            try {
                configure_no_sigpipe(descriptor);
                return Socket{descriptor};
            } catch (...) {
                static_cast<void>(::close(descriptor));
                throw;
            }
        }
        if (errno != EINTR) {
            throw_socket_error("accept");
        }
    }
}

std::size_t Socket::receive(const std::span<char> buffer) const {
    if (buffer.empty()) {
        return 0;
    }
    if (buffer.size() > static_cast<std::size_t>(std::numeric_limits<ssize_t>::max())) {
        throw std::length_error("receive buffer is too large");
    }

    for (;;) {
        const ssize_t received = ::recv(descriptor_, buffer.data(), buffer.size(), 0);
        if (received >= 0) {
            return static_cast<std::size_t>(received);
        }
        if (errno != EINTR) {
            throw_socket_error("recv");
        }
    }
}

void Socket::send_all(const std::string_view bytes) const {
    std::size_t sent = 0;
    while (sent < bytes.size()) {
        const std::size_t remaining = bytes.size() - sent;
        const std::size_t chunk =
            std::min(remaining, static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
#if defined(MSG_NOSIGNAL)
        constexpr int flags = MSG_NOSIGNAL;
#else
        constexpr int flags = 0;
#endif
        const ssize_t result = ::send(descriptor_, bytes.data() + sent, chunk, flags);
        if (result > 0) {
            sent += static_cast<std::size_t>(result);
            continue;
        }
        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result == 0) {
            throw std::system_error(EPIPE, std::generic_category(), "send");
        }
        throw_socket_error("send");
    }
}

void Socket::shutdown() const noexcept {
    if (descriptor_ >= 0) {
        static_cast<void>(::shutdown(descriptor_, SHUT_RDWR));
    }
}

std::uint16_t Socket::local_port() const {
    sockaddr_storage address{};
    socklen_t size = sizeof(address);
    if (::getsockname(descriptor_, reinterpret_cast<sockaddr*>(&address), &size) != 0) {
        throw_socket_error("getsockname");
    }
    if (address.ss_family == AF_INET) {
        const auto* const ipv4 = reinterpret_cast<const sockaddr_in*>(&address);
        return ntohs(ipv4->sin_port);
    }
    if (address.ss_family == AF_INET6) {
        const auto* const ipv6 = reinterpret_cast<const sockaddr_in6*>(&address);
        return ntohs(ipv6->sin6_port);
    }
    throw std::runtime_error("socket has an unsupported address family");
}

int Socket::native_handle() const noexcept { return descriptor_; }

Socket::operator bool() const noexcept { return descriptor_ >= 0; }

void Socket::close() noexcept {
    if (descriptor_ >= 0) {
        static_cast<void>(::close(descriptor_));
        descriptor_ = -1;
    }
}

} // namespace novacache::net
