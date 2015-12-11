/*
 * 2011+ Copyright (c) Evgeniy Polyakov <zbr@ioremap.net>
 * All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/wait.h>

#include <ctype.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#ifdef HAVE_COCAINE_SUPPORT

#include <map>
#include <vector>
#include <sstream>
#include <functional>
#include <msgpack.hpp>

#include <cocaine/context.hpp>
#include <cocaine/logging.hpp>
#include <cocaine/app.hpp>
#include <cocaine/exceptions.hpp>
#include <cocaine/api/event.hpp>
#include <cocaine/api/stream.hpp>
#include <cocaine/api/service.hpp>
#include <cocaine/api/storage.hpp>

#include <elliptics/interface.h>
#include <elliptics/srw.h>
#include <elliptics/utils.hpp>

#include "cocaine-json-trait.hpp"
#include "elliptics.h"

#include <blackhole/logger.hpp>
#include <blackhole/macro.hpp>

#define SRW_LOG(__log__, __level__, __app__, ...) \
	BH_LOG((__log__), (__level__), __VA_ARGS__) \
		("app", (__app__)) \
		("source", "srw")

struct sph_wrapper
{
	struct sph sph;
	std::string event;

	sph_wrapper(struct sph *p_sph)
	{
		char *data = (char *)(p_sph + 1);
		event = std::string(data, p_sph->event_size);
		sph = *p_sph;
	}
};

/**
 * This class handles reply stream from worker.
 * Function write() accepts string from worker and sends it to the client
 * started EXEC transaction if DNET_SPH_FLAGS_SRC_BLOCK was set.
 * It allows to write interactive worker application in a straightforward way:
 *  - read SPH + data from request stream
 *  - do some useful job
 *  - send reply via response stream
 * Chunked replies are allowed by protocol.
 */
class dnet_upstream_t: public cocaine::api::stream_t
{
	public:
		dnet_upstream_t(struct dnet_node *node, struct dnet_net_state *state, struct dnet_cmd *cmd,
				const sph_wrapper &sph, std::function<void()> deleter):
		m_completed(false),
		m_node(node),
		m_state(dnet_state_get(state)),
		m_cmd(*cmd),
		m_sph(sph),
		m_deleter(deleter),
		m_error(0) {
		}

		~dnet_upstream_t() {
			reply(true, NULL, 0);

			dnet_state_put(m_state);
		}

		/*
		 * This function is called when worker sends chunk to reply stream.
		 * We need to decode reply, check if it is a string (since we don't want to serialize
		 * objects here), prepend with SPH header for the client and send elliptics reply.
		 */
		virtual void write(const char *chunk, size_t size) {
			int err = 0;
			/* Chunk should be decoded */
			msgpack::object obj;
			size_t offset = 0;

			msgpack::unpack_return rv = msgpack::unpack(chunk, size, &offset, &m_zone, &obj);
			if (rv != msgpack::UNPACK_SUCCESS && rv != msgpack::UNPACK_EXTRA_BYTES) {
				err = EINVAL;
				SRW_LOG(*m_node->log, DNET_LOG_ERROR, "app/" + m_sph.event, "unable to unpack response");
			} else if (obj.type != msgpack::type::RAW) {
				err = EINVAL;
				SRW_LOG(*m_node->log, DNET_LOG_ERROR, "app/" + m_sph.event, "insorrect response type, should be string, got: %d", obj.type);
			}

			if (err) {
				reply(true, NULL, 0);
				return;
			}

			/* Make SPH reply for client */
			size_t data_size = sizeof(sph) +  m_sph.event.size() + obj.via.raw.size;
			ioremap::elliptics::data_buffer data(data_size);

			data.write(&m_sph.sph, sizeof(sph));
			data.write(m_sph.event.data(), m_sph.event.size());
			data.write(obj.via.raw.ptr, obj.via.raw.size);

			/* Fix SPH sizes */
			ioremap::elliptics::data_pointer data_ptr(std::move(data));

			sph * sph_p = (sph*)(data_ptr.data());
			sph_p->event_size = m_sph.event.size();
			sph_p->data_size = obj.via.raw.size;

			reply(false, data_ptr.data<const char>(), data_ptr.size());
		}

