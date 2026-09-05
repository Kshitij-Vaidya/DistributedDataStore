#include "novacache/server/server.hpp"

#include "novacache/protocol/encoder.hpp"

#include <array>
#include <exception>
#include <functional>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>
#include <variant>

namespace novacache::server {
namespace {

[[nodiscard]] bool is_command_request(const protocol::RespValue& request) {
    const auto* const array = std::get_if<protocol::Array>(&request.storage());
    if (array == nullptr || !array->value.has_value() || array->value->empty()) {
        return false;
    }
    for (const protocol::RespValue& item : *array->value) {
        const auto* const bulk = std::get_if<protocol::BulkString>(&item.storage());
        if (bulk == nullptr || !bulk->value.has_value()) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool response_is_error(const protocol::RespValue& value) {
    return std::holds_alternative<protocol::Error>(value.storage());
}

[[nodiscard]] std::size_t default_worker_count(const std::size_t configured) {
    if (configured > 0U) {
        return configured;
    }
    const unsigned hardware = std::thread::hardware_concurrency();
    return hardware == 0U ? 2U : static_cast<std::size_t>(hardware);
}

} // namespace

Server::Server(ServerConfig config)
    : config_(std::move(config)),
      listener_(net::Socket::listen(config_.host, config_.port, config_.backlog)),
      bound_port_(listener_.local_port()), reactor_(net::create_reactor()), store_(config_.shards),
      workers_(std::make_unique<util::ThreadPool>(default_worker_count(config_.workers),
                                                  config_.worker_queue_limit)) {
    if (config_.persistence_enabled) {
        if (config_.data_dir.empty()) {
            throw std::invalid_argument("persistence requires --data-dir");
        }
        persistence_ =
            std::make_unique<persistence::PersistenceEngine>(persistence::PersistenceConfig{
                .data_dir = config_.data_dir,
                .fsync = config_.fsync,
                .snapshot_interval_seconds = config_.snapshot_interval_seconds,
            });
        persistence_->recover_into(store_);
    }
    listener_.set_nonblocking(true);
    reactor_->add(listener_.native_handle(), net::Interest::read);
    next_expiry_tick_ = std::chrono::steady_clock::now();
}

Server::~Server() {
    stop();
    if (persistence_ != nullptr) {
        try {
            persistence_->shutdown(store_);
        } catch (...) {
        }
        persistence_.reset();
    }
}

void Server::run() {
    std::array<net::FiredEvent, 64> events{};
    while (!stopping_.load(std::memory_order_acquire)) {
        std::optional<std::chrono::milliseconds> timeout = std::chrono::milliseconds{100};
        if (config_.active_expiry_hz > 0) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= next_expiry_tick_) {
                timeout = std::chrono::milliseconds{0};
            } else {
                timeout =
                    std::chrono::duration_cast<std::chrono::milliseconds>(next_expiry_tick_ - now);
            }
        }

        const std::size_t ready = reactor_->wait(events, timeout);
        if (stopping_.load(std::memory_order_acquire)) {
            break;
        }

        drain_completions();
        for (std::size_t index = 0; index < ready; ++index) {
            const net::FiredEvent& event = events[index];
            if (event.fd == listener_.native_handle()) {
                accept_ready();
                continue;
            }
            handle_connection_event(event.fd, event.ready, event.error, event.hangup);
        }
        drain_completions();
        run_active_expiry();
        close_idle_connections();
        if (persistence_ != nullptr) {
            try {
                persistence_->maybe_snapshot(store_);
            } catch (...) {
            }
        }
        stats_.set_used_memory(store_.approximate_memory());
    }

