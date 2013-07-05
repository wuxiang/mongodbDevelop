#ifndef _GLOBAL_H_
#define _GLOBAL_H_
#include <mongo.h>
#include <bson.h>
#include <stdlib.h>
#include <stdint.h>
#include <strings.h>

void mongoConnect(mongo* conn)
{
	printf("=========================mongoConnect=====================\n");
	mongo_init(conn);
	//int status = mongo_client(conn, "127.0.0.1", 27017);
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
	printf("==============================bsonInsertUser========================\n");
	bson  bs[1];

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
		bson_append_new_oid(bs, "_id");
		bson_append_int(bs, "NO", random());
		bson_append_string(bs, "name", "tomwu");

		bson_append_start_array(bs, "information");
		{
			bson_append_start_object(bs, "0");
			bson_append_int(bs, "sort", 3);
			bson_append_int(bs, "group", 1);
			bson_append_string(bs, "data", "500701,500003,SZ8001,SH00001,870081,");
			bson_append_finish_object(bs);

			bson_append_start_object(bs, "1");
			bson_append_int(bs, "sort", 3);
			bson_append_int(bs, "group", 2);
			bson_append_string(bs, "data", "800709,800103,800101,690100,");
			bson_append_finish_object(bs);
		}
		bson_append_finish_array(bs);
	}
	bson_finish(bs);

	if (MONGO_OK != mongo_insert(conn, "test.information", bs, NULL))
	{
		printf("insert error\n");
	}
	else
	{
		printf("insert ok\n");
	}

	bson_destroy(bs);
}

void mongoQueryAll(mongo* conn)
{
	printf("=====================mongoQuerySimple========================\n");
	bson  query[1];
	mongo_cursor cursor[1];

	bson_init(query);
	bson_finish(query);

	mongo_cursor_init(cursor, conn, "SyncServerDB.AccUserCache");
	//mongo_cursor_set_query(cursor, query);
	mongo_cursor_set_query(cursor, NULL);
	mongo_cursor_set_limit(cursor, 1);

	int i = 0;
	while (mongo_cursor_next(cursor) == MONGO_OK)
	{
		printf("times: %d\n", ++i);
		bson_print(&(cursor->current));
	}

	mongo_cursor_destroy(cursor);
	bson_destroy(query);
}

void mongoQuerySimple(mongo* conn)
{
	printf("=====================mongoQuerySimple========================\n");
	bson  query[1];
	mongo_cursor cursor[1];

	bson_init(query);
	bson_append_string(query, "UName", "wuxiang");
	//bson_append_string(query, "UName", "wuxiangfe");
	bson_finish(query);

	mongo_cursor_init(cursor, conn, "SyncServerDB.AccUserCache");
	mongo_cursor_set_query(cursor, query);

	int i = 0;
	while (mongo_cursor_next(cursor) == MONGO_OK)
	{
		printf("time: +++++%d++++\n", ++i);
		bson_iterator iter[1];
		if (BSON_EOO != bson_find(iter, mongo_cursor_bson(cursor), "PWord"))
		{
			printf("name: %s, password: %s\n", "wuxiang", bson_iterator_string(iter));
		}
	}

	mongo_cursor_destroy(cursor);
	bson_destroy(query);
}

void mongoQueryComplex(mongo* conn)
{
	printf("======================mongoQueryComplex===================\n");
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
		/***************************1********************/
		//bson_append_int(query, "information.sort", 1);
		/*************************1 end*******************/

		/***********************method 2*********************/
		bson_append_start_array(query, "$and");
		{
			//bson_append_start_object(query, "0");
			//{
			//	bson_append_int(query, "information.sort", 3);
			//	bson_append_int(query, "information.group", 2);
			//}
			//bson_append_finish_object(query);
			/***************** or ****************/
			bson_append_start_object(query, "0");
			{
				bson_append_int(query, "information.sort", 3);
			}
			bson_append_finish_object(query);

			bson_append_start_object(query, "1");
			{
				bson_append_int(query, "information.group", 2);
			}
			bson_append_finish_object(query);
		}
		bson_append_finish_array(query);
		/***************************2 end*********************/
	}
	bson_finish(query);

	mongo_cursor cursor[1];
	mongo_cursor_init(cursor, conn, "test.information");
	mongo_cursor_set_query(cursor, query);

	while (mongo_cursor_next(cursor) == MONGO_OK)
	{
		printf("+++++++++++++++object+++++++++++++\n");
		bson_print(&(cursor->current));
	}

	mongo_cursor_destroy(cursor);
	bson_destroy(query);
}

