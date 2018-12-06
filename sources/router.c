
/*
 * Odyssey.
 *
 * Scalable PostgreSQL connection pooler.
*/

#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <inttypes.h>
#include <assert.h>

#include <machinarium.h>
#include <kiwi.h>
#include <odyssey.h>

void
od_router_init(od_router_t *router)
{
	pthread_mutex_init(&router->lock, NULL);
	od_rules_init(&router->rules);
	od_route_pool_init(&router->route_pool);
	router->clients = 0;
}

void
od_router_free(od_router_t *router)
{
	od_route_pool_free(&router->route_pool);
	od_rules_free(&router->rules);
	pthread_mutex_destroy(&router->lock);
}

inline int
od_router_foreach(od_router_t *router,
                  od_route_pool_cb_t callback,
                  void **argv)
{
	od_router_lock(router);
	int rc;
	rc = od_route_pool_foreach(&router->route_pool, callback, argv);
	od_router_unlock(router);
	return rc;
}

static inline int
od_router_kill_clients_cb(od_route_t *route, void **argv)
{
	(void)argv;
	if (! route->rule->obsolete)
		return 0;
	od_route_lock(route);
	od_route_kill_client_pool(route);
	od_route_unlock(route);
	return 0;
}

int
od_router_reconfigure(od_router_t *router, od_rules_t *rules)
{
	od_router_lock(router);

	int updates;
	updates = od_rules_merge(&router->rules, rules);

	if (updates > 0) {
		od_route_pool_foreach(&router->route_pool, od_router_kill_clients_cb,
		                      NULL);
	}

	od_router_unlock(router);
	return updates;
}

static inline int
od_router_expire_server_cb(od_server_t *server, void **argv)
{
	od_list_t *expire_list = argv[0];
	int *count = argv[1];
	od_list_append(expire_list, &server->link);
	(*count)++;
	return 0;
}

static inline int
od_router_expire_server_tick_cb(od_server_t *server, void **argv)
{
	od_route_t *route = server->route;
	od_list_t *expire_list = argv[0];
	int *count = argv[1];

	/* advance idle time for 1 sec */
	if (server->idle_time < route->rule->pool_ttl) {
		server->idle_time++;
		return 0;
	}

	/* remove server for server pool */
	od_server_pool_set(&route->server_pool, server, OD_SERVER_UNDEF);

	/* add to expire list */
	od_list_append(expire_list, &server->link);
	(*count)++;

	return 0;
}

static inline int
od_router_expire_cb(od_route_t *route, void **argv)
{
	od_route_lock(route);

	/* expire by config obsoletion */
	if (route->rule->obsolete && !od_client_pool_total(&route->client_pool))
	{
		od_server_pool_foreach(&route->server_pool,
		                       OD_SERVER_IDLE,
		                       od_router_expire_server_cb,
		                       argv);

		od_route_unlock(route);
		return 0;
	}

	if (! route->rule->pool_ttl) {
		od_route_unlock(route);
		return 0;
	}

	od_server_pool_foreach(&route->server_pool,
	                       OD_SERVER_IDLE,
	                       od_router_expire_server_tick_cb,
	                       argv);

	od_route_unlock(route);
	return 0;
}

int
od_router_expire(od_router_t *router, od_list_t *expire_list)
{
	int count = 0;
	void *argv[] = { expire_list, &count };
	od_router_foreach(router, od_router_expire_cb, argv);
	return count;
}

static inline int
od_router_gc_cb(od_route_t *route, void **argv)
{
	od_route_pool_t *pool = argv[0];
	od_route_lock(route);

	if (od_server_pool_total(&route->server_pool) > 0 ||
	    od_client_pool_total(&route->client_pool) > 0)
		goto done;

	if (!od_route_is_dynamic(route) && !route->rule->obsolete)
		goto done;

	/* remove route from route pool */
	assert(pool->count > 0);
	pool->count--;
	od_list_unlink(&route->link);

	od_route_unlock(route);

	/* unref route rule and free route object */
	od_rules_unref(route->rule);
	od_route_free(route);
	return 0;
done:
	od_route_unlock(route);
	return 0;
}

