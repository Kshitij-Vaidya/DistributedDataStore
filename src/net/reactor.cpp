#include "novacache/net/reactor.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <stdexcept>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(__linux__)
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
#include <sys/event.h>
#include <sys/time.h>
#include <unistd.h>
#else
#error "NovaCache requires epoll on Linux or kqueue on BSD/macOS"
#endif

namespace novacache::net {
namespace {

[[noreturn]] void throw_os_error(const char* operation) {
    throw std::system_error(errno, std::generic_category(), operation);
}

#if defined(__linux__)

class EpollReactor final : public Reactor {
  public:
    EpollReactor() {
        epoll_fd_ = ::epoll_create1(EPOLL_CLOEXEC);
        if (epoll_fd_ < 0) {
            throw_os_error("epoll_create1");
        }
        wakeup_fd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (wakeup_fd_ < 0) {
            const int saved = errno;
            static_cast<void>(::close(epoll_fd_));
            throw std::system_error(saved, std::generic_category(), "eventfd");
        }
        epoll_event event{};
        event.events = EPOLLIN;
        event.data.fd = wakeup_fd_;
        if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, wakeup_fd_, &event) != 0) {
            const int saved = errno;
            static_cast<void>(::close(wakeup_fd_));
            static_cast<void>(::close(epoll_fd_));
            throw std::system_error(saved, std::generic_category(), "epoll_ctl(wakeup)");
        }
    }

    ~EpollReactor() override {
        if (wakeup_fd_ >= 0) {
            static_cast<void>(::close(wakeup_fd_));
        }
        if (epoll_fd_ >= 0) {
            static_cast<void>(::close(epoll_fd_));
        }
    }

