#include "cocaine/detail/service/node/overseer.hpp"

#include <blackhole/scoped_attributes.hpp>

#include "cocaine/context.hpp"

#include "cocaine/detail/service/node/manifest.hpp"
#include "cocaine/detail/service/node/profile.hpp"

#include "cocaine/detail/service/node/balancing/base.hpp"
#include "cocaine/detail/service/node/balancing/null.hpp"
#include "cocaine/detail/service/node/dispatch/client.hpp"
#include "cocaine/detail/service/node/dispatch/handshaker.hpp"
#include "cocaine/detail/service/node/dispatch/worker.hpp"
#include "cocaine/detail/service/node/slave/control.hpp"
#include "cocaine/detail/service/node/slot.hpp"

#include <boost/accumulators/statistics/extended_p_square.hpp>

#include <boost/range/adaptors.hpp>
#include <boost/range/algorithm.hpp>

namespace ph = std::placeholders;

using namespace cocaine;

struct collector_t {
    std::size_t active;
    std::size_t cumload;

    explicit
    collector_t(const overseer_t::pool_type& pool):
        active{},
        cumload{}
    {
        for (const auto& it : pool) {
            const auto load = it.second.load();
            if (it.second.active() && load) {
                active++;
                cumload += load;
            }
        }
    }
};

overseer_t::overseer_t(context_t& context,
                       manifest_t manifest,
                       profile_t profile,
                       std::shared_ptr<asio::io_service> loop):
    log(context.log(format("%s/overseer", manifest.name))),
    context(context),
    birthstamp(std::chrono::high_resolution_clock::now()),
    manifest(std::move(manifest)),
    profile_(profile),
    loop(loop),
    stats{}
{
    COCAINE_LOG_TRACE(log, "overseer has been initialized");
}

overseer_t::~overseer_t() {
    COCAINE_LOG_TRACE(log, "overseer has been destroyed");
}

profile_t
overseer_t::profile() const {
    return *profile_.synchronize();
}

locked_ptr<overseer_t::pool_type>
overseer_t::get_pool() {
    return pool.synchronize();
}

locked_ptr<overseer_t::queue_type>
overseer_t::get_queue() {
    return queue.synchronize();
}

namespace keys {

const std::string UPTIME   = "uptime";
const std::string REQUESTS = "requests";
const std::string QUEUE    = "queue";
const std::string TIMINGS  = "timings";
const std::string POOL     = "pool";
const std::string LOAD     = "load";

namespace requests {
    const std::string ACCEPTED = "accepted";
    const std::string REJECTED = "rejected";
} // namespace requests

namespace queue {
    const std::string CAPACITY         = "capacity";
    const std::string DEPTH            = "depth";
    const std::string OLDEST_EVENT_AGE = "oldest_event_age";
} // namespace queue

namespace pool {
    const std::string ACTIVE   = "active";
    const std::string IDLE     = "idle";
    const std::string CAPACITY = "capacity";
    const std::string SLAVES   = "slaves";
} // namespace pool

namespace slaves {
    const std::string TX                    = "load:tx";
    const std::string RX                    = "load:rx";
    const std::string LOAD                  = "load:total";
    const std::string ACCEPTED              = "accepted";
    const std::string OLDEST_CHANNEL_AGE    = "oldest_channel_age";
    const std::string LAST_CHANNEL_ACTIVITY = "last_channel_activity";
    const std::string STATE                 = "state";
    const std::string UPTIME                = "uptime";
} // namespace slaves

} // namespace keys