void
od_router_gc(od_router_t *router)
{
	void *argv[] = { &router->route_pool };
	od_router_foreach(router, od_router_gc_cb, argv);
}

void
od_router_stat(od_router_t *router,
               uint64_t prev_time_us,
               int prev_update,
               od_route_pool_stat_cb_t callback,
               void **argv)
{
	od_router_lock(router);
	od_route_pool_stat(&router->route_pool, prev_time_us, prev_update,
	                   callback, argv);
	od_router_unlock(router);
}

od_router_status_t
od_router_route(od_router_t *router, od_config_t *config, od_client_t *client)
{
	kiwi_be_startup_t *startup = &client->startup;

	/* match route */
	assert(startup->database != NULL);
	assert(startup->user != NULL);

	od_router_lock(router);

	/* match latest version of route rule */
	od_rule_t *rule;
	rule = od_rules_forward(&router->rules,
	                        kiwi_param_value(startup->database),
	                        kiwi_param_value(startup->user));
	if (rule == NULL) {
		od_router_unlock(router);
		return OD_ROUTER_ERROR_NOT_FOUND;
	}

	/* force settings required by route */
	od_route_id_t id = {
		.database     = kiwi_param_value(startup->database),
		.user         = kiwi_param_value(startup->user),
		.database_len = startup->database->value_len,
		.user_len     = startup->user->value_len
	};
	if (rule->storage_db) {
		id.database = rule->storage_db;
		id.database_len = strlen(rule->storage_db) + 1;
	}
	if (rule->storage_user) {
		id.user = rule->storage_user;
		id.user_len = strlen(rule->storage_user) + 1;
	}

	/* ensure global client_max limit */
	if (config->client_max_set && router->clients >= config->client_max) {
		od_router_unlock(router);
		return OD_ROUTER_ERROR_LIMIT;
	}

	/* match or create dynamic route */
	od_route_t *route;
	route = od_route_pool_match(&router->route_pool, &id, rule);
	if (route == NULL) {
		route = od_route_pool_new(&router->route_pool, &id, rule);
		if (route == NULL) {
			od_router_unlock(router);
			return OD_ROUTER_ERROR;
		}
	}
	router->clients++;
	od_rules_ref(rule);

	od_route_lock(route);
	od_router_unlock(router);

	/* ensure route client_max limit */
	if (rule->client_max_set &&
	    od_client_pool_total(&route->client_pool) >= rule->client_max) {
		od_route_unlock(route);
		od_router_lock(router);
		router->clients--;
		od_rules_unref(rule);
		od_router_unlock(router);
		return OD_ROUTER_ERROR_LIMIT_ROUTE;
	}

	/* add client to route client pool */
	od_client_pool_set(&route->client_pool, client, OD_CLIENT_PENDING);
	client->rule  = rule;
	client->route = route;

	od_route_unlock(route);
	return OD_ROUTER_OK;
}

void
od_router_unroute(od_router_t *router, od_client_t *client)
{
	/* detach client from route */
	assert(client->route);
	assert(client->server == NULL);

	od_router_lock(router);
	assert(router->clients > 0);
	router->clients--;
	od_router_unlock(router);

	od_route_t *route = client->route;
	od_route_lock(route);
	od_client_pool_set(&route->client_pool, client, OD_CLIENT_UNDEF);
	client->route = NULL;
	od_route_unlock(route);
}

