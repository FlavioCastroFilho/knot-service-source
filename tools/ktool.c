/*
 * This file is part of the KNOT Project
 *
 * Copyright (c) 2015, CESAR. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *    * Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *    * Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer in the
 *      documentation and/or other materials provided with the distribution.
 *    * Neither the name of the CESAR nor the
 *      names of its contributors may be used to endorse or promote products
 *      derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL CESAR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
 * EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <math.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <glib.h>

#include <json-c/json.h>

#include <proto-net/knot_proto_net.h>
#include <proto-app/knot_types.h>
#include <proto-app/knot_proto_app.h>

/* Abstract unit socket namespace */
#define KNOT_UNIX_SOCKET	"knot"

struct schema {
	GSList *list;
	int err;
};

typedef void (*json_object_func_t) (struct json_object *jobj,
					const char *key, void *user_data);

static int sock;
static gboolean opt_add = FALSE;
static gboolean opt_rm = FALSE;
static gboolean opt_schema = FALSE;
static gboolean opt_data = FALSE;
static char *opt_uuid = NULL;
static char *opt_token = NULL;
static char *opt_json = NULL;
static gboolean opt_id = FALSE;
static gboolean opt_subs = FALSE;
static gboolean opt_unsubs = FALSE;

static GMainLoop *main_loop;

static int unix_connect(void)
{
	int err, sock;
	struct sockaddr_un addr;

	sock = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
	if (sock < 0)
		return -errno;

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	/* Abstract namespace: first character must be null */
	strncpy(addr.sun_path + 1, KNOT_UNIX_SOCKET, strlen(KNOT_UNIX_SOCKET));

	if (connect(sock, (struct sockaddr *) &addr, sizeof(addr)) == -1) {
		err = -errno;
		close(sock);
		return err;
	}

	return sock;
}

static void print_json_value(struct json_object *jobj,
					const char *key, void *user_data)
{
	enum json_type type;
	const char *name;

	type = json_object_get_type(jobj);
	name = json_type_to_name(type);

	switch (type) {
	case json_type_null:
		break;
	case json_type_boolean:
		printf("%s(%s)\n", json_object_get_boolean(jobj) ?
					"true": "false", name);
		break;
	case json_type_double:
		printf("%lf(%s)\n", json_object_get_double(jobj), name);
		break;
	case json_type_int:
		printf("%d(%s)\n", json_object_get_int(jobj), name);
		break;
	case json_type_string:
		printf("%s(%s)\n", json_object_get_string(jobj), name);
		break;
	case json_type_object:
		break;
	case json_type_array:
		break;
	}
}

static void load_schema(struct json_object *jobj,
					const char *key, void *user_data)
{
	struct schema *schema = user_data;
	knot_schema *entry;
	GSList *ltmp;
	enum json_type type;
	const char *data_name = NULL;
	int intval, err = EINVAL;

	/*
	 * This callback is called for all entries: skip
	 * parsing if one error has been detected previously.
	 */
	if (schema->err)
		return;

	type = json_object_get_type(jobj);

	switch (type) {
	case json_type_null:
	case json_type_boolean:
	case json_type_double:
	case json_type_object:
	case json_type_array:
		/* Not available */
		break;
	case json_type_int:
		intval = json_object_get_int(jobj);

		if (strcmp("sensor_id", key) == 0) {
			entry = g_new0(knot_schema, 1);
			entry->sensor_id = intval;
			schema->list = g_slist_append(schema->list, entry);
			err = 0;
		} else if (strcmp("type_id", key) == 0) {
			ltmp = g_slist_last(schema->list);
			if (!ltmp)
				goto done;

			/* FIXME: if type_id appers before sensor_id? */
			entry = ltmp->data;
			entry->type_id = intval;
			err = 0;
		}

		break;
	case json_type_string:
		data_name = json_object_get_string(jobj);

		if (strcmp("name", key) != 0 || data_name == NULL)
			goto done;

		ltmp = g_slist_last(schema->list);
		if (!ltmp)
			goto done;

		/* FIXME: if name comes before sensor_id or type_id  */
		entry = ltmp->data;
		strcpy(entry->name, data_name);
		err = 0;
		break;
	}

done:
	schema->err = err;
}

