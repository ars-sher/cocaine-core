#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <functional>
#include <system_error>

namespace cocaine {

class channel_t:
    public std::enable_shared_from_this<channel_t>
{
public:
    typedef std::function<void()> callback_type;
    typedef std::chrono::high_resolution_clock clock_type;
    typedef clock_type::time_point             time_point;

private:
    enum side_t {
        none = 0x00,
        tx = 0x01,
        rx = 0x02,
        both = tx | rx
    };

    const std::uint64_t id;
    const time_point birthstamp_;
    const callback_type callback;

    /// Time points of last activity separated by channel direction.
    time_point last_tx_activity;
    time_point last_rx_activity;

    std::atomic<int> state;
    bool watched;
    std::mutex mutex;

public:
    channel_t(std::uint64_t id, time_point birthstamp, callback_type callback);

    time_point
    birthstamp() const;

    bool
    closed() const;

    bool
    send_closed() const;

    bool
    recv_closed() const;

    void
    watch();

    void
    close_send();

    void
    close_recv();

    void
    close_both();

    void
    update_tx_activity() noexcept;

    void
    update_rx_activity() noexcept;

    time_point
    last_activity() const;

private:
    void
    maybe_notify();
};

}
