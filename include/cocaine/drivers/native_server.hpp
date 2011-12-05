#ifndef COCAINE_DRIVER_NATIVE_SERVER_HPP
#define COCAINE_DRIVER_NATIVE_SERVER_HPP

#include "cocaine/drivers/zeromq_server.hpp"

namespace cocaine { namespace engine { namespace driver {

namespace messages {
    enum types {
        request = 1,
        tag,
        error
    };

    struct request_t {
        unsigned int type;
        unique_id_t::type id;
        job::policy_t policy;

        MSGPACK_DEFINE(type, id, policy);
    };

    struct tag_t {
        tag_t(const unique_id_t::type& id_, bool completed_ = false):
            type(tag),
            id(id_),
            completed(completed_)
        { }

        unsigned int type;
        unique_id_t::type id;
        bool completed;

        MSGPACK_DEFINE(type, id, completed);
    };
    
    struct error_t {
        error_t(const unique_id_t::type& id_, unsigned int code_, const std::string& message_):
            type(error),
            id(id_),
            code(code_),
            message(message_)
        { }

        unsigned int type;
        unique_id_t::type id;
        unsigned int code;
        std::string message;

        MSGPACK_DEFINE(type, id, code, message);
    };
    
}

class native_server_t;

class native_server_job_t:
    public unique_id_t,
    public job::job_t
{
    public:
        native_server_job_t(native_server_t* driver,
                            const messages::request_t& request,
                            const networking::route_t& route);

        virtual void react(const events::response_t& event);
        virtual void react(const events::error_t& event);
        virtual void react(const events::choked_t& event);

    private:
        template<class T>
        bool send(const T& response, int flags = 0) {
            zmq::message_t message;
            zeromq_server_t* server = static_cast<zeromq_server_t*>(m_driver);

            // Send the identity
            for(networking::route_t::const_iterator id = m_route.begin(); id != m_route.end(); ++id) {
                message.rebuild(id->size());
                memcpy(message.data(), id->data(), id->size());

                if(!server->socket().send(message, ZMQ_SNDMORE)) {
                    return false;
                }
            }

            // Send the delimiter
            message.rebuild(0);

            if(!server->socket().send(message, ZMQ_SNDMORE)) {
                return false;
            }

            // Send the response
            return server->socket().send_multi(
                boost::tie(
                    response.type,
                    response
                ),
                flags
            );
        }

    private:
        const networking::route_t m_route;
};

class native_server_t:
    public zeromq_server_t
{
    public:
        native_server_t(engine_t* engine,
                        const std::string& method, 
                        const Json::Value& args);

        // Driver interface
        virtual Json::Value info() const;
        
    private:
        // Server interface
        virtual void process(ev::idle&, int);
};

}}}

#endif
