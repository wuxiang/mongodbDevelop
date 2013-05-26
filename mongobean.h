#include "mongo/client/dbclient.h"


namespace MongoBean{
  typedef boost::function<void(const std::string &ns, mongo::BSONObj obj, int flags)> InsertObjWrapperFn;
  typedef boost::function<void(const std::string &ns, const std::vector<mongo::BSONObj>& objs, int flags)> InsertObjsWrapperFn;
}

class MongoBeanDBClientConnection : public mongo::DBClientConnection{

  typedef mongo::DBClientConnection Derived;

public:
  
  MongoBeanDBClientConnection(bool _autoReconnect=false, mongo::DBClientReplicaSet *cp=0, double so_timeout=0):
    mongo::DBClientConnection(_autoReconnect, cp, so_timeout), _before_insert_obj(NULL), _after_insert_obj(NULL),
    _after_insert_objs(NULL), _before_insert_objs(NULL){}

  virtual ~MongoBeanDBClientConnection(){}

  virtual void insert( const std::string &ns, mongo::BSONObj obj , int flags=0);
  virtual void insert( const std::string &ns, const std::vector< mongo::BSONObj >& v , 
		       int flags=0);
  virtual void base_insert(const std::string &ns, mongo::BSONObj obj , int flags=0){
    // Pulled this out into its own method to more easily mock out and test the class
    Derived::insert(ns, obj, flags);
  }

  virtual void base_insert(const std::string &ns, const std::vector<mongo::BSONObj>& objs , int flags=0){
    Derived::insert(ns, objs, flags);
  }

  virtual void before_insert(boost::function<void(const std::string &ns, mongo::BSONObj obj, int flags)> f){
    _before_insert_obj = f; 
  }
  virtual void after_insert(boost::function<void(const std::string &ns, mongo::BSONObj obj, int flags)> f){
    _after_insert_obj = f; 
  }
  virtual void before_insert(boost::function<void(const std::string &ns, const std::vector< mongo::BSONObj >& objs, int flags)> f){
    _before_insert_objs = f; 
  }

  virtual void after_insert(boost::function<void(const std::string &ns, const std::vector< mongo::BSONObj >& objs, int flags)> f){
    _after_insert_objs = f; 
  }

private:
  MongoBean::InsertObjWrapperFn _before_insert_obj;
  MongoBean::InsertObjWrapperFn _after_insert_obj;
  MongoBean::InsertObjsWrapperFn _before_insert_objs;
  MongoBean::InsertObjsWrapperFn _after_insert_objs;
  template <typename WrapperFn, typename DocType>
  void _wrapper_insert(WrapperFn before, WrapperFn after, const std::string &ns,
		       DocType doc_or_docs, int flags);
};