		virtual void close(void) {
			SRW_LOG(*m_node->log, DNET_LOG_NOTICE, "app/" + m_sph.event, "%s", "job completed");
			reply(true, NULL, 0);
			m_deleter();
		}

		virtual void error(int code, const std::string &message) {
			m_error = -code;
			SRW_LOG(*m_node->log, DNET_LOG_ERROR, "app/" + m_sph.event, "%s: %d", message, code);
		}

		void reply(bool completed, const char *reply, size_t size) {
			std::unique_lock<std::mutex> guard(m_lock);
			if (m_completed)
				return;

			m_completed = completed;

			if ((m_sph.sph.flags & DNET_SPH_FLAGS_SRC_BLOCK) || (reply && size)) {
				if (reply && size) {
					if (completed)
						m_cmd.flags &= ~DNET_FLAGS_NEED_ACK;
					dnet_send_reply(m_state, &m_cmd, (void *)reply, size, !completed);
				} else if (completed) {
					m_cmd.flags |= DNET_FLAGS_NEED_ACK;
					dnet_send_ack(m_state, &m_cmd, m_error, 0);
				}
			}
		}

	private:
		bool m_completed;
		struct dnet_node *m_node;
		std::mutex m_lock;
		struct dnet_net_state *m_state;
		struct dnet_cmd m_cmd;
		sph_wrapper m_sph;
		std::function<void()> m_deleter;
		int m_error;
		msgpack::zone m_zone;
};

typedef std::shared_ptr<dnet_upstream_t> dnet_shared_upstream_t;
typedef std::map<int, dnet_shared_upstream_t> jobs_map_t;

struct srw_counters {
	long			blocked;
	long			nonblocked;
	long			reply;

	srw_counters():
	blocked(0),
	nonblocked(0),
	reply(0)
	{
	}
};

typedef std::map<std::string, srw_counters> cmap_t;

class dnet_app_t : public cocaine::app_t {
	public:
		dnet_app_t(cocaine::context_t& context, const std::string& name, const std::string& profile) :
		cocaine::app_t(context, name, profile),
		m_pool_size(-1),
		m_id("default"),
		m_started(false) {
			atomic_set(&m_sph_index, 1);
		}

		~dnet_app_t() {
			stop();
		}

		void start() {
			if (!m_started) {
				cocaine::app_t::start();
				m_started = true;
			}
		}

		void stop() {
			if (m_started) {
				m_started = false;
				cocaine::app_t::stop();
			}
		}

		Json::Value counters(void) {
			Json::Value info(Json::objectValue);

			for (auto it = m_counters.begin(); it != m_counters.end(); ++it) {
				Json::Value obj(Json::objectValue);

				obj["blocked"] = static_cast<Json::Value::Int64>(it->second.blocked);
				obj["nonblocked"] = static_cast<Json::Value::Int64>(it->second.nonblocked);
				obj["reply"] = static_cast<Json::Value::Int64>(it->second.reply);

				info[it->first] = obj;
			}

			return info;
		}

		void update(const std::string &event, struct sph *sph) {
			std::unique_lock<std::mutex> guard(m_lock);

			if (sph->flags & (DNET_SPH_FLAGS_REPLY | DNET_SPH_FLAGS_FINISH)) {
				m_counters[event].reply += 1;
			} else if (sph->flags & DNET_SPH_FLAGS_SRC_BLOCK) {
				m_counters[event].blocked += 1;
			} else {
				m_counters[event].nonblocked += 1;
			}
		}

		void set_pool_size(int pool_size) {
			m_pool_size = pool_size;
		}

		void set_task_id(const std::string &id) {
			m_id = id;
		}

		const std::string &get_task_id(void) const {
			return m_id;
		}

		int get_index(int sph_index) {
			if (m_pool_size == -1)
				return -1;

			if (sph_index == -1)
				return atomic_inc(&m_sph_index) % m_pool_size;

			return sph_index % m_pool_size;
		}

	private:
		std::mutex	m_lock;
		cmap_t		m_counters;
		int		m_pool_size;
		atomic_t	m_sph_index;
		std::string	m_id;
		bool		m_started;
};

typedef std::map<std::string, std::shared_ptr<dnet_app_t> > eng_map_t;

namespace {

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

}

class dnet_sink_t: public cocaine::logging::logger_concept_t {
	public:
		dnet_sink_t(struct dnet_node *n) : m_node(n) {
		}

