#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace novacache::net {

enum class IoStatus {
    ok,
    would_block,
    closed,
};

struct IoResult {
    IoStatus status = IoStatus::ok;
    std::size_t bytes = 0;
};

class Socket {
  public:
    Socket() noexcept = default;
    ~Socket();

    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    Socket(Socket&& other) noexcept;
    Socket& operator=(Socket&& other) noexcept;

    [[nodiscard]] static Socket connect(std::string_view host, std::uint16_t port);
    [[nodiscard]] static Socket listen(std::string_view host, std::uint16_t port,
                                       int backlog = 128);

    [[nodiscard]] Socket accept() const;
    [[nodiscard]] std::optional<Socket> try_accept() const;
    [[nodiscard]] std::size_t receive(std::span<char> buffer) const;
    [[nodiscard]] IoResult try_receive(std::span<char> buffer) const;
    void send_all(std::string_view bytes) const;
    [[nodiscard]] IoResult try_send(std::string_view bytes) const;
    void shutdown() const noexcept;
    void set_nonblocking(bool enabled = true) const;

    [[nodiscard]] std::uint16_t local_port() const;
    [[nodiscard]] int native_handle() const noexcept;
    [[nodiscard]] explicit operator bool() const noexcept;

  private:
    explicit Socket(int descriptor) noexcept;
    void close() noexcept;

    int descriptor_ = -1;
};

} // namespace novacache::net
