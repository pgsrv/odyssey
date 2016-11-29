#ifndef OD_SERVER_H_
#define OD_SERVER_H_

/*
 * odissey.
 *
 * PostgreSQL connection pooler and request router.
*/

typedef struct odserver_t odserver_t;

typedef enum {
	OD_SUNDEF,
	OD_SIDLE,
	OD_SEXPIRE,
	OD_SCONNECT,
	OD_SRESET,
	OD_SACTIVE
} odserver_state_t;

struct odserver_t {
	odserver_state_t  state;
	so_stream_t       stream;
	mm_io_t           io;
	int               is_ready;
	int               is_transaction;
	int               idle_time;
	so_key_t          key;
	so_key_t          key_client;
	void             *route;
	void             *pooler;
	od_list_t         link;
};

static inline void
od_serverinit(odserver_t *s)
{
	s->state          = OD_SUNDEF;
	s->route          = NULL;
	s->io             = NULL;
	s->pooler         = NULL;
	s->idle_time      = 0;
	s->is_ready       = 0;
	s->is_transaction = 0;
	so_keyinit(&s->key);
	so_keyinit(&s->key_client);
	so_stream_init(&s->stream);
	od_listinit(&s->link);
}

static inline odserver_t*
od_serveralloc(void)
{
	odserver_t *s = malloc(sizeof(*s));
	if (s == NULL)
		return NULL;
	od_serverinit(s);
	return s;
}

static inline void
od_serverfree(odserver_t *s)
{
	so_stream_free(&s->stream);
	free(s);
}

#endif