static void read_json_entry(struct json_object *jobj,
					const char *key, void *user_data)
{
	knot_msg_data *msg = user_data;
	knot_data *kdata = &(msg->payload);
	knot_data_bool *kbool;
	knot_data_float *kfloat;
	knot_data_int *kint;
	int32_t ipart, fpart;
	enum json_type type;
	const char *str;

	type = json_object_get_type(jobj);

	if ((strcmp("sensor_id", key) == 0) && (type == json_type_int))
		kdata->hdr.sensor_id = json_object_get_int(jobj);
	else if ((strcmp("unit", key) == 0) && (type == json_type_int)) {
		kdata->hdr.unit = json_object_get_int(jobj);
	} else if (strcmp("value", key) == 0) {
		switch (type) {
		case json_type_boolean:
			kbool = (knot_data_bool *) kdata;
			kbool->hdr.value_type = KNOT_VALUE_TYPE_BOOL;
			kbool->value = json_object_get_boolean(jobj);
			msg->hdr.payload_len = sizeof(knot_data_bool);
			break;
		case json_type_double:
			/* Trick to get integral and fractional parts */
			str = json_object_get_string(jobj);
			/* FIXME: how to handle overflow? */
			if (sscanf(str, "%d.%d", &ipart, &fpart) != 2)
				break;

			kfloat = (knot_data_float *) kdata;
			kfloat->hdr.value_type = KNOT_VALUE_TYPE_FLOAT;
			kfloat->value_int = ipart;
			kfloat->value_dec = fpart;
			kfloat->multiplier = 1; /* TODO: */
			msg->hdr.payload_len = sizeof(knot_data_float);
			break;
		case json_type_int:
			kint = (knot_data_int *) kdata;
			kint->hdr.value_type = KNOT_VALUE_TYPE_INT;
			kint->value = json_object_get_int(jobj);
			kint->multiplier = 1;
			msg->hdr.payload_len = sizeof(knot_data_int);
			break;
		case json_type_string:
		case json_type_null:
			/* FIXME: */
			break;

		/* FIXME: */
		case json_type_object:
			break;
		case json_type_array:
			break;
		}
	} else {
		printf("Unexpected JSON entry!\n");
	}
}

static void json_object_foreach(struct json_object *jobj,
				json_object_func_t func, void *user_data)
{
	struct json_object *next;
	enum json_type type;
	int len, i;

	if (!jobj)
		return;

	json_object_object_foreach(jobj, key, val) {
		type = json_object_get_type(val);
		switch (type) {
		case json_type_null:
		case json_type_boolean:
		case json_type_double:
		case json_type_int:
		case json_type_string:
			func(val, key, user_data);
			break;
		case json_type_object:
			next = json_object_get(val);
			json_object_foreach(next, func, user_data);
			json_object_put(next);
			break;
		case json_type_array:
			len = json_object_array_length(val);
			for (i = 0; i < len; i++) {
				next = json_object_array_get_idx(val, i);
				json_object_foreach(next, func, user_data);
			}
			break;
		}
	}
}

static int authenticate(const char *uuid, const char *token)
{
	knot_msg_authentication msg;
	knot_msg_result resp;
	ssize_t nbytes;
	int err;

	memset(&msg, 0, sizeof(msg));
	memset(&resp, 0, sizeof(resp));

	msg.hdr.type = KNOT_MSG_AUTH_REQ;
	msg.hdr.payload_len = sizeof(msg.uuid) + sizeof(msg.token);
	strncpy(msg.uuid, uuid, sizeof(msg.uuid));
	strncpy(msg.token, token, sizeof(msg.token));

	nbytes = write(sock, &msg, sizeof(msg.hdr) + msg.hdr.payload_len);
	if (nbytes < 0) {
		err = errno;
		printf("write(): %s(%d)\n", strerror(err), err);
		return -err;
	}

	nbytes = read(sock, &resp, sizeof(resp));
	if (nbytes < 0) {
		err = errno;
		printf("read(): %s(%d)\n", strerror(err), err);
		return -err;
	}

	if (resp.result != KNOT_SUCCESS) {
		printf("error(0x%02x)\n", resp.result);
		return -EPROTO;
	}

	return 0;
}

