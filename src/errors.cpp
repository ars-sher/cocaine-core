/*
    Copyright (c) 2011-2015 Andrey Sibiryov <me@kobology.ru>
    Copyright (c) 2011-2015 Other contributors as noted in the AUTHORS file.

    This file is part of Cocaine.

    Cocaine is free software; you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation; either version 3 of the License, or
    (at your option) any later version.

    Cocaine is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with this program. If not, see <http://www.gnu.org/licenses/>.
*/

#include "cocaine/errors.hpp"

#include "cocaine/detail/service/locator.hpp"
#include "cocaine/detail/service/node.hpp"
#include "cocaine/detail/isolate/archive.hpp"
#include "cocaine/detail/isolate/process.hpp"

#include <asio/error.hpp>

#include <boost/assign/list_of.hpp>

#include <boost/bimap.hpp>

using namespace cocaine;
using namespace cocaine::error;

namespace {

class unknown_category_t:
    public std::error_category
{
    virtual
    auto
    name() const throw() -> const char* {
        return "unknown category";
    }

    virtual
    auto
    message(int) const -> std::string {
        return "unknown category error";
    }
};

class transport_category_t:
    public std::error_category
{
    virtual
    auto
    name() const throw() -> const char* {
        return "cocaine.rpc.transport";
    }

    virtual
    auto
    message(int code) const -> std::string {
        if(code == cocaine::error::transport_errors::frame_format_error)
            return "message has an unexpected framing";
        if(code == cocaine::error::transport_errors::insufficient_bytes)
            return "insufficient bytes provided to decode the message";
        if(code == cocaine::error::transport_errors::parse_error)
            return "unable to parse the incoming data";

        return "cocaine.rpc.transport error";
    }
};

class dispatch_category_t:
    public std::error_category
{
    virtual
    auto
    name() const throw() -> const char* {
        return "cocaine.rpc.dispatch";
    }

    virtual
    auto
    message(int code) const -> std::string {
        if(code == cocaine::error::dispatch_errors::duplicate_slot)
            return "duplicate slot";
        if(code == cocaine::error::dispatch_errors::invalid_argument)
            return "unable to decode message arguments";
        if(code == cocaine::error::dispatch_errors::not_connected)
            return "session is detached";
        if(code == cocaine::error::dispatch_errors::revoked_channel)
            return "specified channel was revoked";
        if(code == cocaine::error::dispatch_errors::slot_not_found)
            return "specified slot is not bound";
        if(code == cocaine::error::dispatch_errors::unbound_dispatch)
            return "no dispatch has been assigned for channel";
        if(code == cocaine::error::dispatch_errors::uncaught_error)
            return "uncaught invocation exception";

        return "cocaine.rpc.dispatch error";
    }
};

class repository_category_t:
    public std::error_category
{
    virtual
    auto
    name() const throw() -> const char* {
        return "cocaine.plugins";
    }

    virtual
    auto
    message(int code) const -> std::string {
        if(code == cocaine::error::repository_errors::component_not_found)
            return "component is not available";
        if(code == cocaine::error::repository_errors::duplicate_component)
            return "duplicate component";
        if(code == cocaine::error::repository_errors::initialization_error)
            return "component has failed to intialize";
        if(code == cocaine::error::repository_errors::invalid_interface)
            return "component has an invalid interface";
        if(code == cocaine::error::repository_errors::ltdl_error)
            return "internal libltdl error";
        if(code == cocaine::error::repository_errors::version_mismatch)
            return "component version requirements are not met";

        return "cocaine.plugins error";
    }
};

class security_category_t:
    public std::error_category
{
    virtual
    auto
    name() const throw() -> const char* {
        return "cocaine.security";
    }

    virtual
    auto
    message(int code) const -> std::string {
        if(code == cocaine::error::security_errors::token_not_found)
            return "specified token is not available";

        return "cocaine.security error";
    }
};

auto
unknown_category() -> const std::error_category& {
    static unknown_category_t instance;
    return instance;
}

auto
transport_category() -> const std::error_category& {
    static transport_category_t instance;
    return instance;
}

auto
dispatch_category() -> const std::error_category& {
    static dispatch_category_t instance;
    return instance;
}

auto
repository_category() -> const std::error_category& {
    static repository_category_t instance;
    return instance;
}

auto
security_category() -> const std::error_category& {
    static security_category_t instance;
    return instance;
}

} // namespace

namespace cocaine { namespace error {

auto
make_error_code(transport_errors code) -> std::error_code {
    return std::error_code(static_cast<int>(code), transport_category());
}

auto
make_error_code(dispatch_errors code) -> std::error_code {
    return std::error_code(static_cast<int>(code), dispatch_category());
}

auto
make_error_code(repository_errors code) -> std::error_code {
    return std::error_code(static_cast<int>(code), repository_category());
}

auto
make_error_code(security_errors code) -> std::error_code {
    return std::error_code(static_cast<int>(code), security_category());
}

}} // namespace cocaine::error

// Error category registrar

struct
registrar::impl_type {
    struct uid_tag;
    struct ptr_tag;

    typedef boost::bimap<
        boost::bimaps::tagged<std::size_t,                 uid_tag>,
        boost::bimaps::tagged<std::error_category const *, ptr_tag>
    > mapping_t;

    mapping_t mapping;

    impl_type();
};

registrar::impl_type::impl_type() {
    mapping = boost::assign::list_of<mapping_t::relation>
        (0x0, &unknown_category()                  )
        (0x1, &std::system_category()              )
        (0x2, &asio::error::get_system_category()  )
        (0x3, &asio::error::get_netdb_category()   )
        (0x4, &asio::error::get_addrinfo_category())
        (0x5, &asio::error::get_misc_category()    )
        (0x6, &transport_category()                )
        (0x7, &dispatch_category()                 )
        (0x8, &repository_category()               )
        (0x9, &security_category()                 )
        (0xA, &locator_category()                  )
        (0xB, &node_category()                     )
    #ifdef COCAINE_ALLOW_CGROUPS
        (0xC, &cgroup_category()                   )
    #endif
        (0xD, &archive_category()                  );
}

synchronized<std::unique_ptr<registrar::impl_type>> registrar::ptr(std::make_unique<impl_type>());

bool
registrar::add(const std::error_category& ec) {
    return ptr.apply([&](std::unique_ptr<impl_type>& impl) -> bool {
        return impl->mapping.insert({impl->mapping.size(), &ec}).second;
    });
}

auto
registrar::map(const std::error_category& ec) -> size_t {
    return ptr.apply([&](const std::unique_ptr<impl_type>& impl) -> size_t {
        if(impl->mapping.by<impl_type::ptr_tag>().count(&ec) == 0) {
            return 0;
        } else {
            return impl->mapping.by<impl_type::ptr_tag>().at(&ec);
        }
    });
}

auto
registrar::map(size_t id) -> const std::error_category& {
    return ptr.apply([&](const std::unique_ptr<impl_type>& impl) -> const std::error_category& {
        if(impl->mapping.by<impl_type::uid_tag>().count(id) == 0) {
            return unknown_category();
        } else {
            return *impl->mapping.by<impl_type::uid_tag>().at(id);
        }
    });
}