void queryTestSetField(mongo* conn)
{
	bson query[1];
	bson_init(query);
	{
		bson_append_string(query, "name", "timxie");
	}
	bson_finish(query);

	bson  field[1];
	bson_init(field);
	{
		bson_append_int(field, "NO", 1);
		bson_append_int(field, "information", 1);
	}
	bson_finish(field);

	mongo_cursor cursor[1];
	mongo_cursor_init(cursor, conn, "test.information");
	mongo_cursor_set_query(cursor, query);
	mongo_cursor_set_fields(cursor, field);

	while (mongo_cursor_next(cursor) == MONGO_OK)
	{
		printf("+++++++++++++++object+++++++++++++\n");
		bson_print(&(cursor->current));
	}

	mongo_cursor_destroy(cursor);
	bson_destroy(field);
	bson_destroy(query);
}

void mongoUpdate(mongo* conn)
{
	printf("======================mongoUpdate====================\n");
	bson  query;
	bson_init(&query);
	{
		bson_append_int(&query, "information.sort", 8);
		bson_append_int(&query, "information.group", 8);
	}
	bson_finish(&query);

	bson  field;
	bson_init(&field);
	{
		bson_append_start_object(&field, "$set");
		{
			bson_append_string(&field, "information.$.data", "000001,200001,300001,");
		}
		bson_append_finish_object(&field);
	}
	bson_finish(&field);

	if (MONGO_OK == mongo_update(conn, "test.information", &query, &field, MONGO_UPDATE_UPSERT, NULL))
	{
		printf("update success!!!\n");
	}
}

void mongoDeleteArrayObj(mongo* conn)
{
	printf("======================mongoUpdate====================\n");
	bson  query;
	bson_init(&query);
	{
		bson_append_string(&query, "name", "tomwu");
		//bson_append_int(&query, "information.sort", 2);
		//bson_append_int(&query, "information.group", 1);
		//bson_append_int(&query, "NO", 846930886);
	}
	bson_finish(&query);

	bson  field;
	bson_init(&field);
	{
		bson_append_start_object(&field, "$pull");
		{
			//bson_append_int(&field, "information.sort", 3); // it will not work
			//bson_append_int(&field, "information.group", 1);// it will not work
			bson_append_start_object(&field, "information");
			{
				bson_append_int(&field, "sort", 3);
				bson_append_int(&field, "group", 2);
			}
			bson_append_finish_object(&field);
		}
		bson_append_finish_object(&field);
	}
	bson_finish(&field);

	if (MONGO_OK == mongo_update(conn, "test.information", &query, &field, MONGO_UPDATE_UPSERT|MONGO_UPDATE_MULTI, NULL))
	{
		printf("update success!!!\n");
	}
	else
	{
		printf("update failed\n");
	}
}

void appendAllTest(mongo* conn)
{
	bson query;
	bson_init(&query);
	{
		bson_append_string(&query, "name", "tomwu");
	}
	bson_finish(&query);

	bson  b;
    bson_init(&b);
	{
		bson_append_start_object(&b, "$pushAll");
		{
			bson_append_start_array( &b, "information" );
			{
				bson_append_start_object( &b, "0" );
				bson_append_string( &b, "name", "John Coltrane: Impressions" );
				bson_append_int( &b, "price", 1099 );
				bson_append_finish_object( &b );

				bson_append_start_object( &b, "1" );
				bson_append_string( &b, "name", "Larry Young: Unity" );
				bson_append_int( &b, "price", 1199 );
				bson_append_finish_object( &b );
			}
			bson_append_finish_array( &b );
		}
		bson_append_finish_object( &b );
	}
	bson_finish(&b);

	if (MONGO_OK == mongo_update(conn, "test.information", &query, &b, MONGO_UPDATE_UPSERT|MONGO_UPDATE_MULTI, NULL))
	{
		printf("update success!!!\n");
	}
	else
	{
		printf("update failed\n");
	}
}