		virtual cocaine::logging::priorities verbosity() const {
			return convert_verbosity(m_node->log->log().verbosity());
		}

		virtual void emit(cocaine::logging::priorities prio, const std::string &app, const std::string& message) {
			dnet_log_level level = convert_verbosity(prio);
			SRW_LOG(*m_node->log, level, app, "%s", message.c_str());
		}

	private:
		struct dnet_node *m_node;
};

class srw {
	public:
		srw(struct dnet_node *n, const std::string &config) :
		m_node(n),
		m_ctx(config, blackhole::aux::util::make_unique<dnet_sink_t>(m_node))
		{
			atomic_set(&m_src_key, 1);

		}

		~srw() {
		}

		int process(struct dnet_net_state *st, struct dnet_cmd *cmd, struct sph *sph) {
			int err = 0;
			char *data = (char *)(sph + 1);
			std::string event = dnet_get_event(sph, data);

			char id_str[DNET_DUMP_NUM * 2 + 1];
			char sph_str[DNET_DUMP_NUM * 2 + 1];

			dnet_dump_id_len_raw(cmd->id.id, DNET_DUMP_NUM, id_str);
			dnet_dump_id_len_raw(sph->src.id, DNET_DUMP_NUM, sph_str);

			id_str[2 * DNET_DUMP_NUM] = '\0';
			sph_str[2 * DNET_DUMP_NUM] = '\0';

			/*!
			 * Elliptics' event always looks like "application@method", where application
			 * is a name of \a application which \a method we should call.
			 *
			 * Afterwards we are able to find this application in our local dictionary
			 * and call it's method with data extracted from \a sph structure.
			 */
			char *ptr = strchr((char *)event.c_str(), '@');
			if (!ptr) {
				dnet_log(m_node, DNET_LOG_ERROR, "%s: sph: %s: %s: invalid event name: "
						"must be application@event or application@start-task",
						id_str, sph_str, event.c_str());
				return -EINVAL;
			}

			std::string app(event.c_str(), ptr - event.c_str());
			std::string ev(ptr+1);

			if ((ev == "start-task") || (ev == "start-multiple-task")) {
				std::unique_lock<std::mutex> guard(m_lock);
				eng_map_t::iterator it = m_map.find(app);
				if (it == m_map.end()) {
					std::shared_ptr<dnet_app_t> eng(new dnet_app_t(m_ctx, app, app));
					eng->start();

					if (ev == "start-multiple-task") {
						auto storage = cocaine::api::storage(m_ctx, "core");
						Json::Value profile = storage->get<Json::Value>("profiles", app);

						int idle = profile["idle-timeout"].asInt();
						int pool_limit = profile["pool-limit"].asInt();
						const int idle_min = 60 * 60 * 24 * 30;

						dnet_log(m_node, DNET_LOG_INFO, "%s: sph: %s: %s: multiple start: "
								"idle: %d/%d, workers: %d",
								id_str, sph_str, event.c_str(), idle, idle_min, pool_limit);

						if (idle && idle < idle_min) {
							dnet_log(m_node, DNET_LOG_ERROR, "%s: sph: %s: %s: multiple start: "
								"idle must be big enough, we check it to be larger than 30 days (%d seconds), "
								"current profile value is %d",
								id_str, sph_str, event.c_str(), idle_min, idle);
							return -EINVAL;
						}

						eng->set_pool_size(pool_limit);

						if (sph->data_size) {
							std::string task_id(data + sph->event_size, sph->data_size);

							eng->set_task_id(task_id);
						}
					}

					m_map.insert(std::make_pair(app, eng));
					dnet_log(m_node, DNET_LOG_INFO, "%s: sph: %s: %s: started", id_str, sph_str, event.c_str());
				} else {
					dnet_log(m_node, DNET_LOG_INFO, "%s: sph: %s: %s: was already started",
							id_str, sph_str, event.c_str());
				}

			} else if (ev == "stop-task") {
				std::unique_lock<std::mutex> guard(m_lock);
				eng_map_t::iterator it = m_map.find(app);
				/* destructor stops engine */
				if (it != m_map.end())
					m_map.erase(it);
				guard.unlock();

				dnet_log(m_node, DNET_LOG_INFO, "%s: sph: %s: %s: stopped", id_str, sph_str, event.c_str());

			} else if (ev == "info") {
				std::unique_lock<std::mutex> guard(m_lock);
				eng_map_t::iterator it = m_map.find(app);
				if (it == m_map.end()) {
					dnet_log(m_node, DNET_LOG_ERROR, "%s: sph: %s: %s: no task", id_str, sph_str, event.c_str());
					return -ENOENT;
				}

				Json::Value info = it->second->info();
				info["counters"] = it->second->counters();

				guard.unlock();

				std::string s = Json::StyledWriter().write(info);

				struct sph *reply;
				std::string tmp;

				tmp.resize(sizeof(struct sph));
				reply = (struct sph *)tmp.data();

				reply->event_size = event.size();
				reply->data_size = s.size();
				memcpy(&reply->addr, &st->n->addrs[0], sizeof(struct dnet_addr));

				tmp += event;
				tmp += s;

				err = dnet_send_reply(st, cmd, (void *)tmp.data(), tmp.size(), 0);
				dnet_log(m_node, DNET_LOG_INFO, "%s: sph: %s: %s: info request complete", id_str, sph_str, event.c_str());

			} else if (sph->flags & (DNET_SPH_FLAGS_REPLY | DNET_SPH_FLAGS_FINISH)) {
				bool final = !!(sph->flags & DNET_SPH_FLAGS_FINISH);

				std::unique_lock<std::mutex> guard(m_lock);

				jobs_map_t::iterator it = m_jobs.find(sph->src_key);
				if (it == m_jobs.end()) {
					dnet_log(m_node, DNET_LOG_ERROR, "%s: sph: %s: %s: no job: %d to complete",
						id_str, sph_str, event.c_str(), sph->src_key);
					return -ENOENT;
				}

				dnet_shared_upstream_t upstream = it->second;
				if (final)
					m_jobs.erase(it);

				eng_map_t::iterator appit = m_map.find(app);
				if (appit != m_map.end())
					appit->second->update(event, sph);

				guard.unlock();

				upstream->reply(final, (char *)sph, sizeof(struct sph) + sph->event_size + sph->data_size);

				memcpy(&sph->addr, &st->n->addrs[0], sizeof(struct dnet_addr));
				dnet_log(m_node, DNET_LOG_INFO, "%s: sph: %s: %s: completed: job: %d, total-size: %zd, finish: %d",
						id_str, sph_str, event.c_str(), sph->src_key, total_size(sph), final);

			} else {
				/*
				 * src_key can be used as index within named workers,
				 * but src_key is also an index in jobs map, save it here
				 * and use to find worker name later
				 */
				int src_key = sph->src_key;

				if (sph->flags & DNET_SPH_FLAGS_SRC_BLOCK) {
					sph->src_key = atomic_inc(&m_src_key);
					memcpy(sph->src.id, cmd->id.id, sizeof(sph->src.id));
				}

				cocaine::api::event_t cevent(ev);

				std::unique_lock<std::mutex> guard(m_lock);
				eng_map_t::iterator it = m_map.find(app);
				if (it == m_map.end()) {
					dnet_log(m_node, DNET_LOG_ERROR, "%s: sph: %s: %s: no task", id_str, sph_str, event.c_str());
					return -ENOENT;
				}

				it->second->update(event, sph);

				dnet_shared_upstream_t upstream(std::make_shared<dnet_upstream_t>(m_node, st, cmd, sph,
												std::bind(&srw::complete_job, this, id_str, sph_wrapper(sph))));

				if (sph->flags & DNET_SPH_FLAGS_SRC_BLOCK) {
					m_jobs.insert(std::make_pair((int)sph->src_key, upstream));
				}

				std::shared_ptr<dnet_app_t> eng = it->second;
				guard.unlock();

				int index = eng->get_index(src_key);
				std::shared_ptr<cocaine::api::stream_t> stream;

				try {
					if (index == -1) {
						stream = eng->enqueue(cevent, upstream);
					} else {
						app = eng->get_task_id() + "-" + app + "-" + ioremap::elliptics::lexical_cast(index);
						stream = eng->enqueue(cevent, upstream, app);
					}

					stream->write((const char *)sph, total_size(sph) + sizeof(struct sph));
					/*
					 * Request stream should be closed after all data was sent to prevent resource leackage.
					 */
					stream->close();

				} catch (const std::exception &e) {
					dnet_log(m_node, DNET_LOG_ERROR, "%s: sph: %s: %s: enqueue/write-exception: queue: %s, src-key-orig: %d, "
							"job: %d, total-size: %zd, block: %d: %s",
							id_str, sph_str, event.c_str(),
							app.c_str(),
							src_key, sph->src_key, total_size(sph),
							!!(sph->flags & DNET_SPH_FLAGS_SRC_BLOCK),
							e.what());
					return -EXFULL;
				}

				dnet_log(m_node, DNET_LOG_INFO, "%s: sph: %s: %s: started: queue: %s, src-key-orig: %d, "
						"job: %d, total-size: %zd, block: %d",
						id_str, sph_str, event.c_str(),
						app.c_str(),
						src_key, sph->src_key, total_size(sph),
						!!(sph->flags & DNET_SPH_FLAGS_SRC_BLOCK));

				if (sph->flags & DNET_SPH_FLAGS_SRC_BLOCK) {
					cmd->flags &= ~DNET_FLAGS_NEED_ACK;
				}
			}

			return err;
		}

