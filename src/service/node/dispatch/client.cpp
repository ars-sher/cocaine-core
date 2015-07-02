#include "cocaine/detail/service/node/dispatch/client.hpp"

#include "cocaine/detail/service/node/slave.hpp"
#include "cocaine/detail/service/node/slave/channel.hpp"

using namespace cocaine;

client_rpc_dispatch_t::client_rpc_dispatch_t(const std::string& name):
    dispatch<incoming_tag>(format("%s/C2W", name)),
    state(state_t::open)
{
    // Uncaught exceptions here will lead to a client disconnection and further dispatch discarding.

    on<protocol::chunk>([&](const std::string& chunk) {
        update_activity();

        stream.write(chunk);
    });

    on<protocol::error>([&](const std::error_code& ec, const std::string& reason) {
        update_activity();

        stream.abort(ec, reason);
        finalize();
    });

    on<protocol::choke>([&] {
        update_activity();

        stream.close();
        finalize();
    });
}

void
client_rpc_dispatch_t::attach(upstream<outcoming_tag> stream_, std::shared_ptr<channel_t> control_) {
    std::lock_guard<std::mutex> lock(mutex);

    stream.attach(std::move(stream_));

    switch (state) {
    case state_t::open:
        state = state_t::bound;
        control = std::move(control_);
        control->update_tx_activity();
        break;
    case state_t::closed:
        finalize(control_, ec);
        break;
    default:
        BOOST_ASSERT(false);
    }
}

void
client_rpc_dispatch_t::discard(const std::error_code& ec) const {
    // TODO: Consider something less weird.
    const_cast<client_rpc_dispatch_t*>(this)->discard(ec);
}

void
client_rpc_dispatch_t::discard(const std::error_code& ec) {
    if (ec) {
        this->ec = ec;

        try {
            // TODO: Add category to indicate that the error is generated by the core.
            stream.abort(ec, ec.message());
        } catch (const std::exception&) {
            // Eat.
        }

        finalize();
    }
}

void
client_rpc_dispatch_t::finalize() {
    std::lock_guard<std::mutex> lock(mutex);

    switch (state) {
    case state_t::open:
        state = state_t::closed;
        break;
    case state_t::bound:
        state = state_t::closed;
        finalize(control, ec);
        control.reset();
        break;
    case state_t::closed:
        break;
    default:
        BOOST_ASSERT(false);
    }
}

void
client_rpc_dispatch_t::finalize(const std::shared_ptr<channel_t>& control, const std::error_code& ec) {
    if (ec) {
        control->close_both();
    } else {
        control->close_send();
    }
}

void
client_rpc_dispatch_t::update_activity() {
    std::lock_guard<std::mutex> lock(mutex);
    if (control) {
        control->update_tx_activity();
    }
}
