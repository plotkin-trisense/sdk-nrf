/*
 * Copyright (c) 2020 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <logging/log.h>
#include <zephyr.h>
#include <stdio.h>
#include <net/mqtt.h>
#include <net/socket.h>
#include <random/rand32.h>
#include "slm_util.h"
#include "slm_at_host.h"
#include "slm_native_tls.h"
#include "slm_at_mqtt.h"

LOG_MODULE_REGISTER(slm_mqtt, CONFIG_SLM_LOG_LEVEL);

#define MQTT_MAX_TOPIC_LEN	128
#define MQTT_MAX_CID_LEN	64
#define MQTT_MESSAGE_BUFFER_LEN	NET_IPV4_MTU

#define INVALID_FDS -1

/**@brief MQTT client operations. */
enum slm_mqttcon_operation {
	MQTTC_DISCONNECT,
	MQTTC_CONNECT,
	MQTTC_CONNECT6,
};

/**@brief MQTT subscribe operations. */
enum slm_mqttsub_operation {
	AT_MQTTSUB_UNSUB,
	AT_MQTTSUB_SUB
};

static struct slm_mqtt_ctx {
	int family; /* Socket address family */
	bool connected;
	struct mqtt_utf8 client_id;
	struct mqtt_utf8 username;
	struct mqtt_utf8 password;
	sec_tag_t sec_tag;
	union {
		struct sockaddr_in  broker;
		struct sockaddr_in6 broker6;
	};
} ctx;

static char mqtt_broker_url[SLM_MAX_URL + 1];
static uint16_t mqtt_broker_port;
static char mqtt_clientid[MQTT_MAX_CID_LEN + 1];
static char mqtt_username[SLM_MAX_USERNAME + 1];
static char mqtt_password[SLM_MAX_PASSWORD + 1];

static struct mqtt_publish_param pub_param;
static uint8_t pub_topic[MQTT_MAX_TOPIC_LEN];
static uint8_t pub_msg[MQTT_MESSAGE_BUFFER_LEN];

/* global variable defined in different files */
extern struct at_param_list at_param_list;
extern char rsp_buf[SLM_AT_CMD_RESPONSE_MAX_LEN];

#define THREAD_STACK_SIZE	KB(2)
#define THREAD_PRIORITY		K_LOWEST_APPLICATION_THREAD_PRIO

static struct k_thread mqtt_thread;
static K_THREAD_STACK_DEFINE(mqtt_thread_stack, THREAD_STACK_SIZE);

/* Buffers for MQTT client. */
static uint8_t rx_buffer[MQTT_MESSAGE_BUFFER_LEN];
static uint8_t tx_buffer[MQTT_MESSAGE_BUFFER_LEN];
static uint8_t payload_buf[MQTT_MESSAGE_BUFFER_LEN];

/* The mqtt client struct */
static struct mqtt_client client;

/**@brief Function to read the published payload.
 */
static int publish_get_payload(struct mqtt_client *c, size_t length)
{
	if (length > sizeof(payload_buf)) {
		return -EMSGSIZE;
	}

	return mqtt_readall_publish_payload(c, payload_buf, length);
}

/**@brief Function to handle received publish event.
 */
static int handle_mqtt_publish_evt(struct mqtt_client *const c, const struct mqtt_evt *evt)
{
	int ret;

	ret = publish_get_payload(c, evt->param.publish.message.payload.len);
	if (ret < 0) {
		return ret;
	}

	sprintf(rsp_buf, "\r\n#XMQTTMSG: %d,%d\r\n",
		evt->param.publish.message.topic.topic.size,
		evt->param.publish.message.payload.len);
	rsp_send(rsp_buf, strlen(rsp_buf));
	rsp_send(evt->param.publish.message.topic.topic.utf8,
		evt->param.publish.message.topic.topic.size);
	rsp_send("\r\n", 2);
	rsp_send(payload_buf, evt->param.publish.message.payload.len);
	rsp_send("\r\n", 2);

	return 0;
}