    std::vector<int> fds;
    fds.reserve(connections_.size());
    for (const auto& [fd, _] : connections_) {
        fds.push_back(fd);
    }
    for (const int fd : fds) {
        close_connection(fd);
    }
    workers_->shutdown();
}

void Server::stop() noexcept {
    const bool already = stopping_.exchange(true, std::memory_order_acq_rel);
    if (already) {
        return;
    }
    try {
        reactor_->wakeup();
    } catch (...) {
    }
    try {
        listener_.shutdown();
    } catch (...) {
    }
}

std::uint16_t Server::bound_port() const noexcept { return bound_port_; }

bool Server::stopping() const noexcept { return stopping_.load(std::memory_order_acquire); }

Stats& Server::stats() noexcept { return stats_; }

const Stats& Server::stats() const noexcept { return stats_; }

void Server::accept_ready() {
    while (!stopping_.load(std::memory_order_acquire)) {
        if (connections_.size() >= config_.max_connections) {
            return;
        }
        std::optional<net::Socket> accepted = listener_.try_accept();
        if (!accepted.has_value()) {
            return;
        }

        accepted->set_nonblocking(true);
        auto connection = std::make_unique<Connection>();
        connection->id = next_connection_id_++;
        connection->socket = std::move(*accepted);
        connection->parser = protocol::Parser{config_.parser_limits};
        connection->last_activity = std::chrono::steady_clock::now();
        const int fd = connection->socket.native_handle();
        reactor_->add(fd, net::Interest::read);
        Connection& active = *connection;
        connections_.emplace(fd, std::move(connection));
        stats_.on_accept();
        // Edge-triggered reactors will not re-notify for data already buffered at add time.
        read_connection(active);
        drain_completions();
        if (connections_.contains(fd)) {
            if (active.closing && active.output.empty() && active.inflight == 0U) {
                close_connection(fd);
            } else {
                update_interest(active);
            }
        }
    }
}

void Server::handle_connection_event(const int fd, const net::Interest ready, const bool error,
                                     const bool hangup) {
    const auto iterator = connections_.find(fd);
    if (iterator == connections_.end()) {
        return;
    }
    Connection& connection = *iterator->second;
    if (error) {
        close_connection(fd);
        return;
    }
    if (has_interest(ready, net::Interest::read) || hangup) {
        read_connection(connection);
        drain_completions();
        if (!connections_.contains(fd)) {
            return;
        }
    }
    if (has_interest(ready, net::Interest::write)) {
        write_connection(connection);
        if (!connections_.contains(fd)) {
            return;
        }
    }
    if (connection.closing && connection.output.empty() && connection.inflight == 0U) {
        close_connection(fd);
        return;
    }
    update_interest(connection);
}

void Server::read_connection(Connection& connection) {
    if (connection.read_closed || connection.closing) {
        return;
    }

    std::array<char, 8192> buffer{};
    for (;;) {
        if (queue_saturated()) {
            break;
        }
        const net::IoResult result = connection.socket.try_receive(buffer);
        if (result.status == net::IoStatus::would_block) {
            break;
        }
        if (result.status == net::IoStatus::closed) {
            connection.read_closed = true;
            if (connection.parser.buffered_bytes() != 0U) {
                connection.output.append(
                    protocol::encode(protocol::RespValue::error("ERR Protocol error")));
                connection.closing = true;
            } else if (connection.inflight == 0U && connection.output.empty()) {
                connection.closing = true;
            }
            break;
        }

        stats_.add_bytes_in(result.bytes);
        connection.last_activity = std::chrono::steady_clock::now();
        if (connection.parser.feed(std::string_view{buffer.data(), result.bytes}) !=
            protocol::ParseStatus::incomplete) {
            connection.output.append(
                protocol::encode(protocol::RespValue::error("ERR Protocol error")));
            connection.closing = true;
            connection.read_closed = true;
            break;
        }
        queue_parsed_commands(connection);
        if (connection.closing) {
            break;
        }
    }
}

void Server::queue_parsed_commands(Connection& connection) {
    for (;;) {
        if (queue_saturated() && connection.inflight > 0U) {
            return;
        }
        protocol::ParseResult result = connection.parser.next();
        if (result.status == protocol::ParseStatus::incomplete) {
            return;
        }
        if (result.status != protocol::ParseStatus::complete || !result.value.has_value() ||
            !is_command_request(*result.value)) {
            connection.output.append(
                protocol::encode(protocol::RespValue::error("ERR Protocol error")));
            connection.closing = true;
            connection.read_closed = true;
            return;
        }
        enqueue_command(connection, std::move(*result.value));
    }
}

void Server::enqueue_command(Connection& connection, protocol::RespValue request) {
    const std::uint64_t sequence = connection.next_request_seq++;
    ++connection.inflight;
    connection.command_queue.push_back(QueuedCommand{sequence, std::move(request)});
    if (!connection.strand_running) {
        launch_strand(connection);
    }
}

void Server::launch_strand(Connection& connection) {
    if (connection.command_queue.empty()) {
        connection.strand_running = false;
        return;
    }

    QueuedCommand item = std::move(connection.command_queue.front());
    connection.command_queue.pop_front();
    connection.strand_running = true;

    const std::uint64_t connection_id = connection.id;
    std::function<void()> job = [this, connection_id, item = std::move(item)]() mutable {
        commands::CommandContext context{
            .store = store_,
            .stats = &stats_,
            .persistence = persistence_.get(),
            .version = novacache::version(),
            .tcp_port = bound_port_,
            .worker_count = worker_count(),
        };
        protocol::RespValue response = registry_.execute(item.request, context);
        Completion completion{connection_id, item.sequence, protocol::encode(response),
                              response_is_error(response)};
        {
            const std::lock_guard lock{completion_mutex_};
            completions_.push_back(std::move(completion));
        }
        try {
            reactor_->wakeup();
        } catch (...) {
        }
    };

    if (!workers_->try_submit(job)) {
        // Run on the I/O thread when the pool is saturated.
        job();
    }
}

void Server::drain_completions() {
    for (;;) {
        std::deque<Completion> local;
        {
            const std::lock_guard lock{completion_mutex_};
            local.swap(completions_);
        }
        if (local.empty()) {
            return;
        }
        for (Completion& completion : local) {
            apply_completion(std::move(completion));
        }
    }
}

void Server::apply_completion(Completion completion) {
    int fd = -1;
    Connection* connection = nullptr;
    for (auto& [candidate_fd, connection_ptr] : connections_) {
        if (connection_ptr->id == completion.connection_id) {
            fd = candidate_fd;
            connection = connection_ptr.get();
            break;
        }
    }
    if (connection == nullptr) {
        return;
    }

    stats_.on_command(completion.error);
    connection->pending_responses.emplace(completion.sequence, std::move(completion.payload));
    if (connection->inflight > 0U) {
        --connection->inflight;
    }

    while (true) {
        const auto pending = connection->pending_responses.find(connection->next_response_seq);
        if (pending == connection->pending_responses.end()) {
            break;
        }
        if (connection->output.size() > config_.max_output_buffer_bytes ||
            pending->second.size() > config_.max_output_buffer_bytes - connection->output.size()) {
            connection->closing = true;
            connection->read_closed = true;
            connection->pending_responses.clear();
            break;
        }
        connection->output.append(pending->second);
        connection->pending_responses.erase(pending);
        ++connection->next_response_seq;
    }
    connection->last_activity = std::chrono::steady_clock::now();

    if (!connection->output.empty()) {
        write_connection(*connection);
    }
    if (!connections_.contains(fd)) {
        return;
    }

    // Continue the per-connection strand so pipelined commands stay ordered.
    if (!connection->command_queue.empty()) {
        launch_strand(*connection);
    } else {
        connection->strand_running = false;
    }

    if (connection->closing && connection->output.empty() && connection->inflight == 0U) {
        close_connection(fd);
        return;
    }
    update_interest(*connection);
}

void Server::write_connection(Connection& connection) {
    while (!connection.output.empty()) {
        const net::IoResult result = connection.socket.try_send(connection.output);
        if (result.status == net::IoStatus::would_block) {
            break;
        }
        if (result.status == net::IoStatus::closed || result.bytes == 0U) {
            connection.closing = true;
            connection.output.clear();
            break;
        }
        stats_.add_bytes_out(result.bytes);
        connection.output.erase(0, result.bytes);
        connection.last_activity = std::chrono::steady_clock::now();
    }
}

void Server::update_interest(Connection& connection) {
    const auto compute_interest = [&] {
        net::Interest interest = net::Interest::none;
        const bool want_read = !connection.read_closed && !connection.closing &&
                               !queue_saturated() &&
                               connection.output.size() < config_.max_output_buffer_bytes;
        if (want_read) {
            interest |= net::Interest::read;
        }
        if (!connection.output.empty()) {
            interest |= net::Interest::write;
        }
        return interest;
    };

    net::Interest interest = compute_interest();
    const bool enabling_read = has_interest(interest, net::Interest::read) &&
                               !has_interest(connection.interest, net::Interest::read);
    if (interest != connection.interest) {
        connection.interest = interest;
        reactor_->modify(connection.socket.native_handle(), interest);
    }
    if (enabling_read) {
        // Catch data that arrived while read interest was disabled (edge-triggered).
        read_connection(connection);
        interest = compute_interest();
        if (interest != connection.interest) {
            connection.interest = interest;
            reactor_->modify(connection.socket.native_handle(), interest);
        }
    }
}

void Server::close_connection(const int fd) {
    const auto iterator = connections_.find(fd);
    if (iterator == connections_.end()) {
        return;
    }
    try {
        reactor_->remove(fd);
    } catch (...) {
    }
    iterator->second->socket.shutdown();
    connections_.erase(iterator);
    stats_.on_disconnect();
}

void Server::run_active_expiry() {
    if (config_.active_expiry_hz <= 0) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now < next_expiry_tick_) {
        return;
    }
    const auto interval = std::chrono::milliseconds{1000 / std::max(1, config_.active_expiry_hz)};
    next_expiry_tick_ = now + interval;
    const std::size_t expired = store_.active_expire_cycle(20);
    if (expired > 0U) {
        stats_.add_expired(expired);
    }
}

void Server::close_idle_connections() {
    if (config_.idle_timeout <= std::chrono::seconds::zero()) {
        return;
    }
    const auto deadline = std::chrono::steady_clock::now() - config_.idle_timeout;
    std::vector<int> idle;
    for (const auto& [fd, connection] : connections_) {
        if (connection->last_activity < deadline && connection->inflight == 0U) {
            idle.push_back(fd);
        }
    }
    for (const int fd : idle) {
        close_connection(fd);
    }
}

std::size_t Server::worker_count() const noexcept {
    return workers_ == nullptr ? 0U : workers_->thread_count();
}

bool Server::queue_saturated() const {
    return workers_ != nullptr && workers_->queued_jobs() >= config_.worker_queue_limit;
}

} // namespace novacache::server
