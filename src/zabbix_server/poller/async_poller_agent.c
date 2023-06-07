/*
** Zabbix
** Copyright (C) 2001-2023 Zabbix SIA
**
** This program is free software; you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation; either version 2 of the License, or
** (at your option) any later version.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software
** Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
**/

#include "checks_agent.h"
#include "async_poller_agent.h"

#include "log.h"
#include "zbxsysinfo.h"
#include "zbx_availability_constants.h"
#include "zbx_item_constants.h"
#include "zbxpreproc.h"

static int	agent_task_process(short event, void *data)
{
	zbx_agent_context	*agent_context = (zbx_agent_context *)data;
	ssize_t			received_len;

	printf("chat_task_process(%x)\n", event);

	if (0 == event)
	{
		/* initialization */
		agent_context->step = ZABBIX_AGENT_STEP_CONNECT_WAIT;
		return ZBX_ASYNC_TASK_WRITE;
	}

	if (0 != (event & EV_TIMEOUT))
	{
		agent_context->ret = TIMEOUT_ERROR;
		SET_MSG_RESULT(&agent_context->result, zbx_dsprintf(NULL, "Get value from agent failed: timed out during %d", agent_context->step));
		return ZBX_ASYNC_TASK_STOP;
	}

	zabbix_log(LOG_LEVEL_DEBUG, "In %s() step:%d event:%x", __func__, agent_context->step, event);

	switch (agent_context->step)
	{
		case ZABBIX_AGENT_STEP_CONNECT_WAIT:
			if (ZBX_TCP_SEC_TLS_CERT == agent_context->host.tls_connect ||
					ZBX_TCP_SEC_TLS_PSK == agent_context->host.tls_connect)
			{
				char	*error = NULL;
				short	event_tls = 0;

				if (SUCCEED != zbx_tls_connect(agent_context->s, agent_context->host.tls_connect,
						agent_context->tls_arg1, agent_context->tls_arg2,
						agent_context->server_name, &event_tls, &error))
				{
					if (POLLOUT & event_tls)
						return ZBX_ASYNC_TASK_READ;
					if (POLLIN & event_tls)
						return ZBX_ASYNC_TASK_WRITE;

					SET_MSG_RESULT(&agent_context->result, zbx_dsprintf(NULL, "Get value from agent"
						" failed: TCP successful, cannot establish TLS to [[%s]:%hu]: %s",
						agent_context->interface.addr, agent_context->interface.port, error));
					agent_context->ret = NETWORK_ERROR;
					zbx_free(error);

					return ZBX_ASYNC_TASK_STOP;
				}
				else
				{
					agent_context->step = ZABBIX_AGENT_STEP_SEND;
					return ZBX_ASYNC_TASK_WRITE;
				}
			}
			ZBX_FALLTHROUGH;
		case ZABBIX_AGENT_STEP_SEND:
			if (0 == (event & EV_WRITE))
			{
				SET_MSG_RESULT(&agent_context->result, zbx_dsprintf(NULL, "Get value from agent failed:"
						" unexpected read event during send"));
				agent_context->ret = FAIL;
				return ZBX_ASYNC_TASK_STOP;
			}

			zabbix_log(LOG_LEVEL_DEBUG, "Sending [%s]", agent_context->key);

			if (SUCCEED != zbx_tcp_send(&agent_context->s, agent_context->key))
			{
				SET_MSG_RESULT(&agent_context->result, zbx_dsprintf(NULL, "Get value from agent failed: %s", zbx_socket_strerror()));
				agent_context->ret = NETWORK_ERROR;
				return ZBX_ASYNC_TASK_STOP;
			}
			else
			{
				agent_context->step = ZABBIX_AGENT_STEP_RECV;
				return ZBX_ASYNC_TASK_READ;
			}
			break;
		case ZABBIX_AGENT_STEP_RECV:
			if (0 == (event & EV_READ))
			{
				SET_MSG_RESULT(&agent_context->result, zbx_dsprintf(NULL, "Get value from agent failed:"
						" unexpected write event during send"));
				zabbix_log(LOG_LEVEL_DEBUG, "unexpected read event when reading result of [%s]", agent_context->key);
				return ZBX_ASYNC_TASK_STOP;
			}

			if (FAIL != (received_len = zbx_tcp_recv_ext(&agent_context->s, 0, 0)))
			{
				zbx_agent_handle_response(&agent_context->s, received_len, &agent_context->ret,
						agent_context->interface.addr, &agent_context->result);
				zabbix_log(LOG_LEVEL_DEBUG, "received");
				agent_context->ret = SUCCEED;
				return ZBX_ASYNC_TASK_STOP;
			}
			else
			{
				SET_MSG_RESULT(&agent_context->result, zbx_dsprintf(NULL, "Get value from agent failed: %s", zbx_socket_strerror()));
				agent_context->ret = NETWORK_ERROR;
			}
			break;
	}

	return ZBX_ASYNC_TASK_STOP;
}