/**@brief MQTT client event handler
 */
void mqtt_evt_handler(struct mqtt_client *const c, const struct mqtt_evt *evt)
{
	int ret;

	ret = evt->result;
	switch (evt->type) {
	case MQTT_EVT_CONNACK:
		if (evt->result != 0) {
			ctx.connected = false;
		}
		break;

	case MQTT_EVT_DISCONNECT:
		ctx.connected = false;
		break;

	case MQTT_EVT_PUBLISH:
		ret = handle_mqtt_publish_evt(c, evt);
		break;

	case MQTT_EVT_PUBACK:
		if (evt->result == 0) {
			LOG_DBG("PUBACK packet id: %u", evt->param.puback.message_id);
		}
		break;

	case MQTT_EVT_PUBREC:
		if (evt->result != 0) {
			break;
		}
		LOG_DBG("PUBREC packet id: %u", evt->param.pubrec.message_id);
		{
			struct mqtt_pubrel_param param = {
				.message_id = evt->param.pubrel.message_id
			};
			ret = mqtt_publish_qos2_release(&client, &param);
			if (ret) {
				LOG_ERR("mqtt_publish_qos2_release: Fail! %d", ret);
			} else {
				LOG_DBG("Release, id %u", evt->param.pubrec.message_id);
			}
		}
		break;

	case MQTT_EVT_PUBREL:
		if (evt->result != 0) {
			break;
		}
		LOG_DBG("PUBREL packet id %u", evt->param.pubrel.message_id);
		{
			struct mqtt_pubcomp_param param = {
				.message_id = evt->param.pubrel.message_id
			};
			ret = mqtt_publish_qos2_complete(&client, &param);
			if (ret) {
				LOG_ERR("mqtt_publish_qos2_complete Failed:%d", ret);
			} else {
				LOG_DBG("Complete, id %u", evt->param.pubrel.message_id);
			}
		}
		break;

	case MQTT_EVT_PUBCOMP:
		if (evt->result == 0) {
			LOG_DBG("PUBCOMP packet id %u", evt->param.pubcomp.message_id);
		}
		break;

	case MQTT_EVT_SUBACK:
		if (evt->result == 0) {
			LOG_DBG("SUBACK packet id: %u", evt->param.suback.message_id);
		}
		break;

	default:
		LOG_DBG("default: %d", evt->type);
		break;
	}

	sprintf(rsp_buf, "\r\n#XMQTTEVT: %d,%d\r\n",
		evt->type, ret);
	rsp_send(rsp_buf, strlen(rsp_buf));
}

static void mqtt_thread_fn(void *arg1, void *arg2, void *arg3)
{
	int err = 0;
	struct pollfd fds;

	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	fds.fd = client.transport.tcp.sock;
#if defined(CONFIG_MQTT_LIB_TLS)
	if (client.transport.type == MQTT_TRANSPORT_SECURE) {
		fds.fd = client.transport.tls.sock;
	}
#endif
	fds.events = POLLIN;
	while (true) {
		if (!ctx.connected) {
			LOG_WRN("MQTT disconnected");
			break;
		}
		err = poll(&fds, 1, mqtt_keepalive_time_left(&client));
		if (err < 0) {
			LOG_ERR("ERROR: poll %d", errno);
			break;
		}
		/* timeout or revent, send KEEPALIVE */
		(void)mqtt_live(&client);

		if ((fds.revents & POLLIN) == POLLIN) {
			err = mqtt_input(&client);
			if (err != 0) {
				LOG_ERR("ERROR: mqtt_input %d", err);
				mqtt_abort(&client);
				break;
			}
		}
		if ((fds.revents & POLLERR) == POLLERR) {
			LOG_ERR("POLLERR");
			mqtt_abort(&client);
			break;
		}
		if ((fds.revents & POLLHUP) == POLLHUP) {
			LOG_ERR("POLLHUP");
			mqtt_abort(&client);
			break;
		}
		if ((fds.revents & POLLNVAL) == POLLNVAL) {
			LOG_ERR("POLLNVAL");
			mqtt_abort(&client);
			break;
		}
	}

	LOG_INF("MQTT thread terminated");
}

