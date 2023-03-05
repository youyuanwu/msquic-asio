#define BOOST_TEST_MODULE msquic_test
#include <boost/test/unit_test.hpp>

#include "boost/msquic/event.hpp"
#include <future>

namespace net = boost::asio;
namespace quic = boost::msquic;


BOOST_AUTO_TEST_SUITE(test_winhttp_infra)

BOOST_AUTO_TEST_CASE(Baisc) {

  net::io_context ioc;
  quic::event ev(ioc.get_executor());

  int i = {};

  ev.async_wait([&i](const boost::system::error_code &ec){
    BOOST_REQUIRE(!ec);
    i++;
  });

  auto a = std::async(std::launch::async,[&ev](){
    ev.set();
  });

  // not yet invoked
  BOOST_REQUIRE_EQUAL(0, i);

  ioc.run();

  a.wait();

  BOOST_REQUIRE_EQUAL(1, i);

  // reset and do it again.
  i = 10;
  ev.reset();
  ioc.reset();

  auto starttime = std::chrono::steady_clock::now();
  ev.async_wait([&i, starttime](const boost::system::error_code &ec){
    BOOST_REQUIRE(!ec);
    i++;
    // check if event is triggered after 1 sec.
    auto endtime = std::chrono::steady_clock::now();
    std::chrono::milliseconds duration = std::chrono::duration_cast<std::chrono::milliseconds>(endtime - starttime);
    BOOST_REQUIRE_GE(duration.count(), 1000);
  });

  auto b = std::async(std::launch::async,[&ev](){
    // wait 1 sec to trigger event
    std::this_thread::sleep_for(std::chrono::seconds(1));
    ev.set();
  });

  ioc.run();
  b.wait();
  BOOST_REQUIRE_EQUAL(11, i);
}

BOOST_AUTO_TEST_SUITE_END()