		void complete_job(std::string id, sph_wrapper sph)
		{
			std::unique_lock<std::mutex> guard(m_lock);

			jobs_map_t::iterator it = m_jobs.find(sph.sph.src_key);
			if (it == m_jobs.end()) {
				dnet_log(m_node, DNET_LOG_ERROR, "%s: sph: %s: %s: no job: %d to complete",
					id.c_str(), dnet_dump_id_str(sph.sph.src.id), sph.event.c_str(), sph.sph.src_key);
				return;
			}

			m_jobs.erase(it);
		}

	private:
		struct dnet_node		*m_node;
		cocaine::context_t		m_ctx;
		std::mutex			m_lock;
		eng_map_t			m_map;
		jobs_map_t			m_jobs;
		atomic_t			m_src_key;

		std::string dnet_get_event(const struct sph *sph, const char *data) {
			return std::string(data, sph->event_size);
		}

		size_t total_size(const struct sph *sph) {
			return sph->event_size + sph->data_size;
		}
};

int dnet_srw_init(struct dnet_node *n, struct dnet_config *cfg)
{
	int err = 0;

	try {
		n->srw = (void *)new srw(n, cfg->srw.config);
		dnet_log(n, DNET_LOG_INFO, "srw: initialized: config: %s", cfg->srw.config);
		return 0;
	} catch (const std::exception &e) {
		dnet_log(n, DNET_LOG_ERROR, "srw: init failed: config: %s, exception: %s", cfg->srw.config, e.what());
		err = -ENOMEM;
	}

	return err;
}

