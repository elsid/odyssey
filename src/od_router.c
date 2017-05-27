
/*
 * odissey.
 *
 * PostgreSQL connection pooler and request router.
*/

#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>

#include <machinarium.h>
#include <soprano.h>

#include "od_macro.h"
#include "od_version.h"
#include "od_list.h"
#include "od_pid.h"
#include "od_syslog.h"
#include "od_log.h"
#include "od_daemon.h"
#include "od_scheme.h"
#include "od_lex.h"
#include "od_config.h"
#include "od_msg.h"
#include "od_system.h"
#include "od_instance.h"

#include "od_server.h"
#include "od_server_pool.h"
#include "od_client.h"
#include "od_client_pool.h"
#include "od_route_id.h"
#include "od_route.h"
#include "od_route_pool.h"
#include "od_io.h"

#include "od_pooler.h"
#include "od_relay.h"
#include "od_frontend.h"
#include "od_router.h"

typedef struct
{
	od_routerstatus_t status;
	od_client_t *client;
	machine_queue_t response;
} od_msgrouter_t;

typedef struct
{
	od_route_t *route;
	od_server_t *server;
} od_msgrouter_push_t;

static od_route_t*
od_router_fwd(od_router_t *router, so_bestartup_t *startup)
{
	od_instance_t *instance = router->system->instance;

	assert(startup->database != NULL);
	assert(startup->user != NULL);

	/* match required route according to scheme */
	od_schemeroute_t *route_scheme;
	route_scheme =
		od_schemeroute_match(&instance->scheme,
		                     so_parameter_value(startup->database));
	if (route_scheme == NULL) {
		/* try to use default route */
		route_scheme = instance->scheme.routing_default;
		if (route_scheme == NULL)
			return NULL;
	}
	od_routeid_t id = {
		.database     = so_parameter_value(startup->database),
		.database_len = startup->database->value_len,
		.user         = so_parameter_value(startup->user),
		.user_len     = startup->user->value_len
	};

	/* force settings required by route */
	if (route_scheme->database) {
		id.database = route_scheme->database;
		id.database_len = strlen(route_scheme->database) + 1;
	}
	if (route_scheme->user) {
		id.user = route_scheme->user;
		id.user_len = strlen(route_scheme->user) + 1;
	}

	/* match or create dynamic route */
	od_route_t *route;
	route = od_routepool_match(&router->route_pool, &id);
	if (route)
		return route;
	route = od_routepool_new(&router->route_pool, route_scheme, &id);
	if (route == NULL) {
		od_error(&instance->log, NULL, "failed to allocate route");
		return NULL;
	}
	return route;
}

static inline void
od_router(void *arg)
{
	od_router_t *router = arg;
	od_instance_t *instance = router->system->instance;

	od_log(&instance->log, NULL, "router: started");

	for (;;)
	{
		machine_msg_t msg;
		msg = machine_queue_get(router->queue, UINT32_MAX);
		if (msg == NULL)
			break;

		od_msg_t msg_type;
		msg_type = machine_msg_get_type(msg);
		switch (msg_type) {
		case OD_MROUTER_ATTACH:
		{
			/* attach client to route */
			od_msgrouter_t *msg_attach;
			msg_attach = machine_msg_get_data(msg);

			od_route_t *route;
			route = od_router_fwd(router, &msg_attach->client->startup);
			if (route == NULL) {
				msg_attach->status = OD_RERROR_NOT_FOUND;
				machine_queue_put(msg_attach->response, msg);
				continue;
			}

			/* ensure route client_max limit */
			int client_total;
			client_total = od_clientpool_total(&route->client_pool);
			if (client_total >= route->scheme->client_max) {
				od_log(&instance->log, NULL,
				       "router: route '%s' client_max reached (%d)",
				       route->scheme->target,
				       route->scheme->client_max);
				msg_attach->status = OD_RERROR_LIMIT;
				machine_queue_put(msg_attach->response, msg);
				continue;
			}

			/*od_clientpool_set(&route->client_pool, msg_attach->client, OD_CPENDING);*/
			msg_attach->client->route = route;
			msg_attach->status = OD_ROK;
			machine_queue_put(msg_attach->response, msg);
			continue;
		}

		case OD_MROUTER_POP:
		{
			/* get client server from route server pool */
			od_msgrouter_t *msg_pop;
			msg_pop = machine_msg_get_data(msg);

			od_route_t *route;
			route = msg_pop->client->route;
			assert(route != NULL);

			break;
		}

		case OD_MROUTER_PUSH:
		{
			/* push client server back to route server pool */
			od_msgrouter_push_t *msg_push;
			msg_push = machine_msg_get_data(msg);

			od_serverpool_set(&msg_push->route->server_pool,
			                   msg_push->server,
			                   OD_SIDLE);
			/* todo: wakeup waiters */
			break;
		}
		default:
			assert(0);
			break;
		}

		machine_msg_free(msg);
	}

}