void  test4Project()
{
	mongo  conn[1];
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
		return;
	}

	printf("connect ok\n");

	bson  query;
	bson_init(&query);
	{
		bson_append_string(&query, "userid", "wuxiangfe");
	}
	bson_finish(&query);

	bson field;
	bson_init(&field);
	{
		bson_append_int(&field, "time", 1);
		bson_append_int(&field, "content", 1);
	}
	bson_finish(&field);

	mongo_cursor cursor;
	mongo_cursor_init(&cursor, conn, "Cloud.SyncData");
	mongo_cursor_set_query(&cursor, &query);
	mongo_cursor_set_fields(&cursor, &field);

	//bool flag = false;
	char stk[64];
	time_t tm = 0;
	while (MONGO_OK == mongo_cursor_next(&cursor))
	{
		//bson_print(mongo_cursor_bson(&cursor));
		bson_iterator it[1];

		if (BSON_EOO != bson_find(it, mongo_cursor_bson(&cursor), "time"))
		{
			fprintf(stderr, "time: %ld\n", bson_iterator_time_t(it));
		}

		if (BSON_EOO != bson_find(it, mongo_cursor_bson(&cursor), "content"))
		{
			bson  subobj[1];
			bson_iterator_subobject(it, subobj);

			bson_iterator  subIter[1];
			if (BSON_EOO != bson_find(subIter, subobj, "group0"))
			{
				fprintf(stderr, "find object!!!\n");
				bson_iterator  subsubIter[1];
				bson_iterator_subiterator(subIter, subsubIter);
				while (BSON_EOO != bson_iterator_next(subsubIter))
				{
					bson  desObj[1];
					bson_iterator_subobject(subsubIter, desObj);

					bson_iterator  stkIte[1];
					if (BSON_EOO != bson_find(stkIte, desObj, "data"))
					{
						bzero(stk, 64);
						strcpy(stk, bson_iterator_string(stkIte));
						tm = 0;
						if (BSON_EOO != bson_find(stkIte, desObj, "time"))
						{
							tm = bson_iterator_time_t(stkIte);
						}
					}
					fprintf(stderr, "data: %s,\ttime: %ld\n", stk, tm);
				}
			}
		}

	}

	mongo_cursor_destroy(&cursor);
	bson_destroy(&field);
	bson_destroy(&query);
}

void multiFieldUpdate()
{
	mongo  conn[1];
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
		return;
	}

	printf("connect ok\n");

	bson  query;
	bson_init(&query);
	{
		bson_append_string(&query, "UName", "elisonwu");
		bson_append_int(&query, "sort", 3);
		bson_append_int(&query, "group", 1);
	}
	bson_finish(&query);

	bson field;
	bson_init(&field);
	{
		bson_append_start_object(&field, "$pushAll");
		{
			bson_append_start_array(&field, "content");
			{
				bson_append_start_object(&field, "1");
				{
					bson_append_string(&field, "data", "SH88888888");
					bson_append_time_t(&field, "time", 888888);
				}
				bson_append_finish_object(&field);

				bson_append_start_object(&field, "2");
				{
					bson_append_string(&field, "data", "SZ99999999");
					bson_append_time_t(&field, "time", 99999999);
				}
				bson_append_finish_object(&field);
			}
			bson_append_finish_array(&field);
		}
		bson_append_finish_object(&field);

		bson_append_start_object(&field, "$set");
		{
			bson_append_time_t(&field, "time", 888888);
			bson_append_string(&field, "macIP", "192.168.3.154");
			bson_append_string(&field, "PushID", "apple");
			bson_append_int(&field, "platform",  8);
		}
		bson_append_finish_object(&field);
	}
	bson_finish(&field);

	if (MONGO_OK != mongo_update(conn, "SyncServerDB.StockData", &query, &field, MONGO_UPDATE_UPSERT | MONGO_UPDATE_MULTI, NULL))
	{
		fprintf(stderr, "mongodb update failed\n");
	}

	fprintf(stderr, "mongodb update success\n");
}

#endif //_GLOBAL_H_
