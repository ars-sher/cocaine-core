#pragma once

#include <functional>

#include <boost/optional/optional.hpp>

#include "cocaine/rpc/dispatch.hpp"
#include "cocaine/rpc/slot/streamed.hpp"
#include "cocaine/rpc/upstream.hpp"

#include "cocaine/idl/node.hpp"
#include "cocaine/idl/rpc.hpp"

namespace cocaine {

class state_machine_t;

class channel_t;

/// An adapter for [Client -> Worker] message passing.
class client_rpc_dispatch_t:
    public dispatch<io::event_traits<io::app::enqueue>::dispatch_type>
{
    typedef io::event_traits<io::app::enqueue>::dispatch_type incoming_tag;
    typedef io::event_traits<io::worker::rpc::invoke>::dispatch_type outcoming_tag;
    typedef io::protocol<incoming_tag>::scope protocol;

    enum class state_t {
        /// The dispatch is collecting messages into the queue.
        open,
        /// The dispatch is delegating incoming messages to the attached upstream.
        bound,
        /// The dispatch is closed normally or abnormally depending on `ec` variable.
        closed
    };

    /// Current state.
    state_t state;

    /// Upstream to the worker.
    streamed<std::string> stream;

    std::shared_ptr<channel_t> control;

    /// Closed state error code.
    ///
    /// Non-null error code means that the dispatch is closed abnormally, i.e. client has been
    /// disconnected without closing its channels.
    std::error_code ec;

    std::mutex mutex;

public:
    explicit
    client_rpc_dispatch_t(const std::string& name);

    void
    attach(upstream<outcoming_tag> stream, std::shared_ptr<channel_t> control);

    /// Discards this part of bidirectional channel.
    ///
    /// Usually this method is called when the client has been disconnected without closing its
    /// opened channels.
    /// In this case we should call the close callback given earlier to prevend resource leak.
    void
    discard(const std::error_code& ec);

    virtual
    void
    discard(const std::error_code& ec) const;

private:
    void
    finalize();

    void
    finalize(const std::shared_ptr<channel_t>& control, const std::error_code& ec);

    void
    update_activity();
};

} // namespace cocaine