/**@brief Resolves the configured hostname and
 * initializes the MQTT broker structure
 */
static int broker_init(void)
{
	int err;
	struct addrinfo *result;
	struct addrinfo hints = {
		.ai_family = ctx.family,
		.ai_socktype = SOCK_STREAM
	};

	err = getaddrinfo(mqtt_broker_url, NULL, &hints, &result);
	if (err) {
		LOG_ERR("ERROR: getaddrinfo failed %d", err);
		return err;
	}

	if (ctx.family == AF_INET) {
		ctx.broker = *(struct sockaddr_in *)result->ai_addr;
		ctx.broker.sin_port = htons(mqtt_broker_port);
	} else {
		ctx.broker6 = *(struct sockaddr_in6 *)result->ai_addr;
		ctx.broker6.sin6_port = htons(mqtt_broker_port);
	}

	/* Free the address. */
	freeaddrinfo(result);
	return 0;
}

/**@brief Initialize the MQTT client structure
 */
static void client_init(void)
{
	/* Init MQTT client */
	mqtt_client_init(&client);

	/* MQTT client configuration */
	if (ctx.family == AF_INET) {
		client.broker = &ctx.broker;
	} else {
		client.broker = &ctx.broker6;
	}
	client.evt_cb = mqtt_evt_handler;
	client.client_id.utf8 = mqtt_clientid;
	client.client_id.size = strlen(mqtt_clientid);
	client.password = NULL;
	if (ctx.username.size > 0) {
		client.user_name = &ctx.username;
		if (ctx.password.size > 0) {
			client.password = &ctx.password;
		}
	} else {
		client.user_name = NULL;
		/* ignore password if no user_name */
	}
	client.protocol_version = MQTT_VERSION_3_1_1;

	/* MQTT buffers configuration */
	client.rx_buf = rx_buffer;
	client.rx_buf_size = sizeof(rx_buffer);
	client.tx_buf = tx_buffer;
	client.tx_buf_size = sizeof(tx_buffer);

#if defined(CONFIG_MQTT_LIB_TLS)
	/* MQTT transport configuration */
	if (ctx.sec_tag != INVALID_SEC_TAG) {
		struct mqtt_sec_config *tls_config;

		tls_config = &(client.transport).tls.config;
		tls_config->peer_verify   = TLS_PEER_VERIFY_REQUIRED;
		tls_config->cipher_list   = NULL;
		tls_config->cipher_count  = 0;
		tls_config->sec_tag_count = 1;
		tls_config->sec_tag_list  = (int *)&ctx.sec_tag;
		tls_config->hostname      = mqtt_broker_url;
		client.transport.type     = MQTT_TRANSPORT_SECURE;
	} else {
		client.transport.type     = MQTT_TRANSPORT_NON_SECURE;
	}
#else
	client.transport.type = MQTT_TRANSPORT_NON_SECURE;
#endif
}

static int do_mqtt_connect(void)
{
	int err;

	if (ctx.connected) {
		return -EISCONN;
	}

	/* Init MQTT broker */
	err = broker_init();
	if (err) {
		return err;
	}

	/* Connect to MQTT broker */
	client_init();
	err = mqtt_connect(&client);
	if (err != 0) {
		LOG_ERR("ERROR: mqtt_connect %d", err);
		return err;
	}

	k_thread_create(&mqtt_thread, mqtt_thread_stack,
			K_THREAD_STACK_SIZEOF(mqtt_thread_stack),
			mqtt_thread_fn, NULL, NULL, NULL,
			THREAD_PRIORITY, K_USER, K_NO_WAIT);

	ctx.connected = true;
	return 0;
}

