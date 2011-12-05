#include "cocaine/drivers/base.hpp"
#include "cocaine/job.hpp"

using namespace cocaine::engine::job;

policy_t::policy_t():
    urgent(false),
    timeout(config_t::get().engine.heartbeat_timeout),
    deadline(0.0f)
{ }

policy_t::policy_t(bool urgent_, ev::tstamp timeout_, ev::tstamp deadline_):
    urgent(urgent_),
    timeout(timeout_),
    deadline(deadline_)
{ }

job_t::job_t(driver::driver_t* driver, policy_t policy):
    m_driver(driver),
    m_policy(policy)
{
    if(m_policy.deadline) {
        m_expiration_timer.set<job_t, &job_t::discard>(this);
        m_expiration_timer.start(m_policy.deadline);
    }

    initiate();
}

job_t::~job_t() {
    m_expiration_timer.stop();

    // TEST: Ensure that the job has been completed
    BOOST_ASSERT(state_downcast<const complete*>() != 0);

    terminate();
}

void job_t::discard(ev::periodic&, int) {
    process_event(events::timeout_t("the job has expired"));
}

waiting::waiting():
    m_timestamp(ev::get_default_loop().now())
{ }

waiting::~waiting() {
    context<job_t>().driver()->audit(driver::in_queue,
        ev::get_default_loop().now() - m_timestamp);
}

processing::processing():
    m_timestamp(ev::get_default_loop().now())
{ }

processing::~processing() {
    context<job_t>().driver()->audit(driver::on_slave,
        ev::get_default_loop().now() - m_timestamp);
}

