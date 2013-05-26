#include "../include/mongobean.h"
#define BOOST_TEST_DYN_LINK  
#define BOOST_TEST_MAIN
#include <boost/test/unit_test.hpp>


struct t_functor{
  std::string m_ns;
  mongo::BSONObj m_obj;
  int m_flags;
  bool called;
t_functor(): called(false){}
void operator()(const std::string &ns, mongo::BSONObj obj, int flags){
m_ns=ns;
 m_obj=obj;
 m_flags=flags;
 called=true;
}
};

struct t_v_functor{
  std::string m_ns;
  std::vector<mongo::BSONObj> m_objs;
  int m_flags;
  bool called;
t_v_functor(): called(false){}
void operator()(const std::string &ns, const std::vector<mongo::BSONObj>& objs, int flags){
  m_ns=ns;
  m_objs=objs;
  m_flags=flags;
  called=true;
}
};

mongo::BSONObj get_test_obj1(){
  mongo::BSONObjBuilder b;
  b.append("name", "Joe");
  b.append("age", 33);
  return b.obj();  
}

class MockMongoBeanDBClientConnection : public MongoBeanDBClientConnection{
 public:
 MockMongoBeanDBClientConnection(): insert_called(false), v_insert_called(false){}
  bool insert_called;
  bool v_insert_called;
  void base_insert(const std::string &ns, mongo::BSONObj obj, int flags=0){
    insert_called = true;
  }
  void base_insert(const std::string &ns, const std::vector<mongo::BSONObj>& objs, 
		   int flags=0){
    v_insert_called = true;
  }
};


BOOST_AUTO_TEST_CASE( mongobean_should_set_wrappers )
{  
  t_functor before_fn;
  t_functor after_fn;
  MongoBean::InsertObjWrapperFn test_before_functor(boost::ref(before_fn));
  MongoBean::InsertObjWrapperFn test_after_functor(boost::ref(after_fn));
  mongo::BSONObj test_obj = get_test_obj1();
  MockMongoBeanDBClientConnection c;
  c.connect("localhost");
  c.before_insert(test_before_functor);
  c.after_insert(test_after_functor);
  c.insert("test.tests", test_obj);
  BOOST_CHECK_EQUAL( before_fn.m_ns, "test.tests" );
  BOOST_CHECK_EQUAL( before_fn.m_obj, test_obj);
  BOOST_CHECK_EQUAL( before_fn.m_flags, 0);
  BOOST_CHECK_EQUAL( before_fn.called, true);
  BOOST_CHECK_EQUAL( after_fn.m_ns, "test.tests" );
  BOOST_CHECK_EQUAL( after_fn.m_obj, test_obj);
  BOOST_CHECK_EQUAL( after_fn.m_flags, 0);
  BOOST_CHECK_EQUAL( after_fn.called, true);
  BOOST_CHECK_EQUAL( c.insert_called, true);  
}


BOOST_AUTO_TEST_CASE( mongobean_should_set_wrappers_insert_multiple_objs)
{  
  t_v_functor before_fn;
  t_v_functor after_fn;
  MongoBean::InsertObjsWrapperFn test_before_functor(boost::ref(before_fn));
  MongoBean::InsertObjsWrapperFn test_after_functor(boost::ref(after_fn));
  mongo::BSONObj test_obj = get_test_obj1();
  std::vector<mongo::BSONObj> bv = std::vector<mongo::BSONObj>();
  bv.push_back(test_obj);
  bv.push_back(test_obj);
  MockMongoBeanDBClientConnection c;
  c.connect("localhost");
  c.before_insert(test_before_functor);
  c.after_insert(test_after_functor);
  c.insert("test.tests", bv);
  BOOST_CHECK_EQUAL( before_fn.m_ns, "test.tests" );
  BOOST_CHECK( before_fn.m_objs == bv);
  BOOST_CHECK_EQUAL( before_fn.m_flags, 0);
  BOOST_CHECK_EQUAL( before_fn.called, true);
  BOOST_CHECK_EQUAL( after_fn.m_ns, "test.tests" );
  BOOST_CHECK( after_fn.m_objs == bv);
  BOOST_CHECK_EQUAL( after_fn.m_flags, 0);
  BOOST_CHECK_EQUAL( after_fn.called, true);
  BOOST_CHECK_EQUAL( c.v_insert_called, true);  
}