static int do_mqtt_disconnect(void)
{
	int err;

	if (!ctx.connected) {
		return -ENOTCONN;
	}

	err = mqtt_disconnect(&client);
	if (err) {
		LOG_ERR("ERROR: mqtt_disconnect %d", err);
		return err;
	}

	if (k_thread_join(&mqtt_thread, K_SECONDS(CONFIG_MQTT_KEEPALIVE)) != 0) {
		LOG_WRN("Wait for thread terminate failed");
	}

	slm_at_mqtt_uninit();

	return err;
}

static int do_mqtt_publish(uint8_t *msg, size_t msg_len)
{
	pub_param.message.payload.data = msg;
	pub_param.message.payload.len  = msg_len;

	return mqtt_publish(&client, &pub_param);
}

static int do_mqtt_subscribe(uint16_t op,
				uint8_t *topic_buf,
				size_t topic_len,
				uint16_t qos)
{
	int err = -EINVAL;
	struct mqtt_topic subscribe_topic;
	static uint16_t sub_message_id;

	sub_message_id++;
	if (sub_message_id == UINT16_MAX) {
		sub_message_id = 1;
	}

	const struct mqtt_subscription_list subscription_list = {
		.list = &subscribe_topic,
		.list_count = 1,
		.message_id = sub_message_id
	};

	if (qos <= MQTT_QOS_2_EXACTLY_ONCE) {
		subscribe_topic.qos = (uint8_t)qos;
	} else {
		return err;
	}
	subscribe_topic.topic.utf8 = topic_buf;
	subscribe_topic.topic.size = topic_len;

	if (op == 1) {
		err = mqtt_subscribe(&client, &subscription_list);
	} else if (op == 0) {
		err = mqtt_unsubscribe(&client, &subscription_list);
	}

	return err;
}

/**@brief handle AT#XMQTTCON commands
 *  AT#XMQTTCON=<op>[,<cid>,<username>,<password>,<url>,<port>[,<sec_tag>]]
 *  AT#XMQTTCON?
 *  AT#XMQTTCON=?
 */
int handle_at_mqtt_connect(enum at_cmd_type cmd_type)
{
	int err = -EINVAL;
	uint16_t op;

	switch (cmd_type) {
	case AT_CMD_TYPE_SET_COMMAND:
		err = at_params_unsigned_short_get(&at_param_list, 1, &op);
		if (err) {
			return err;
		}
		if (op == MQTTC_CONNECT || op == MQTTC_CONNECT6)  {
			size_t clientid_sz = sizeof(mqtt_clientid);
			size_t username_sz = sizeof(mqtt_username);
			size_t password_sz = sizeof(mqtt_password);
			size_t url_sz = sizeof(mqtt_broker_url);

			err = util_string_get(&at_param_list, 2, mqtt_clientid, &clientid_sz);
			if (err) {
				return err;
			}
			err = util_string_get(&at_param_list, 3, mqtt_username, &username_sz);
			if (err) {
				return err;
			} else {
				ctx.username.utf8 = mqtt_username;
				ctx.username.size = strlen(mqtt_username);
			}
			err = util_string_get(&at_param_list, 4, mqtt_password, &password_sz);
			if (err) {
				return err;
			} else {
				ctx.password.utf8 = mqtt_password;
				ctx.password.size = strlen(mqtt_password);
			}
			err = util_string_get(&at_param_list, 5, mqtt_broker_url, &url_sz);
			if (err) {
				return err;
			}
			err = at_params_unsigned_short_get(&at_param_list, 6, &mqtt_broker_port);
			if (err) {
				return err;
			}
			ctx.sec_tag = INVALID_SEC_TAG;
			if (at_params_valid_count_get(&at_param_list) > 7) {
				err = at_params_unsigned_int_get(&at_param_list, 7, &ctx.sec_tag);
				if (err) {
					return err;
				}
			}
			ctx.family = (op == MQTTC_CONNECT) ? AF_INET : AF_INET6;
			err = do_mqtt_connect();
		} else if (op == MQTTC_DISCONNECT) {
			err = do_mqtt_disconnect();
		} else {
			err = -EINVAL;
		}
		break;

	case AT_CMD_TYPE_READ_COMMAND:
		if (ctx.sec_tag != INVALID_SEC_TAG) {
			sprintf(rsp_buf, "\r\n#XMQTTCON: %d,\"%s\",\"%s\",%d,%d\r\n",
				ctx.connected, mqtt_clientid, mqtt_broker_url, mqtt_broker_port,
				ctx.sec_tag);
		} else {
			sprintf(rsp_buf, "\r\n#XMQTTCON: %d,\"%s\",\"%s\",%d\r\n",
				ctx.connected, mqtt_clientid, mqtt_broker_url, mqtt_broker_port);
		}
		rsp_send(rsp_buf, strlen(rsp_buf));
		err = 0;
		break;

	case AT_CMD_TYPE_TEST_COMMAND:
		sprintf(rsp_buf, "\r\n#XMQTTCON: (0,1,2),<cid>,<username>,"
				 "<password>,<url>,<port>,<sec_tag>\r\n");
		rsp_send(rsp_buf, strlen(rsp_buf));
		err = 0;
		break;

	default:
		break;
	}

	return err;
}