static int write_knot_data(struct json_object *jobj)
{
	knot_msg_data msg;
	knot_msg_result resp;
	ssize_t nbytes;
	int err;

	memset(&msg, 0, sizeof(msg));
	memset(&resp, 0, sizeof(resp));

	/*
	 * The current implementation is limited to only data entry.
	 * JSON files should not contain array elements.
	 */

	json_object_foreach(jobj, read_json_entry, &msg);

	if (msg.hdr.payload_len == 0) {
		printf("JSON parsing error: data not found!\n");
		return -EINVAL;
	}

	msg.hdr.type = KNOT_MSG_DATA;
	/* Payload len is set by read_json_entry() */

	nbytes = write(sock, &msg, sizeof(msg.hdr) + msg.hdr.payload_len);
	if (nbytes < 0) {
		err = errno;
		printf("write(): %s(%d)\n", strerror(err), err);
		return -err;
	}

	nbytes = read(sock, &resp, sizeof(resp));
	if (nbytes < 0) {
		err = errno;
		printf("read(): %s(%d)\n", strerror(err), err);
		return -err;
	}

	if (resp.result != KNOT_SUCCESS) {
		printf("error(0x%02x)\n", resp.result);
		return -EPROTO;
	}

	return 0;
}

static int send_schema(GSList *list)
{
	knot_msg_config msg;
	knot_schema *entry;
	GSList *l;
	ssize_t nbytes;
	int err;

	memset(&msg, 0, sizeof(msg));
	msg.hdr.type = KNOT_MSG_SCHEMA;

	for (l = list; l;) {
		entry = l->data;

		msg.hdr.payload_len = sizeof(*entry);
		memcpy(&msg.schema, entry, msg.hdr.payload_len);

		l = g_slist_next(l);
		if (!l)
			msg.hdr.type = KNOT_MSG_SCHEMA |
					KNOT_MSG_SCHEMA_FLAG_END;

		nbytes = write(sock, &msg, sizeof(msg.hdr) +
						msg.hdr.payload_len);
		if (nbytes < 0) {
			err = errno;
			printf("write(): %s(%d)\n", strerror(err), err);
			return -err;
		}
	}

	return 0;
}

static int cmd_register(void)
{
	knot_msg_register msg;
	knot_msg_credential crdntl;
	const char *devname = "dummy0";
	int len = strlen(devname);
	ssize_t nbytes;
	int err;

	memset(&msg, 0, sizeof(msg));
	msg.hdr.type = KNOT_MSG_REGISTER_REQ;
	msg.hdr.payload_len = len;
	strncpy(msg.devName, devname, len);

	nbytes = write(sock, &msg, sizeof(msg.hdr) + len);
	if (nbytes < 0) {
		err = errno;
		printf("writev(): %s(%d)\n", strerror(err), err);
		return -err;
	}

	memset(&crdntl, 0, sizeof(crdntl));
	nbytes = read(sock, &crdntl, sizeof(crdntl));
	if (nbytes < 0) {
		err = errno;
		printf("KNOT Register read(): %s(%d)\n", strerror(err), err);
		return -err;
	}

	if (crdntl.result != KNOT_SUCCESS) {
		printf("KNOT Register: error(0x%02x)\n", crdntl.result);
		return -EPROTO;
	}

	printf("UUID: %.*s\n", (int) sizeof(crdntl.uuid), crdntl.uuid);
	printf("TOKEN: %.*s\n", (int) sizeof(crdntl.token), crdntl.token);

	return 0;
}

static int cmd_unregister(void)
{
	knot_msg_unregister msg;
	knot_msg_result rslt;
	ssize_t nbytes;
	int err;

	/*
	 * When token is informed try authenticate first. Leave this
	 * block sequential to allow testing unregistering without
	 * previous authentication.
	 */

	if (opt_token) {
		printf("Authenticating ...\n");
		err = authenticate(opt_uuid, opt_token);
	}

	memset(&msg, 0, sizeof(msg));
	msg.hdr.type = KNOT_MSG_UNREGISTER_REQ;
	msg.hdr.payload_len = 0;

	nbytes = write(sock, &msg, sizeof(msg));
	if (nbytes < 0) {
		err = errno;
		printf("KNOT Unregister: %s(%d)\n", strerror(err), err);
		return -err;
	}

	memset(&rslt, 0, sizeof(rslt));
	nbytes = read(sock, &rslt, sizeof(rslt));
	if (nbytes < 0) {
		err = errno;
		printf("KNOT Unregister read(): %s(%d)\n", strerror(err), err);
		return -err;
	}

	if (rslt.result != KNOT_SUCCESS) {
		printf("KNOT Unregister: error(0x%02x)\n", rslt.result);
		return -EPROTO;
	}

	printf("KNOT Unregister: OK\n");

	return 0;
}

