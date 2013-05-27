#ifndef _GLOBAL_H_
#define _GLOBAL_H_
#include <mongo.h>
#include <bson.h>
#include <stdlib.h>

void mongoConnect(mongo* conn)
{
	int status = mongo_connect(conn, "127.0.0.1", 27017);
	if (status != MONGO_OK)
	{
		switch (conn->err)
		{
			case MONGO_CONN_NO_SOCKET:
				printf("no socket\n");
				return;
			case MONGO_CONN_FAIL:
				printf("connected failed\n");
				return;
			case MONGO_CONN_NOT_MASTER:
				printf("not master\n");
		}
	}

	printf("connect ok\n");
}

void bsonInsertUser(mongo* conn)
{
	bson*  bs = bson_create();

	/* user information
	 *
	 * {
	 * ID: oid,
	 * UName: string
	 * pWord: string
	 * SyncTime: time_t
	 * }
	bson_init(bs);
	bson_append_new_oid(bs, "ID");
	bson_append_string(bs, "UName", "elison");
	bson_append_string(bs, "pWord", "elison88");
	bson_append_long(bs, "SyncTime", 1369019003);
	bson_finish(bs);

	mongo_write_concern  cern[1];
	mongo_write_concern_init(cern);
	cern->w = 1;
	mongo_write_concern_finish(cern);

	if (MONGO_OK != mongo_insert(conn, "SyncServerDB.AccUserCache", bs, cern))
	{
		printf("insert error\n");
	}
	*/
	/* tmp information
	 * {
	 * NO : int,
	 * name : string
	 * information :
	 * [
	 *	 {
	 *		sort : int,
	 *		group: int,
	 *		data : string
	 *	 },
	 *	 {
	 *		sort : int,
	 *		group: int,
	 *		data : string
	 *	 },
	 *	 {
	 *		sort : int,
	 *		group: int,
	 *		data : string
	 *	 }
	 * ]
	 *
	 *
	 * }
	 */
	bson_init(bs);
	{
		bson_append_int(bs, "NO", random());
		bson_append_string(bs, "name", "timxie");

		bson_append_start_array(bs, "information");
		{
			bson_append_start_object(bs, "0");
			bson_append_int(bs, "sort", 1);
			bson_append_int(bs, "group", 1);
			bson_append_string(bs, "data", "300701,300003,SZ8001");
			bson_append_finish_object(bs);

			bson_append_start_object(bs, "1");
			bson_append_int(bs, "sort", 1);
			bson_append_int(bs, "group", 2);
			bson_append_string(bs, "data", "300709,300103,800101");
			bson_append_finish_object(bs);
		}
		bson_append_finish_array(bs);
	}
	bson_finish(bs);

	if (MONGO_OK != mongo_insert(conn, "test.information", bs, NULL))
	{
		printf("insert error\n");
	}

	printf("insert ok\n");
	bson_destroy(bs);
}

void mongoQuerySimple(mongo* conn)
{
	bson  query[1];
	mongo_cursor cursor[1];

	bson_init(query);
	bson_append_string(query, "UName", "wuxiang");
	bson_finish(query);

	mongo_cursor_init(cursor, conn, "SyncServerDB.AccUserCache");
	mongo_cursor_set_query(cursor, query);

	int i = 0;
	while (mongo_cursor_next(cursor) == MONGO_OK)
	{
		printf("time: +++++%d++++\n", ++i);
		bson_iterator iter[1];
		if (bson_find(iter, mongo_cursor_bson(cursor), "pWord"))
		{
			printf("name: %s, password: %s\n", "wuxiang", bson_iterator_string(iter));
		}
	}

	mongo_cursor_destroy(cursor);
	bson_destroy(query);
}

void mongoQueryComplex(mongo* conn)
{
	/*
	 * just sample 1
	bson query[1];
	bson_init(query);
	bson_append_string(query, "UName", "wuxiang");
	bson_append_string(query, "pWord", "1bc7b84877eecc98fbfaf4e7ca8d5cda");
	bson_finish(query);

	mongo_cursor cursor[1];
	mongo_cursor_init(cursor, conn, "SyncServerDB.AccUserCache");
	mongo_cursor_set_query(cursor, query);

	int i = 0;
	while (mongo_cursor_next(cursor) == MONGO_OK)
	{
		printf("time: +++++%d++++\n", ++i);
		bson_print(&(cursor->current));
	}

	bson_destroy(query);
	mongo_cursor_destroy(cursor);
	*/

	bson query[1];
	bson_init(query);
	{
		bson_append_start_array(query, "information");
		{
			bson_append_start_object(query, "0");
			bson_append_int(query, "sort", 1);
			bson_append_int(query, "group", 1);
			bson_append_finish_object(query);
		}
		bson_append_finish_array(query);
	}
	bson_finish(query);

	mongo_cursor cursor[1];
	mongo_cursor_init(cursor, conn, "test.information");
	mongo_cursor_set_query(cursor, query);

	while (mongo_cursor_next(cursor) == MONGO_OK)
	{
		bson_print(&(cursor->current));
	}

	mongo_cursor_destroy(cursor);
	bson_destroy(query);
}

void mongoUpdate(mongo* conn)
{
}

#endif //_GLOBAL_H_