void dnet_srw_cleanup(struct dnet_node *n)
{
	if (n->srw) {
		try {
			srw *sr = (srw *)n->srw;
			delete sr;
		} catch (...) {
		}

		n->srw = NULL;
	}
}

int dnet_cmd_exec_raw(struct dnet_net_state *st, struct dnet_cmd *cmd, struct sph *header, const void *data)
{
	struct dnet_node *n = st->n;
	srw *s = (srw *)n->srw;

	if (!s)
		return -ENOTSUP;

	try {
		return s->process(st, cmd, header);
	} catch (const std::exception &e) {
		dnet_log(n, DNET_LOG_ERROR, "%s: srw-processing: event: %.*s, data-size: %lld, exception: %s",
				dnet_dump_id(&cmd->id), header->event_size, (const char *)data,
				(unsigned long long)header->data_size,
				e.what());
	}

	return -EINVAL;
}

int dnet_srw_update(struct dnet_node *, int )
{
	return 0;
}
#else
#include <errno.h>

#include "elliptics.h"

int dnet_srw_init(struct dnet_node *, struct dnet_config *)
{
	return -ENOTSUP;
}

void dnet_srw_cleanup(struct dnet_node *)
{
}

int dnet_cmd_exec_raw(struct dnet_net_state *, struct dnet_cmd *, struct sph *, const void *)
{
	return -ENOTSUP;
}

int dnet_srw_update(struct dnet_node *, int)
{
	return 0;
}
#endif