static int cmd_schema(void)
{
	struct json_object *jobj;
	struct schema schema;
	struct stat sb;
	int err;

	if (!opt_uuid) {
		printf("Device's UUID missing!\n");
		return -EINVAL;
	}

	/*
	 * When token is informed try authenticate first. Leave this
	 * block sequential to allow testing sending schema without
	 * previous authentication.
	 */
	if (opt_token) {
		printf("Authenticating ...\n");
		err = authenticate(opt_uuid, opt_token);
	}
	/*
	 * In order to allow a more flexible way to manage schemas, ktool
	 * receives a JSON file and convert it to KNOT protocol format.
	 * Variable length argument could be another alternative, however
	 * it will not be intuitive to the users to inform the data id, type,
	 * and values.
	 */

	if (!opt_json) {
		printf("Device's SCHEMA missing!\n");
		return -EINVAL;
	}

	if (stat(opt_json, &sb) == -1) {
		err = errno;
		printf("json file: %s(%d)\n", strerror(err), err);
		return -err;
	}

	if ((sb.st_mode & S_IFMT) != S_IFREG) {
		printf("json file: invalid argument!\n");
		return -EINVAL;
	}

	jobj = json_object_from_file(opt_json);
	if (!jobj) {
		printf("json file(%s): failed to read from file!\n", opt_json);
		return -EINVAL;
	}

	memset(&schema, 0, sizeof(schema));
	json_object_foreach(jobj, load_schema, &schema);
	if (!schema.err)
		send_schema(schema.list);

	g_slist_free_full(schema.list, g_free);
	json_object_put(jobj);

	return 0;
}

static int cmd_data(void)
{
	struct json_object *jobj;
	struct stat sb;
	int err;

	if (!opt_uuid) {
		printf("Device's UUID missing!\n");
		return -EINVAL;
	}

	/*
	 * When token is informed try authenticate first. Leave this
	 * block sequential to allow testing sending data without
	 * previous authentication.
	 */
	if (opt_token) {
		printf("Authenticating ...\n");
		err = authenticate(opt_uuid, opt_token);
	}

	/*
	 * In order to allow a more flexible way to manage data, ktool
	 * receives a JSON file and convert it to KNOT protocol format.
	 * Variable length argument could be another alternative, however
	 * it will not be intuitive to the users to inform the data id, type,
	 * and values.
	 */

	if (!opt_json) {
		printf("Device's data missing!\n");
		return -EINVAL;
	}

	if (stat(opt_json, &sb) == -1) {
		err = errno;
		printf("json file: %s(%d)\n", strerror(err), err);
		return -err;
	}

	if ((sb.st_mode & S_IFMT) != S_IFREG) {
		printf("json file: invalid argument!\n");
		return -EINVAL;
	}

	jobj = json_object_from_file(opt_json);
	if (!jobj) {
		printf("json file(%s): failed to read from file!\n", opt_json);
		return -EINVAL;
	}

	json_object_foreach(jobj, print_json_value, NULL);
	write_knot_data(jobj);
	json_object_put(jobj);

	return 0;
}

static int cmd_id(void)
{
	int err;
	ssize_t nbytes;
	uint8_t datagram[128];

	memset(datagram, 0, sizeof(datagram));

	/*
	 * TODO: set knot protocol headers and payload
	 * Send UUID and token to identify a previously
	 * registered device.
	 */
	nbytes = write(sock, datagram, sizeof(datagram));
	if (nbytes < 0) {
		err = errno;
		printf("write(): %s(%d)\n", strerror(err), err);
		return -err;
	}

	return 0;
}

static int cmd_subscribe(void)
{
	return -ENOSYS;
}

static int cmd_unsubscribe(void)
{
	return -ENOSYS;
}

