#include <stdio.h>
#include <mongo.h>
#include "global.h"

int main()
{
	mongo  conn[1];

	mongoConnect(conn);
	//bsonInsertUser(conn);
	//mongoQuerySimple(conn);
	//mongoQueryComplex(conn);
	//queryTestSetField(conn);
	//mongoQueryAll(conn);
	//mongoDeleteArrayObj(conn);
	//appendAllTest(conn);
	//mongoUpdate(conn);

	//mongo_destroy(conn);
	//test4Project();
	multiFieldUpdate();
	return 0;
}