dynamic_t::object_t
overseer_t::info() const {
    dynamic_t::object_t result;

    const auto now = std::chrono::high_resolution_clock::now();

    // Prepare application specific info.
    result[keys::UPTIME] = std::chrono::duration_cast<
        std::chrono::seconds
    >(now - birthstamp).count();

    // Prepare channels info.
    {
        dynamic_t::object_t stat;
        stat[keys::requests::ACCEPTED] = stats.accepted.load();
        stat[keys::requests::REJECTED] = stats.rejected.load();

        result[keys::REQUESTS] = stat;
    }

    // Pending events queue info.
    {
        dynamic_t::object_t stat;
        stat[keys::queue::CAPACITY] = profile().queue_limit;

        queue.apply([&](const queue_type& queue) {
            stat[keys::queue::DEPTH] = queue.size();

            typedef queue_type::value_type value_type;

            if (queue.empty()) {
                stat[keys::queue::OLDEST_EVENT_AGE] = 0;
            } else {
                const auto min = *boost::min_element(queue |
                    boost::adaptors::transformed(+[](const value_type& cur) {
                        return cur.event.birthstamp;
                    })
                );

                const auto age = std::chrono::duration_cast<
                    std::chrono::milliseconds
                >(now - min).count();

                stat[keys::queue::OLDEST_EVENT_AGE] = age;
            }
        });

        result[keys::QUEUE] = stat;
    }

    // Response time quantiles over all events (not sure it makes sence).
    {
        dynamic_t::object_t stat;

        std::array<char, 16> buf;
        for (const auto& quantile : stats.quantiles()) {
            if (std::snprintf(buf.data(), buf.size(), "%.2f%%", quantile.probability)) {
                stat[buf.data()] = quantile.value;
            }
        }

        result[keys::TIMINGS] = stat;
    }

    // Current and cumulative slaves info.
    pool.apply([&](const pool_type& pool) {
        const collector_t collector(pool);

        dynamic_t::object_t sstat;
        for (const auto& kv : pool) {
            const auto& id    = kv.first;
            const auto& slave = kv.second;

            const auto sstats = slave.stats();

            dynamic_t::object_t stat;
            stat[keys::slaves::TX]       = sstats.tx;
            stat[keys::slaves::RX]       = sstats.rx;
            stat[keys::slaves::LOAD]     = sstats.load;
            stat[keys::slaves::ACCEPTED] = sstats.total;
            stat[keys::slaves::STATE]    = slave.state();
            stat[keys::slaves::UPTIME]   = slave.uptime();

            if (sstats.age) {
                const auto duration = std::chrono::duration_cast<
                    std::chrono::milliseconds
                >(now - *sstats.age).count();
                stat[keys::slaves::OLDEST_CHANNEL_AGE] = duration;
            } else {
                stat[keys::slaves::OLDEST_CHANNEL_AGE] = dynamic_t::null;
            }

            if (sstats.activity) {
                const auto duration = std::chrono::duration_cast<
                    std::chrono::seconds
                >(now - *sstats.activity).count();
                stat[keys::slaves::LAST_CHANNEL_ACTIVITY] = duration;
            } else {
                stat[keys::slaves::LAST_CHANNEL_ACTIVITY] = dynamic_t::null;
            }

            sstat[id] = stat;
        }

        // Slaves pool info.
        {
            dynamic_t::object_t stat;

            stat[keys::pool::ACTIVE]   = collector.active;
            stat[keys::pool::IDLE]     = pool.size() - collector.active;
            stat[keys::pool::CAPACITY] = profile().pool_limit;
            stat[keys::pool::SLAVES]   = sstat;

            result[keys::POOL] = stat;
        }

        // Cumulative load on the app over all the slaves.
        result[keys::LOAD] = collector.cumload;
    });


    return result;
}

void
overseer_t::balance(std::unique_ptr<balancer_t> balancer) {
    if (balancer) {
        this->balancer = std::move(balancer);
    } else {
        this->balancer.reset(new null_balancer_t);
    }
}

std::shared_ptr<client_rpc_dispatch_t>
overseer_t::enqueue(io::streaming_slot<io::app::enqueue>::upstream_type&& downstream,
                    app::event_t event,
                    boost::optional<service::node::slave::id_t> /*id*/)
{
    // TODO: Handle id parameter somehow.

    queue.apply([&](queue_type& queue) {
        const auto limit = profile().queue_limit;

        if (queue.size() >= limit && limit > 0) {
            ++stats.rejected;
            throw std::system_error(error::queue_is_full);
        }
    });

    auto dispatch = std::make_shared<client_rpc_dispatch_t>(manifest.name);

    queue->push_back({
        std::move(event),
        dispatch,
        std::move(downstream),
    });

    ++stats.accepted;
    balancer->on_queue();

    return dispatch;
}

