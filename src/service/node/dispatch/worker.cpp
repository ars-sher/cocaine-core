#include "cocaine/detail/service/node/dispatch/worker.hpp"

#include "cocaine/detail/service/node/slave/channel.hpp"

using namespace cocaine;

worker_rpc_dispatch_t::worker_rpc_dispatch_t(upstream<outcoming_tag>& stream_, std::shared_ptr<channel_t> control_):
    dispatch<incoming_tag>("W2C"),
    stream(std::move(stream_)),
    state(state_t::open),
    control(std::move(control_))
{
    on<protocol::chunk>([&](const std::string& chunk) {
        control->update_rx_activity();

        try {
            stream = stream.send<protocol::chunk>(chunk);
        } catch (const std::system_error&) {
            finalize(asio::error::connection_aborted);
        }
    });

    on<protocol::error>([&](const std::error_code& ec, const std::string& reason) {
        control->update_rx_activity();

        try {
            stream.send<protocol::error>(ec, reason);
            finalize();
        } catch (const std::system_error&) {
            finalize(asio::error::connection_aborted);
        }
    });

    on<protocol::choke>([&]() {
        control->update_rx_activity();

        try {
            stream.send<protocol::choke>();
            finalize();
        } catch (const std::system_error&) {
            finalize(asio::error::connection_aborted);
        }
    });
}

void
worker_rpc_dispatch_t::finalize(const std::error_code& ec) {
    std::lock_guard<std::mutex> lock(mutex);

    // Ensure that we call this method only once no matter what.
    if (state == state_t::closed) {
        // TODO: Log the error.
        return;
    }

    state = state_t::closed;

    // TODO: We have to send the error to the worker on any error occurred while writing to the
    // client stream, otherwise it may push its messages to the Hell forever.
    if (ec) {
        control->close_both();
    } else {
        control->close_recv();
    }

    control.reset();
}