/*
 * 'token' and 'uuid' are returned by registration process. Later a
 * command line prompt may be displayed to the user allowing an
 * interactive mode to be able to receive messages and change properties
 * on demand. Options should be provided to inform invalid 'token'/'uuid'
 * to allow testing error conditions, or inform previously registered
 * devices. Commands are based on KNOT protocol, and they should be mapped
 * to any specific backend.
 */

static GOptionEntry schema_options[] = {
	{ "uuid", 0, 0, G_OPTION_ARG_STRING, &opt_uuid,
						"Device's UUID", NULL },
	{ "token", 0, 0, G_OPTION_ARG_STRING, &opt_token,
						"Device's token", NULL },
	{ "json", 0, 0, G_OPTION_ARG_FILENAME, &opt_json,
						"Path to JSON file", NULL },
	{ NULL },
};

static GOptionEntry options[] = {

	{ "add", 'a', 0, G_OPTION_ARG_NONE, &opt_add,
				"Register a device to Meshblu",
				NULL },
	{ "remove", 'r', 0, G_OPTION_ARG_NONE, &opt_rm,
				"Unregister a device from Meshblu",
				NULL },
	{ "schema", 'h', 0, G_OPTION_ARG_NONE, &opt_schema,
				"Get/Put JSON representing device's schema",
				NULL },
	{ "data", 'd', 0, G_OPTION_ARG_NONE, &opt_data,
				"Sends data of a given device", NULL },
	{ "id", 'i', 0, G_OPTION_ARG_NONE, &opt_id,
				"Identify a Meshblu device",
				NULL },
	{ "subscribe", 's', 0, G_OPTION_ARG_NONE, &opt_subs,
				"Subscribe for messages of a given device",
				NULL },
	{ "unsubscribe", 'u', 0, G_OPTION_ARG_NONE, &opt_unsubs,
				"Unsubscribe for messages", NULL },
	{ NULL },
};

static void sig_term(int sig)
{
	g_main_loop_quit(main_loop);
}

int main(int argc, char *argv[])
{
	GOptionContext *context;
	GOptionGroup *schema_group;
	GError *gerr = NULL;
	int err = 0;

	printf("KNOT Tool\n");

	context = g_option_context_new(NULL);
	g_option_context_add_main_entries(context, options, NULL);

	/* Define options for setting or reading JSON schema of a given
	   device(UUID):
	    read:
		ktool --schema --uuid=value
	    write:
		ktool --schema --uuid=value --json=filename
	 */
	schema_group = g_option_group_new("schema", "Schema options",
					"Show all schema options", NULL, NULL);
	g_option_context_add_group(context, schema_group);
	g_option_group_add_entries(schema_group, schema_options);

	/* TODO: Use GOptionGroup to inform parameters */
	if (!g_option_context_parse(context, &argc, &argv, &gerr)) {
		printf("Invalid arguments: %s\n", gerr->message);
		g_error_free(gerr);
		g_option_context_free(context);
		exit(EXIT_FAILURE);
	}

	g_option_context_free(context);

	signal(SIGTERM, sig_term);
	signal(SIGINT, sig_term);
	main_loop = g_main_loop_new(NULL, FALSE);

	sock = unix_connect();
	if (sock == -1) {
		err = -errno;
		printf("connect(): %s (%d)\n", strerror(-err), -err);
		return err;
	}

	if (opt_add) {
		printf("Registering node ...\n");
		err = cmd_register();
	}

	if (opt_schema) {
		printf("Registering JSON schema for a device ...\n");
		err = cmd_schema();
	} else if (opt_data) {
		printf("Setting data for a device ...\n");
		err = cmd_data();
	} else if (opt_uuid) {
		printf("Unregistering node: %s\n", opt_uuid);
		err = cmd_unregister();
	} else if (opt_id) {
		printf("Identifying node ...\n");
		err = cmd_id();
	} else if (opt_subs) {
		printf("Subscribing node ...\n");
		err = cmd_subscribe();
	} else if (opt_unsubs) {
		printf("Unsubscribing node ...\n");
		err = cmd_unsubscribe();
	}

	if (err < 0) {
		close(sock);
		return err;
	}

	g_main_loop_run(main_loop);
	g_main_loop_unref(main_loop);

	printf("Exiting\n");

	close(sock);

	return 0;
}