io::dispatch_ptr_t
overseer_t::prototype() {
    return std::make_shared<const handshaker_t>(
        manifest.name,
        std::bind(&overseer_t::on_handshake, shared_from_this(), ph::_1, ph::_2, ph::_3)
    );
}

void
overseer_t::spawn() {
    spawn(pool.synchronize());
}

void
overseer_t::spawn(locked_ptr<pool_type>& pool) {
    COCAINE_LOG_INFO(log, "enlarging the slaves pool to %d", pool->size() + 1);

    slave_context ctx(context, manifest, profile());

    // It is guaranteed that the cleanup handler will not be invoked from within the slave's
    // constructor.
    const auto uuid = ctx.id;
    pool->insert(std::make_pair(
        uuid,
        slave_t(std::move(ctx), *loop, std::bind(&overseer_t::on_slave_death, shared_from_this(), ph::_1, uuid))
    ));
}

void
overseer_t::spawn(locked_ptr<pool_type>&& pool) {
    spawn(pool);
}

void
overseer_t::assign(slave_t& slave, slave::channel_t& payload) {
    // Attempts to inject the new channel into the slave.
    const auto id = slave.id();
    const auto timestamp = payload.event.birthstamp;

    // TODO: Race possible.
    const auto channel = slave.inject(payload, [=](std::uint64_t channel) {
        const auto now = std::chrono::high_resolution_clock::now();
        const auto elapsed = std::chrono::duration<
            double,
            std::chrono::milliseconds::period
        >(now - timestamp).count();

        stats.timings.apply([&](stats_t::quantiles_t& timings) {
            timings(elapsed);
        });

        // TODO: Hack, but at least it saves from the deadlock.
        loop->post(std::bind(&balancer_t::on_channel_finished, balancer, id, channel));
    });

    balancer->on_channel_started(id, channel);
}

void
overseer_t::despawn(const std::string& id, despawn_policy_t policy) {
    pool.apply([&](pool_type& pool) {
        auto it = pool.find(id);
        if (it != pool.end()) {
            switch (policy) {
            case despawn_policy_t::graceful:
                it->second.seal();
                break;
            case despawn_policy_t::force:
                pool.erase(it);
                balancer->on_slave_death(id);
                break;
            default:
                BOOST_ASSERT(false);
            }
        }
    });
}

void
overseer_t::terminate() {
    COCAINE_LOG_DEBUG(log, "overseer is processing terminate request");

    balance();
    pool->clear();
}

std::shared_ptr<control_t>
overseer_t::on_handshake(const std::string& id,
                         std::shared_ptr<session_t> session,
                         upstream<io::worker::control_tag>&& stream)
{
    blackhole::scoped_attributes_t holder(*log, {{ "uuid", id }});

    COCAINE_LOG_DEBUG(log, "processing handshake message");

    auto control = pool.apply([&](pool_type& pool) -> std::shared_ptr<control_t> {
        auto it = pool.find(id);
        if (it == pool.end()) {
            COCAINE_LOG_DEBUG(log, "rejecting slave as unexpected");
            return nullptr;
        }

        COCAINE_LOG_DEBUG(log, "activating slave");
        try {
            return it->second.activate(std::move(session), std::move(stream));
        } catch (const std::exception& err) {
            // The slave can be in invalid state; broken, for example, or because the overseer is
            // overloaded. In fact I hope it never happens.
            // Also unlikely we can receive here std::bad_alloc if unable to allocate more memory
            // for control dispatch.
            // If this happens the session will be closed.
            COCAINE_LOG_ERROR(log, "failed to activate the slave: %s", err.what());
        }

        return nullptr;
    });

    if (control) {
        balancer->on_slave_spawn(id);
    }

    return control;
}

void
overseer_t::on_slave_death(const std::error_code& ec, std::string uuid) {
    if (ec) {
        COCAINE_LOG_DEBUG(log, "slave has removed itself from the pool: %s", ec.message());
    } else {
        COCAINE_LOG_DEBUG(log, "slave has removed itself from the pool");
    }

    pool.apply([&](pool_type& pool) {
        auto it = pool.find(uuid);
        if (it != pool.end()) {
            it->second.terminate(ec);
            pool.erase(it);
        }
    });
    balancer->on_slave_death(uuid);
}
