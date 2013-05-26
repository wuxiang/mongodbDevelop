#include "../include/mongobean.h"
#include <cstdlib>
#include <iostream>


void MongoBeanDBClientConnection::insert( const std::string &ns, mongo::BSONObj obj , int flags){
  _wrapper_insert<MongoBean::InsertObjWrapperFn, mongo::BSONObj>(_before_insert_obj, 
								 _after_insert_obj,
								 ns, obj, flags);
}

void MongoBeanDBClientConnection::insert( const std::string &ns, 
					const std::vector< mongo::BSONObj >& v , int flags){
  _wrapper_insert<MongoBean::InsertObjsWrapperFn, 
		  const std::vector<mongo::BSONObj>& >(_before_insert_objs, 
						       _after_insert_objs,
						       ns, v, flags);
}

template <typename WrapperFn, typename DocType>
void MongoBeanDBClientConnection::_wrapper_insert(WrapperFn before, 
						  WrapperFn after, 
						  const std::string &ns,
						  DocType doc_or_docs, int flags){
  //
  if(before){
    before(ns, doc_or_docs, flags);
  }
  base_insert(ns, doc_or_docs, flags);
  if(after){
    after(ns, doc_or_docs, flags);
  }
}