int od_router_init(od_router_t *router, od_system_t *system)
{
	od_instance_t *instance = system->instance;
	od_routepool_init(&router->route_pool);
	router->machine = -1;
	router->system = system;
	router->queue = machine_queue_create();
	if (router->queue == NULL) {
		od_error(&instance->log, NULL, "failed to create router queue");
		return -1;
	}
	return 0;
}

int od_router_start(od_router_t *router)
{
	od_instance_t *instance = router->system->instance;
	router->machine = machine_create("router", od_router, router);
	if (router->machine == -1) {
		od_error(&instance->log, NULL, "failed to start router");
		return 1;
	}
	return 0;
}

od_routerstatus_t
od_route(od_router_t *router, od_client_t *client)
{
	/* create response queue */
	machine_queue_t response;
	response = machine_queue_create();
	if (response == NULL)
		return OD_RERROR;

	/* send attach request to router */
	machine_msg_t msg;
	msg = machine_msg_create(OD_MROUTER_ATTACH, sizeof(od_msgrouter_t));
	if (msg == NULL) {
		machine_queue_free(response);
		return OD_RERROR;
	}
	od_msgrouter_t *msg_attach;
	msg_attach = machine_msg_get_data(msg);
	msg_attach->status = OD_RERROR;
	msg_attach->client = client;
	msg_attach->response = response;
	machine_queue_put(router->queue, msg);

	/* wait for reply */
	msg = machine_queue_get(response, UINT32_MAX);
	if (msg == NULL) {
		/* todo:  */
		machine_queue_free(response);
		return OD_RERROR;
	}
	msg_attach = machine_msg_get_data(msg);
	od_routerstatus_t status;
	status = msg_attach->status;
	machine_queue_free(response);
	machine_msg_free(msg);
	return status;
}

od_routerstatus_t
od_route_pop(od_router_t *router, od_client_t *client)
{
	/* create response queue */
	machine_queue_t response;
	response = machine_queue_create();
	if (response == NULL)
		return OD_RERROR;

	/* send pop request to router */
	machine_msg_t msg;
	msg = machine_msg_create(OD_MROUTER_POP, sizeof(od_msgrouter_t));
	if (msg == NULL) {
		machine_queue_free(response);
		return OD_RERROR;
	}
	od_msgrouter_t *msg_pop;
	msg_pop = machine_msg_get_data(msg);
	msg_pop->status = OD_RERROR;
	msg_pop->client = client;
	msg_pop->response = response;
	machine_queue_put(router->queue, msg);

	/* wait for reply */
	msg = machine_queue_get(response, UINT32_MAX);
	if (msg == NULL) {
		/* todo:  */
		machine_queue_free(response);
		return OD_RERROR;
	}
	msg_pop = machine_msg_get_data(msg);
	od_routerstatus_t status;
	status = msg_pop->status;
	machine_queue_free(response);
	machine_msg_free(msg);
	return status;
}

void
od_route_push(od_router_t *router, od_client_t *client)
{
	/* send server push request to router */
	machine_msg_t msg;
	msg = machine_msg_create(OD_MROUTER_PUSH, sizeof(od_msgrouter_push_t));
	if (msg == NULL)
		return;
	od_msgrouter_push_t *msg_push;
	msg_push = machine_msg_get_data(msg);
	msg_push->route = client->route;
	msg_push->server = client->server;
	machine_queue_put(router->queue, msg);

	client->server = NULL;
}