#ifndef _GLOBAL_H_
#define _GLOBAL_H_
#include <mongo.h>
#include <bson.h>

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

#endif //_GLOBAL_H_