od_router_status_t
od_router_attach(od_router_t *router, od_config_t *config, od_id_mgr_t *id_mgr,
                 od_client_t *client)
{
	(void)router;
	od_route_t *route = client->route;
	assert(route != NULL);

	od_route_lock(route);

	/* get client server from route server pool */
	od_server_t *server;
	for (;;)
	{
		server = od_server_pool_next(&route->server_pool, OD_SERVER_IDLE);
		if (server)
			goto attach;

		/* always start new connection, if pool_size is zero */
		if (route->rule->pool_size == 0)
			break;

		/* TODO: wait for free server */
#if 0
		/* enqueue client */
		od_client_pool_set(&route->client_pool, client, OD_CLIENT_QUEUE);
#endif
	}

	od_route_unlock(route);

	/* create new server object */
	server = od_server_allocate();
	if (server == NULL)
		return OD_ROUTER_ERROR;
	od_id_mgr_generate(id_mgr, &server->id, "s");
	server->global = client->global;
	server->route  = route;
	od_packet_set_chunk(&server->packet_reader, config->packet_read_size);

	od_route_lock(route);

	/* TODO */
	/* recheck for free server again? */

attach:
	od_server_pool_set(&route->server_pool, server, OD_SERVER_ACTIVE);
	od_client_pool_set(&route->client_pool, client, OD_CLIENT_ACTIVE);
	od_route_unlock(route);

	client->server     = server;
	server->client     = client;
	server->idle_time  = 0;
	server->key_client = client->key;

	/* attach server io to clients machine context */
	if (server->io && od_config_is_multi_workers(config))
		machine_io_attach(server->io);

	return OD_ROUTER_OK;
}

void
od_router_detach(od_router_t *router, od_config_t *config, od_client_t *client)
{
	(void)router;
	od_route_t *route = client->route;
	assert(route != NULL);

	/* detach from current machine event loop */
	od_server_t *server = client->server;
	if (od_config_is_multi_workers(config))
		machine_io_detach(server->io);

	od_route_lock(route);

	client->server = NULL;
	server->client = NULL;
	server->last_client_id = client->id;
	od_server_pool_set(&route->server_pool, server, OD_SERVER_IDLE);
	od_client_pool_set(&route->client_pool, client, OD_CLIENT_PENDING);

	/* TODO: wakeup waiters */
	/*
	if (route->client_pool.count_queue > 0) {
		od_client_t *waiter;
		waiter = od_client_pool_next(&route->client_pool, OD_CLIENT_QUEUE);
	}
	*/

	od_route_unlock(route);
}

void
od_router_close(od_router_t *router, od_client_t *client)
{
	(void)router;
	od_route_t *route = client->route;
	assert(route != NULL);

	od_server_t *server = client->server;
	od_backend_close_connection(server);

	od_route_lock(route);

	od_client_pool_set(&route->client_pool, client, OD_CLIENT_PENDING);
	od_server_pool_set(&route->server_pool, server, OD_SERVER_UNDEF);
	client->server         = NULL;
	server->last_client_id = client->id;
	server->client         = NULL;
	server->route          = NULL;

	od_route_unlock(route);

	assert(server->io == NULL);
	od_server_free(server);
}

static inline int
od_router_cancel_cmp(od_server_t *server, void **argv)
{
	return kiwi_key_cmp(&server->key_client, argv[0]);
}

static inline int
od_router_cancel_cb(od_route_t *route, void **argv)
{
	od_route_lock(route);

	od_server_t *server;
	server = od_server_pool_foreach(&route->server_pool, OD_SERVER_ACTIVE,
	                                od_router_cancel_cmp, argv);
	if (server)
	{
		od_router_cancel_t *cancel = argv[1];
		cancel->id     = server->id;
		cancel->key    = server->key;
		cancel->storage = od_rules_storage_copy(route->rule->storage);
		od_route_unlock(route);
		if (cancel->storage == NULL)
			return -1;
		return 1;
	}

	od_route_unlock(route);
	return 0;
}

od_router_status_t
od_router_cancel(od_router_t *router, kiwi_key_t *key, od_router_cancel_t *cancel)
{
	/* match server by client forged key */
	void *argv[] = { key, cancel };
	int rc;
	rc = od_router_foreach(router, od_router_cancel_cb, argv);
	if (rc <= 0)
		return OD_ROUTER_ERROR_NOT_FOUND;
	return OD_ROUTER_OK;
}

static inline int
od_router_kill_cb(od_route_t *route, void **argv)
{
	od_route_lock(route);
	od_route_kill_client(route, argv[0]);
	od_route_unlock(route);
	return 0;
}

void
od_router_kill(od_router_t *router, od_id_t *id)
{
	void *argv[] = { id };
	od_router_foreach(router, od_router_kill_cb, argv);
}
