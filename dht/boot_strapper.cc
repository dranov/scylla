/*
 *
 * Modified by ScyllaDB
 * Copyright (C) 2015-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: (AGPL-3.0-or-later and Apache-2.0)
 */

#include <boost/algorithm/string/split.hpp>
#include <boost/algorithm/string/erase.hpp>
#include <boost/algorithm/string/classification.hpp>

#include <seastar/core/coroutine.hh>

#include "dht/boot_strapper.hh"
#include "dht/range_streamer.hh"
#include "gms/failure_detector.hh"
#include "gms/gossiper.hh"
#include "log.hh"
#include "db/config.hh"
#include "replica/database.hh"
#include "streaming/stream_reason.hh"
#include "locator/abstract_replication_strategy.hh"

static logging::logger blogger("boot_strapper");

namespace dht {

future<> boot_strapper::bootstrap(streaming::stream_reason reason, gms::gossiper& gossiper, inet_address replace_address) {
    blogger.debug("Beginning bootstrap process: sorted_tokens={}", get_token_metadata().sorted_tokens());
    sstring description;
    if (reason == streaming::stream_reason::bootstrap) {
        description = "Bootstrap";
    } else if (reason == streaming::stream_reason::replace) {
        description = "Replace";
    } else {
        throw std::runtime_error("Wrong stream_reason provided: it can only be replace or bootstrap");
    }
    try {
        auto streamer = make_lw_shared<range_streamer>(_db, _stream_manager, _token_metadata_ptr, _abort_source, _tokens, _address, description, reason);
        auto nodes_to_filter = gossiper.get_unreachable_members();
        if (reason == streaming::stream_reason::replace) {
            nodes_to_filter.insert(std::move(replace_address));
        }
        blogger.debug("nodes_to_filter={}", nodes_to_filter);
        streamer->add_source_filter(std::make_unique<range_streamer::failure_detector_source_filter>(nodes_to_filter));
        auto keyspaces = _db.local().get_non_system_keyspaces();
        for (auto& keyspace_name : keyspaces) {
            if (!_db.local().has_keyspace(keyspace_name)) {
                // The keyspace was dropped while we were looping.
                continue;
            }

            auto& ks = _db.local().find_keyspace(keyspace_name);
            auto strategy = ks.get_replication_strategy_ptr();
            // We took a strategy ptr to keep it alive during the `co_await`.
            // The keyspace may be dropped in the meantime.
            dht::token_range_vector ranges = co_await strategy->get_pending_address_ranges(_token_metadata_ptr, _tokens, _address);
            blogger.debug("Will stream keyspace={}, ranges={}", keyspace_name, ranges);
            co_await streamer->add_ranges(keyspace_name, ranges, gossiper, reason == streaming::stream_reason::replace);
        }
        _abort_source.check();
        co_await streamer->stream_async();
    } catch (...) {
        blogger.warn("Error during bootstrap: {}", std::current_exception());
        throw;
    }
}

std::unordered_set<token> boot_strapper::get_bootstrap_tokens(const token_metadata_ptr tmptr, const db::config& cfg, dht::check_token_endpoint check) {
    std::unordered_set<sstring> initial_tokens;
    sstring tokens_string = cfg.initial_token();
    try {
        boost::split(initial_tokens, tokens_string, boost::is_any_of(sstring(", ")));
    } catch (...) {
        throw std::runtime_error(format("Unable to parse initial_token={}", tokens_string));
    }
    initial_tokens.erase("");

    // if user specified tokens, use those
    if (initial_tokens.size() > 0) {
        blogger.debug("tokens manually specified as {}", initial_tokens);
        std::unordered_set<token> tokens;
        for (auto& token_string : initial_tokens) {
            auto token = dht::token::from_sstring(token_string);
            if (check && tmptr->get_endpoint(token)) {
                throw std::runtime_error(format("Bootstrapping to existing token {} is not allowed (decommission/removenode the old node first).", token_string));
            }
            tokens.insert(token);
        }
        blogger.info("Get manually specified bootstrap_tokens={}", tokens);
        return tokens;
    }

    size_t num_tokens = cfg.num_tokens();
    if (num_tokens < 1) {
        throw std::runtime_error("num_tokens must be >= 1");
    }

    if (num_tokens == 1) {
        blogger.warn("Picking random token for a single vnode.  You should probably add more vnodes; failing that, you should probably specify the token manually");
    }

    auto tokens = get_random_tokens(std::move(tmptr), num_tokens);
    blogger.info("Get random bootstrap_tokens={}", tokens);
    return tokens;
}

std::unordered_set<token> boot_strapper::get_random_tokens(const token_metadata_ptr tmptr, size_t num_tokens) {
    std::unordered_set<token> tokens;
    while (tokens.size() < num_tokens) {
        auto token = dht::token::get_random_token();
        auto ep = tmptr->get_endpoint(token);
        if (!ep) {
            tokens.emplace(token);
        }
    }
    return tokens;
}


} // namespace dht