static int mqtt_datamode_callback(uint8_t op, const uint8_t *data, int len)
{
	int ret = 0;

	if (op == DATAMODE_SEND) {
		ret = do_mqtt_publish((uint8_t *)data, len);
		LOG_INF("datamode send: %d", ret);
	} else if (op == DATAMODE_EXIT) {
		LOG_DBG("MQTT datamode exit");
	}

	return ret;
}

/**@brief handle AT#XMQTTPUB commands
 *  AT#XMQTTPUB=<topic>[,<msg>[,<qos>[,<retain>]]]
 *  AT#XMQTTPUB? READ command not supported
 *  AT#XMQTTPUB=?
 */
int handle_at_mqtt_publish(enum at_cmd_type cmd_type)
{
	int err = -EINVAL;

	uint16_t qos = MQTT_QOS_0_AT_MOST_ONCE;
	uint16_t retain = 0;
	size_t topic_sz = MQTT_MAX_TOPIC_LEN;
	size_t msg_sz = MQTT_MESSAGE_BUFFER_LEN;
	uint16_t param_count = at_params_valid_count_get(&at_param_list);

	switch (cmd_type) {
	case AT_CMD_TYPE_SET_COMMAND:
		err = util_string_get(&at_param_list, 1, pub_topic, &topic_sz);
		if (err) {
			return err;
		}
		pub_msg[0] = '\0';
		if (at_params_type_get(&at_param_list, 3) == AT_PARAM_TYPE_STRING) {
			err = util_string_get(&at_param_list, 3, pub_msg, &msg_sz);
			if (err) {
				return err;
			}
			if (param_count > 4) {
				err = at_params_unsigned_short_get(&at_param_list, 4, &qos);
				if (err) {
					return err;
				}
			}
			if (param_count > 5) {
				err = at_params_unsigned_short_get(&at_param_list, 5, &retain);
				if (err) {
					return err;
				}
			}
		} else if (at_params_type_get(&at_param_list, 3) == AT_PARAM_TYPE_NUM_INT) {
			err = at_params_unsigned_short_get(&at_param_list, 3, &qos);
			if (err) {
				return err;
			}
			if (param_count > 4) {
				err = at_params_unsigned_short_get(&at_param_list, 4, &retain);
				if (err) {
					return err;
				}
			}
		}

		/* common publish parameters*/
		if (qos <= MQTT_QOS_2_EXACTLY_ONCE) {
			pub_param.message.topic.qos = (uint8_t)qos;
		} else {
			return -EINVAL;
		}
		if (retain <= 1) {
			pub_param.retain_flag = (uint8_t)retain;
		} else {
			return -EINVAL;
		}
		pub_param.message.topic.topic.utf8 = pub_topic;
		pub_param.message.topic.topic.size = topic_sz;
		pub_param.dup_flag = 0;
		pub_param.message_id++;
		if (pub_param.message_id == UINT16_MAX) {
			pub_param.message_id = 1;
		}
		if (strlen(pub_msg) == 0) {
			/* Publish payload in data mode */
			err = enter_datamode(mqtt_datamode_callback);
		} else {
			err = do_mqtt_publish(pub_msg, msg_sz);
		}
		break;

	case AT_CMD_TYPE_TEST_COMMAND:
		sprintf(rsp_buf, "\r\n#XMQTTPUB: <topic>,<msg>,(0,1,2),(0,1)\r\n");
		rsp_send(rsp_buf, strlen(rsp_buf));
		err = 0;
		break;

	default:
		break;
	}

	return err;
}

