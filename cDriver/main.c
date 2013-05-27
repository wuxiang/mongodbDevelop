#include <stdio.h>
#include <mongo.h>
#include "global.h"

int main()
{
	mongo*  conn = mongo_create();
	mongoConnect(conn);
	mongo_destroy(conn);
	return 0;
}