void	zbx_async_check_agent_clean(zbx_agent_context *agent_context)
{
	zbx_free(agent_context->key_orig);
	zbx_free(agent_context->key);
	zbx_free_agent_result(&agent_context->result);
}

int	zbx_async_check_agent(zbx_dc_item_t *item, AGENT_RESULT *result, zbx_poller_config_t *poller_config,
		zbx_async_task_clear_cb_t clear_cb)
{
	zbx_agent_context	*agent_context = zbx_malloc(NULL, sizeof(zbx_agent_context));
	int			ret = NOTSUPPORTED;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s() host:'%s' addr:'%s' key:'%s' conn:'%s'", __func__, item->host.host,
			item->interface.addr, item->key, zbx_tcp_connection_type_name(item->host.tls_connect));

	agent_context->poller_config = poller_config;
	agent_context->itemid = item->itemid;
	agent_context->hostid = item->host.hostid;
	agent_context->value_type = item->value_type;
	agent_context->flags = item->flags;
	agent_context->state = item->state;
	agent_context->host = item->host;
	agent_context->interface = item->interface;
	agent_context->interface.addr = (item->interface.addr == item->interface.dns_orig ?
			agent_context->interface.dns_orig : agent_context->interface.ip_orig);
	agent_context->key = item->key;
	agent_context->key_orig = zbx_strdup(NULL, item->key_orig);
	item->key = NULL;
	zbx_init_agent_result(&agent_context->result);

	switch (agent_context->host.tls_connect)
	{
		case ZBX_TCP_SEC_UNENCRYPTED:
			agent_context->tls_arg1 = NULL;
			agent_context->tls_arg2 = NULL;
			break;
#if defined(HAVE_GNUTLS) || defined(HAVE_OPENSSL)
		case ZBX_TCP_SEC_TLS_CERT:
			agent_context->tls_arg1 = agent_context->host.tls_issuer;
			agent_context->tls_arg2 = agent_context->host.tls_subject;
			break;
		case ZBX_TCP_SEC_TLS_PSK:
			agent_context->tls_arg1 = agent_context->host.tls_psk_identity;
			agent_context->tls_arg2 = agent_context->host.tls_psk;
			break;
#else
		case ZBX_TCP_SEC_TLS_CERT:
		case ZBX_TCP_SEC_TLS_PSK:
			SET_MSG_RESULT(result, zbx_dsprintf(NULL, "A TLS connection is configured to be used with agent"
					" but support for TLS was not compiled into %s.",
					get_program_type_string(program_type)));
			ret = CONFIG_ERROR;
			goto out;
#endif
		default:
			THIS_SHOULD_NEVER_HAPPEN;
			SET_MSG_RESULT(result, zbx_strdup(NULL, "Invalid TLS connection parameters."));
			ret = CONFIG_ERROR;
			goto out;
	}

	if (SUCCEED != zbx_socket_connect(&agent_context->s, SOCK_STREAM, agent_context->poller_config->config_source_ip,
			agent_context->interface.addr, agent_context->interface.port,
			agent_context->poller_config->config_timeout, agent_context->host.tls_connect, agent_context->tls_arg1))
	{
		goto out;
	}

#if defined(HAVE_GNUTLS) || defined(HAVE_OPENSSL)
	if (NULL != agent_context->interface.addr && SUCCEED != zbx_is_ip(agent_context->interface.addr))
		agent_context->server_name = agent_context->interface.addr;
#else
	ZBX_UNUSED(tls_arg1);
	ZBX_UNUSED(tls_arg2);
#endif

	poller_config->processing++;
	zbx_async_poller_add_task(poller_config->base, agent_context->s.socket, agent_context,
			agent_context->poller_config->config_timeout, agent_task_process, clear_cb);

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __func__, zbx_result_string(SUCCEED));

	return SUCCEED;
out:
	zbx_async_check_agent_clean(agent_context);
	zbx_free(agent_context);
	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __func__, zbx_result_string(ret));

	return ret;
}