/**@brief handle AT#XMQTTSUB commands
 *  AT#XMQTTSUB=<topic>,<qos>
 *  AT#XMQTTSUB? READ command not supported
 *  AT#XMQTTSUB=?
 */
int handle_at_mqtt_subscribe(enum at_cmd_type cmd_type)
{
	int err = -EINVAL;
	uint16_t qos;
	char topic[MQTT_MAX_TOPIC_LEN];
	int topic_sz = MQTT_MAX_TOPIC_LEN;

	switch (cmd_type) {
	case AT_CMD_TYPE_SET_COMMAND:
		if (at_params_valid_count_get(&at_param_list) == 3) {
			err = util_string_get(&at_param_list, 1, topic, &topic_sz);
			if (err < 0) {
				return err;
			}
			err = at_params_unsigned_short_get(&at_param_list, 2, &qos);
			if (err < 0) {
				return err;
			}
			err = do_mqtt_subscribe(AT_MQTTSUB_SUB, topic, topic_sz, qos);
		} else {
			return -EINVAL;
		}
		break;

	case AT_CMD_TYPE_TEST_COMMAND:
		sprintf(rsp_buf, "\r\n#XMQTTSUB: <topic>,(0,1,2)\r\n");
		rsp_send(rsp_buf, strlen(rsp_buf));
		err = 0;
		break;

	default:
		break;
	}

	return err;
}

/**@brief handle AT#XMQTTUNSUB commands
 *  AT#XMQTTUNSUB=<topic>
 *  AT#XMQTTUNSUB? READ command not supported
 *  AT#XMQTTUNSUB=?
 */
int handle_at_mqtt_unsubscribe(enum at_cmd_type cmd_type)
{
	int err = -EINVAL;
	char topic[MQTT_MAX_TOPIC_LEN];
	int topic_sz = MQTT_MAX_TOPIC_LEN;

	switch (cmd_type) {
	case AT_CMD_TYPE_SET_COMMAND:
		if (at_params_valid_count_get(&at_param_list) == 2) {
			err = util_string_get(&at_param_list, 1,
							topic, &topic_sz);
			if (err < 0) {
				return err;
			}
			err = do_mqtt_subscribe(AT_MQTTSUB_UNSUB,
						topic, topic_sz, 0);
		} else {
			return -EINVAL;
		}
		break;

	case AT_CMD_TYPE_TEST_COMMAND:
		sprintf(rsp_buf, "\r\n#XMQTTUNSUB: <topic>\r\n");
		rsp_send(rsp_buf, strlen(rsp_buf));
		err = 0;
		break;

	default:
		break;
	}

	return err;
}

int slm_at_mqtt_init(void)
{
	pub_param.message_id = 0;
	memset(&ctx, 0, sizeof(ctx));
	ctx.sec_tag = INVALID_SEC_TAG;

	return 0;
}

int slm_at_mqtt_uninit(void)
{
	client.broker = NULL;

	return 0;
}
