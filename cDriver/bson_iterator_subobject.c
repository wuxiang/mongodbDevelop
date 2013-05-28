//#include "test.h"
#include <bson.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

int main(){
    //bson_buffer bb;
    bson b, sub;
    bson_iterator it;

    /* Create a rich document like this one:

 { _id: ObjectId("4d95ea712b752328eb2fc2cc"),
   user_id: ObjectId("4d95ea712b752328eb2fc2cd"),

   items: [
     { sku: "col-123",
       name: "John Coltrane: Impressions",
       price: 1099,
     },

     { sku: "young-456",
       name: "Larry Young: Unity",
       price: 1199
     }
   ],

   address: {
     street: "59 18th St.",
     zip: 10010
   },

   total: 2298
 }
     */
    //bson_buffer_init( &bb );
    bson_init(&b);
    bson_append_new_oid( &b, "_id" );
    bson_append_new_oid( &b, "user_id" );

    bson_append_start_array( &b, "items" );
        bson_append_start_object( &b, "0" );
            bson_append_string( &b, "name", "John Coltrane: Impressions" );
            bson_append_int( &b, "price", 1099 );
        bson_append_finish_object( &b );

        bson_append_start_object( &b, "1" );
            bson_append_string( &b, "name", "Larry Young: Unity" );
            bson_append_int( &b, "price", 1199 );
        bson_append_finish_object( &b );
    bson_append_finish_object( &b );

    bson_append_start_object( &b, "address" );
        bson_append_string( &b, "street", "59 18th St." );
        bson_append_int( &b, "zip", 10010 );
    bson_append_finish_object( &b );

    bson_append_int( &b, "total", 2298 );

    /* Convert from a buffer to a raw BSON object that
 can be sent to the server:
     */
    //bson_from_buffer( &b, &bb );

    /* Advance to the 'items' array */
    bson_find( &it, &b, "items" );

    /* Get the subobject representing items */
    //bson_iterator_subobject( &it, &sub );
	bson_iterator  subite;
	bson_iterator_subiterator(&it, &subite);
	while(bson_iterator_next(&subite) != BSON_EOO)
	{
		printf("+++++++++++start++++++++++++\n");
		bson  subobj;
		bson_iterator_subobject(&subite, &subobj);
		//bson_print(&subobj);
		bson_iterator subobjite;
		if (bson_find(&subobjite, &subobj, "name"))
		{
			printf("name: %s\n", bson_iterator_string(&subobjite));
		}
		printf("+++++++++++end++++++++++++++\n");
	}

    /* Now iterate that object */
    //bson_print( &sub );

   return 0;
}