    void add(const int fd, const Interest interest) override {
        epoll_event event = make_event(fd, interest);
        if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &event) != 0) {
            throw_os_error("epoll_ctl(ADD)");
        }
    }

    void modify(const int fd, const Interest interest) override {
        epoll_event event = make_event(fd, interest);
        if (::epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &event) != 0) {
            throw_os_error("epoll_ctl(MOD)");
        }
    }

    void remove(const int fd) override {
        if (::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr) != 0 && errno != ENOENT) {
            throw_os_error("epoll_ctl(DEL)");
        }
    }

    std::size_t wait(const std::span<FiredEvent> out,
                     const std::optional<std::chrono::milliseconds> timeout) override {
        if (out.empty()) {
            return 0;
        }
        std::vector<epoll_event> events(out.size());
        int timeout_ms = -1;
        if (timeout.has_value()) {
            timeout_ms = static_cast<int>(
                std::min<std::chrono::milliseconds::rep>(timeout->count(), 2'147'483'647));
        }

        int ready = 0;
        for (;;) {
            ready =
                ::epoll_wait(epoll_fd_, events.data(), static_cast<int>(events.size()), timeout_ms);
            if (ready >= 0) {
                break;
            }
            if (errno != EINTR) {
                throw_os_error("epoll_wait");
            }
        }

        std::size_t produced = 0;
        for (int index = 0; index < ready; ++index) {
            const epoll_event& event = events[static_cast<std::size_t>(index)];
            if (event.data.fd == wakeup_fd_) {
                drain_wakeup();
                continue;
            }
            FiredEvent& fired = out[produced++];
            fired.fd = event.data.fd;
            fired.ready = Interest::none;
            if ((event.events & EPOLLIN) != 0U) {
                fired.ready |= Interest::read;
            }
            if ((event.events & EPOLLOUT) != 0U) {
                fired.ready |= Interest::write;
            }
            fired.error = (event.events & EPOLLERR) != 0U;
            fired.hangup = (event.events & EPOLLHUP) != 0U;
        }
        return produced;
    }

    void wakeup() override {
        constexpr std::uint64_t one = 1;
        for (;;) {
            const ssize_t written = ::write(wakeup_fd_, &one, sizeof(one));
            if (written == static_cast<ssize_t>(sizeof(one)) || (written < 0 && errno == EAGAIN)) {
                return;
            }
            if (written < 0 && errno == EINTR) {
                continue;
            }
            throw_os_error("eventfd write");
        }
    }

  private:
    [[nodiscard]] static epoll_event make_event(const int fd, const Interest interest) {
        epoll_event event{};
        event.data.fd = fd;
        event.events = EPOLLET;
        if (has_interest(interest, Interest::read)) {
            event.events |= EPOLLIN;
        }
        if (has_interest(interest, Interest::write)) {
            event.events |= EPOLLOUT;
        }
        return event;
    }

    void drain_wakeup() const {
        std::uint64_t value = 0;
        while (::read(wakeup_fd_, &value, sizeof(value)) > 0) {
        }
    }

    int epoll_fd_ = -1;
    int wakeup_fd_ = -1;
};

#else

class KqueueReactor final : public Reactor {
  public:
    static constexpr uintptr_t wakeup_ident = 1;

    KqueueReactor() {
        kq_ = ::kqueue();
        if (kq_ < 0) {
            throw_os_error("kqueue");
        }

        struct kevent change;
        EV_SET(&change, wakeup_ident, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, nullptr);
        if (::kevent(kq_, &change, 1, nullptr, 0, nullptr) != 0) {
            const int saved = errno;
            static_cast<void>(::close(kq_));
            throw std::system_error(saved, std::generic_category(), "kevent(EVFILT_USER ADD)");
        }
    }

    ~KqueueReactor() override {
        if (kq_ >= 0) {
            static_cast<void>(::close(kq_));
        }
    }

    void add(const int fd, const Interest interest) override { apply(fd, interest, true); }

    void modify(const int fd, const Interest interest) override { apply(fd, interest, false); }

    void remove(const int fd) override {
        struct kevent changes[2]{};
        EV_SET(&changes[0], static_cast<uintptr_t>(fd), EVFILT_READ, EV_DELETE, 0, 0, nullptr);
        EV_SET(&changes[1], static_cast<uintptr_t>(fd), EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
        for (struct kevent& change : changes) {
            if (::kevent(kq_, &change, 1, nullptr, 0, nullptr) != 0 && errno != ENOENT) {
                throw_os_error("kevent(DELETE)");
            }
        }
        registered_.erase(fd);
    }

    std::size_t wait(const std::span<FiredEvent> out,
                     const std::optional<std::chrono::milliseconds> timeout) override {
        if (out.empty()) {
            return 0;
        }
        std::vector<struct kevent> events(out.size());
        timespec timeout_value{};
        const timespec* timeout_ptr = nullptr;
        if (timeout.has_value()) {
            timeout_value.tv_sec = static_cast<time_t>(timeout->count() / 1000);
            timeout_value.tv_nsec = static_cast<long>((timeout->count() % 1000) * 1'000'000L);
            timeout_ptr = &timeout_value;
        }

        int ready = 0;
        for (;;) {
            ready = ::kevent(kq_, nullptr, 0, events.data(), static_cast<int>(events.size()),
                             timeout_ptr);
            if (ready >= 0) {
                break;
            }
            if (errno != EINTR) {
                throw_os_error("kevent(wait)");
            }
        }

        std::size_t produced = 0;
        for (int index = 0; index < ready; ++index) {
            const struct kevent& event = events[static_cast<std::size_t>(index)];
            if (event.filter == EVFILT_USER) {
                continue;
            }
            FiredEvent* slot = nullptr;
            for (std::size_t existing = 0; existing < produced; ++existing) {
                if (out[existing].fd == static_cast<int>(event.ident)) {
                    slot = &out[existing];
                    break;
                }
            }
            if (slot == nullptr) {
                slot = &out[produced++];
                slot->fd = static_cast<int>(event.ident);
                slot->ready = Interest::none;
                slot->error = false;
                slot->hangup = false;
            }
            if (event.filter == EVFILT_READ) {
                slot->ready |= Interest::read;
            } else if (event.filter == EVFILT_WRITE) {
                slot->ready |= Interest::write;
            }
            if ((event.flags & EV_ERROR) != 0U) {
                slot->error = true;
            }
            if ((event.flags & EV_EOF) != 0U) {
                slot->hangup = true;
            }
        }
        return produced;
    }

    void wakeup() override {
        struct kevent change;
        EV_SET(&change, wakeup_ident, EVFILT_USER, 0, NOTE_TRIGGER, 0, nullptr);
        if (::kevent(kq_, &change, 1, nullptr, 0, nullptr) != 0) {
            throw_os_error("kevent(NOTE_TRIGGER)");
        }
    }

  private:
    void apply(const int fd, const Interest interest, const bool) {
        const Interest previous = registered_.contains(fd) ? registered_.at(fd) : Interest::none;
        struct kevent changes[4]{};
        int count = 0;

        const auto ensure = [&](const Interest flag, const short filter) {
            const bool want = has_interest(interest, flag);
            const bool had = has_interest(previous, flag);
            if (want && !had) {
                EV_SET(&changes[count++], static_cast<uintptr_t>(fd), filter, EV_ADD | EV_CLEAR, 0,
                       0, nullptr);
            } else if (!want && had) {
                EV_SET(&changes[count++], static_cast<uintptr_t>(fd), filter, EV_DELETE, 0, 0,
                       nullptr);
            }
        };
        ensure(Interest::read, EVFILT_READ);
        ensure(Interest::write, EVFILT_WRITE);

        if (count > 0 && ::kevent(kq_, changes, count, nullptr, 0, nullptr) != 0) {
            throw_os_error("kevent(apply)");
        }
        if (interest == Interest::none) {
            registered_.erase(fd);
        } else {
            registered_[fd] = interest;
        }
    }

    int kq_ = -1;
    std::unordered_map<int, Interest> registered_;
};

#endif

} // namespace

std::unique_ptr<Reactor> create_reactor() {
#if defined(__linux__)
    return std::make_unique<EpollReactor>();
#else
    return std::make_unique<KqueueReactor>();
#endif
}

} // namespace novacache::net
