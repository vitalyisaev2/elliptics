/*
 * Copyright 2013+ Ruslan Nigmatullin <euroelessar@yandex.ru>
 * Copyright 2011-2012 Andrey Sibiryov <me@kobology.ru>
 *
 * This file is part of Elliptics.
 *
 * Elliptics is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Elliptics is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with Elliptics.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "storage.hpp"

#include <cocaine/context.hpp>
#include <cocaine/logging.hpp>

using namespace cocaine;
using namespace cocaine::logging;
using namespace cocaine::storage;
namespace ell = ioremap::elliptics;

static cocaine::logging::priorities convert_verbosity(ioremap::elliptics::log_level level)
{
	switch (level) {
		case DNET_LOG_DEBUG:
			return cocaine::logging::debug;
		case DNET_LOG_NOTICE:
		case DNET_LOG_INFO:
			return cocaine::logging::info;
		case DNET_LOG_WARNING:
			return cocaine::logging::warning;
		case DNET_LOG_ERROR:
			return cocaine::logging::error;
		default:
			return cocaine::logging::ignore;
	};
}

static dnet_log_level convert_verbosity(cocaine::logging::priorities prio) {
	switch (prio) {
		case cocaine::logging::debug:
			return DNET_LOG_DEBUG;
		case cocaine::logging::info:
			return DNET_LOG_INFO;
		case cocaine::logging::warning:
			return DNET_LOG_WARNING;
		case cocaine::logging::error:
		default:
			return DNET_LOG_ERROR;
	}
}

log_adapter_impl_t::log_adapter_impl_t(const std::shared_ptr<logging::log_t> &log): m_log(log), m_formatter("%(message)s %(...L)s")
{
}

void log_adapter_impl_t::handle(const blackhole::record_t &record)
{
	dnet_log_level level = record.extract<dnet_log_level>(blackhole::keyword::severity<dnet_log_level>().name());
	auto cocaine_level = convert_verbosity(level);
	COCAINE_LOG(m_log, cocaine_level, "elliptics: %s", m_formatter.format(record));
}

log_adapter_t::log_adapter_t(const std::shared_ptr<logging::log_t> &log)
  : ioremap::elliptics::logger_base(DNET_LOG_DEBUG)
{
	add_frontend(blackhole::aux::util::make_unique<log_adapter_impl_t>(log));
	set_filter(convert_verbosity(log->verbosity()));
}

namespace {

dnet_config parse_json_config(const Json::Value& args) {
	dnet_config cfg;

	std::memset(&cfg, 0, sizeof(cfg));

	cfg.wait_timeout   = args.get("wait-timeout", 5).asInt();
	cfg.check_timeout  = args.get("check-timeout", 20).asInt();
	cfg.io_thread_num  = args.get("io-thread-num", 0).asUInt();
	cfg.net_thread_num = args.get("net-thread-num", 0).asUInt();
	cfg.flags          = args.get("flags", 0).asInt();

	return cfg;
}

}

elliptics_storage_t::elliptics_storage_t(context_t &context, const std::string &name, const Json::Value &args) :
	category_type(context, name, args),
	m_context(context),
	m_log(new log_t(context, name)),
	m_log_adapter(m_log),
	m_config(parse_json_config(args)),
	m_node(ioremap::elliptics::logger(m_log_adapter, blackhole::log::attributes_t()), m_config),
	m_session(m_node)
{
	Json::Value nodes(args["nodes"]);

	if (nodes.empty() || !nodes.isArray()) {
		throw storage_error_t("no nodes has been specified");
	}

	std::vector<ioremap::elliptics::address> remotes;
	for (auto it = nodes.begin(); it != nodes.end(); ++it) {
		try {
			remotes.emplace_back((*it).asString());
		} catch (ioremap::elliptics::error &exc) {
			throw storage_error_t("failed to parse remote: %s", exc.what());
		}
	}

	try {
		m_node.add_remote(remotes);
	} catch (ioremap::elliptics::error &exc) {
		throw storage_error_t("failed to add remotes: %s", exc.what());
	}

	{
		auto scn = args.get("success-copies-num", "any");

		if (scn == "any") {
			m_success_copies_num = ioremap::elliptics::checkers::at_least_one;
		} else if (scn == "quorum") {
			m_success_copies_num = ioremap::elliptics::checkers::quorum;
		} else if (scn == "all") {
			m_success_copies_num = ioremap::elliptics::checkers::all;
		} else {
			throw storage_error_t("unknown success-copies-num type");
		}
	}

	{
		auto timeouts = args["timeouts"];

		if (!timeouts.empty()) {
			if (!timeouts.isObject()) {
				throw storage_error_t("invalid format of timeouts");
			}

			m_timeouts.read = timeouts.get("read", 5).asInt();
			m_timeouts.write = timeouts.get("write", 5).asInt();
			m_timeouts.remove = timeouts.get("remove", 5).asInt();
			m_timeouts.find = timeouts.get("find", 5).asInt();
		}
	}

	Json::Value groups(args["groups"]);

	if (groups.empty() || !groups.isArray()) {
		throw storage_error_t("no groups has been specified");
	}

	std::transform(groups.begin(), groups.end(), std::back_inserter(m_groups), std::mem_fn(&Json::Value::asInt));

	m_session.set_groups(m_groups);
	m_session.set_exceptions_policy(ell::session::no_exceptions);
}

std::string elliptics_storage_t::read(const std::string &collection, const std::string &key)
{
	auto result = async_read(collection, key);
	result.wait();

	if (result.error()) {
		throw storage_error_t(result.error().message());
	}

	return result.get_one().file().to_string();
}

void elliptics_storage_t::write(const std::string &collection,
	const std::string &key,
	const std::string &blob,
	const std::vector<std::string> &tags)
{
	auto result = async_write(collection, key, blob, tags);
	result.wait();

	COCAINE_LOG_DEBUG(
		m_log,
		"write finished: %s",
		result.error().message()
	);

	if (result.error()) {
		throw storage_error_t(result.error().message());
	}
}

std::vector<std::string> elliptics_storage_t::find(const std::string &collection, const std::vector<std::string> &tags)
{
	auto result = async_find(collection, tags);
	result.wait();

	if (result.error()) {
		throw storage_error_t(result.error().message());
	}

	return convert_list_result(result.get());
}

void elliptics_storage_t::remove(const std::string &collection, const std::string &key)
{
	auto result = async_remove(collection, key);
	result.wait();

	if (result.error()) {
		throw storage_error_t(result.error().message());
	}
}

ell::async_read_result elliptics_storage_t::async_read(const std::string &collection, const std::string &key)
{
	using namespace std::placeholders;

	COCAINE_LOG_DEBUG(
		m_log,
		"reading the '%s' object, collection: '%s'",
		key,
		collection
	);

	ell::session session = m_session.clone();
	session.set_namespace(collection.data(), collection.size());
	session.set_timeout(m_timeouts.read);

	return session.read_data(key, 0, 0);
}

ell::async_read_result elliptics_storage_t::async_read_latest(const std::string &collection, const std::string &key)
{
	using namespace std::placeholders;

	COCAINE_LOG_DEBUG(
		m_log,
		"reading the '%s' object, collection: '%s'",
		key,
		collection
	);

	ell::session session = m_session.clone();
	session.set_namespace(collection.data(), collection.size());
	session.set_timeout(m_timeouts.read);

	return session.read_latest(key, 0, 0);
}

static void on_adding_index_finished(const elliptics_storage_t::log_ptr &log,
	ell::async_result_handler<ell::write_result_entry> handler,
	const ell::error_info &err)
{
	if (err) {
		COCAINE_LOG_DEBUG(
			log,
			"index adding failed: %s",
			err.message()
		);
	} else {
		COCAINE_LOG_DEBUG(
			log,
			"index adding completed"
		);
	}
	handler.complete(err);
}

static void on_write_finished(const elliptics_storage_t::log_ptr &log,
	ell::async_result_handler<ell::write_result_entry> handler,
	ell::session session,
	const std::string &key,
	const std::vector<std::string> &index_names,
	const ell::sync_write_result &result,
	const ell::error_info &err)
{
	using namespace std::placeholders;

	if (err) {
		COCAINE_LOG_DEBUG(
			log,
			"write failed: %s",
			err.message()
		);
		handler.complete(err);
		return;
	}
	COCAINE_LOG_DEBUG(
		log,
		"write partially completed"
	);

	for (auto it = result.begin(); it != result.end(); ++it) {
		handler.process(*it);
	}

	std::vector<ell::data_pointer> index_data(index_names.size(),
		ell::data_pointer::copy(key.c_str(), key.size()));

	session.set_indexes(key, index_names, index_data)
		.connect(std::bind(on_adding_index_finished, log, handler, _2));
}

ell::async_write_result elliptics_storage_t::async_write(const std::string &collection, const std::string &key, const std::string &blob, const std::vector<std::string> &tags)
{
	using namespace std::placeholders;

	COCAINE_LOG_DEBUG(
		m_log,
		"writing the '%s' object, collection: '%s'",
		key,
		collection
	);

	ell::session session = m_session.clone();
	session.set_namespace(collection.data(), collection.size());
	session.set_filter(ioremap::elliptics::filters::all_with_ack);
	session.set_timeout(m_timeouts.write);
	session.set_checker(m_success_copies_num);

	auto write_result = session.write_data(key, blob, 0);

	if (tags.empty()) {
		return write_result;
	}

	ell::async_write_result result(session);
	ell::async_result_handler<ell::write_result_entry> handler(result);

	write_result.connect(std::bind(on_write_finished, m_log, handler, session, key, tags, _1, _2));

	return result;
}

ell::async_write_result elliptics_storage_t::async_write_with_ttl(const std::string &collection, const std::string &key, const std::string &blob, const std::vector<std::string> &tags, long timeout)
{
	using namespace std::placeholders;

	COCAINE_LOG_DEBUG(
		m_log,
		"writing the '%s' object, collection: '%s', ttl: '%d'",
		key,
		collection,
    timeout
	);

	ell::session session = m_session.clone();
  session.set_ioflags(DNET_IO_FLAGS_CACHE | DNET_IO_FLAGS_CACHE_REMOVE_FROM_DISK);
	session.set_namespace(collection.data(), collection.size());
	session.set_filter(ioremap::elliptics::filters::all_with_ack);
	session.set_timeout(m_timeouts.write);
	session.set_checker(m_success_copies_num);

	auto write_result = session.write_data(key, blob, 0);
  auto write_result2 = session.write_cache(key, blob, timeout);

	if (tags.empty()) {
		return write_result;
	}

	ell::async_write_result result(session);
	ell::async_result_handler<ell::write_result_entry> handler(result);

	write_result.connect(std::bind(on_write_finished, m_log, handler, session, key, tags, _1, _2));

	return result;
}

ell::async_find_indexes_result elliptics_storage_t::async_find(const std::string &collection, const std::vector<std::string> &tags)
{
	COCAINE_LOG_DEBUG(
		m_log,
		"listing collection: '%s'",
		collection
	);
	using namespace std::placeholders;

	ell::session session = m_session.clone();
	session.set_namespace(collection.data(), collection.size());
	session.set_timeout(m_timeouts.find);

	return session.find_all_indexes(tags);
}

static void on_removing_index_finished(ell::async_result_handler<ell::callback_result_entry> handler,
	ell::session session,
	const std::string &key,
	const ell::sync_update_indexes_result &,
	const ell::error_info &err)
{
	using namespace std::placeholders;

	if (err) {
		handler.complete(err);
		return;
	}

	session.remove(key).connect(handler);
}

ell::async_remove_result elliptics_storage_t::async_remove(const std::string &collection, const std::string &key)
{
	using namespace std::placeholders;

	COCAINE_LOG_DEBUG(
		m_log,
		"removing the '%s' object, collection: '%s'",
		key,
		collection
	);

	ell::session session = m_session.clone();
	session.set_namespace(collection.data(), collection.size());
	session.set_timeout(m_timeouts.remove);
	session.set_checker(m_success_copies_num);

	ell::async_remove_result result(session);
	ell::async_result_handler<ell::callback_result_entry> handler(result);

	session.set_checker(ell::checkers::no_check);
	session.set_filter(ell::filters::all_with_ack);

	session.set_indexes(key, std::vector<std::string>(), std::vector<ell::data_pointer>())
		.connect(std::bind(on_removing_index_finished, handler, session, key, _1, _2));

	return result;
}

ioremap::elliptics::async_read_result elliptics_storage_t::async_cache_read(const std::string &collection, const std::string &key)
{
	COCAINE_LOG_DEBUG(
		m_log,
		"cache reading the '%s' object, collection: '%s'",
		key,
		collection
	);

	ell::session session = m_session.clone();
	session.set_namespace(collection.data(), collection.size());
	session.set_ioflags(DNET_IO_FLAGS_CACHE | DNET_IO_FLAGS_CACHE_ONLY);
	session.set_timeout(m_timeouts.read);

	return session.read_data(key, 0, 0);
}

ioremap::elliptics::async_write_result elliptics_storage_t::async_cache_write(const std::string &collection, const std::string &key,
	const std::string &blob, int timeout)
{
	COCAINE_LOG_DEBUG(
		m_log,
		"cache writing the '%s' object, collection: '%s'",
		key,
		collection
	);

	ell::session session = m_session.clone();
	session.set_namespace(collection.data(), collection.size());
	session.set_ioflags(DNET_IO_FLAGS_CACHE | DNET_IO_FLAGS_CACHE_ONLY);
	session.set_timeout(m_timeouts.write);
	session.set_checker(m_success_copies_num);

	return session.write_cache(key, blob, timeout);
}

std::pair<ioremap::elliptics::async_read_result, elliptics_storage_t::key_name_map> elliptics_storage_t::async_bulk_read(
	const std::string &collection, const std::vector<std::string> &keys)
{
	COCAINE_LOG_DEBUG(
		m_log,
		"bulk reading, collection: '%s'",
		collection
	);

	ell::session session = m_session.clone();
	session.set_namespace(collection.data(), collection.size());
	session.set_timeout(m_timeouts.read);

	key_name_map keys_map;
	dnet_raw_id id;

	for (size_t i = 0; i < keys.size(); ++i) {
		session.transform(keys[i], id);
		keys_map[id] = keys[i];
	}

	return std::make_pair(session.bulk_read(keys), std::move(keys_map));
}

ioremap::elliptics::async_write_result elliptics_storage_t::async_bulk_write(const std::string &collection, const std::vector<std::string> &keys,
	const std::vector<std::string> &blobs)
{
	COCAINE_LOG_DEBUG(
		m_log,
		"bulk writing, collection: '%s'",
		collection
	);

	ell::session session = m_session.clone();
	session.set_namespace(collection.data(), collection.size());
	session.set_filter(ell::filters::all);
	session.set_timeout(m_timeouts.write);
	session.set_checker(m_success_copies_num);

	std::vector<dnet_io_attr> ios;
	ios.reserve(blobs.size());

	dnet_io_attr io;
	dnet_id id;
	memset(&io, 0, sizeof(io));
	dnet_empty_time(&io.timestamp);
	memset(&id, 0, sizeof(id));

	for (size_t i = 0; i < blobs.size(); ++i) {
		session.transform(keys[i], id);
		memcpy(io.id, id.id, sizeof(io.id));

		io.size = blobs[i].size();

		ios.push_back(io);
	}

	return session.bulk_write(ios, blobs);
}

std::vector<std::string> elliptics_storage_t::convert_list_result(const ioremap::elliptics::sync_find_indexes_result &result)
{
	std::vector<std::string> promise_result;

	for (auto it = result.begin(); it != result.end(); ++it) {
		if (!it->indexes.empty()) {
			promise_result.push_back(it->indexes.front().data.to_string());
		}
	}

	return promise_result;
}
