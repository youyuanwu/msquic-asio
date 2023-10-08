#define BOOST_TEST_MODULE msquic_test
#include <boost/test/unit_test.hpp>

#include <boost/asio.hpp>

#include "boost/msquic/basic_listener_handle.hpp"

#include "boost/msquic/basic_config_handle.hpp"
#include "boost/msquic/basic_quic_handle.hpp"
#include "boost/msquic/basic_registration_handle.hpp"
#include "boost/msquic/msquic_api.hpp"

#include "boost/msquic/event.hpp"
#include "test_helper.hpp"

#include <future>
#include <latch>

namespace net = boost::asio;
namespace quic = boost::msquic;

BOOST_AUTO_TEST_SUITE(test_quic)

BOOST_AUTO_TEST_CASE(Basic) {

  net::io_context ioc;
  quic::event ev(ioc.get_executor());

  int i = {};

  ev.async_wait([&i](const boost::system::error_code &ec) {
    BOOST_REQUIRE(!ec);
    i++;
  });

  auto a = std::async(std::launch::async, [&ev]() { ev.set(); });

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
  ev.async_wait([&i, starttime](const boost::system::error_code &ec) {
    BOOST_REQUIRE(!ec);
    i++;
    // check if event is triggered after 1 sec.
    auto endtime = std::chrono::steady_clock::now();
    std::chrono::milliseconds duration =
        std::chrono::duration_cast<std::chrono::milliseconds>(endtime -
                                                              starttime);
    BOOST_REQUIRE_GE(duration.count(), 1000);
  });

  auto b = std::async(std::launch::async, [&ev]() {
    // wait 1 sec to trigger event
    std::this_thread::sleep_for(std::chrono::seconds(1));
    ev.set();
  });

  ioc.run();
  b.wait();
  BOOST_REQUIRE_EQUAL(11, i);
}

// runs the client synchronously
void run_client_helper(
    quic::api &api,
    quic::basic_registration_handle<net::io_context::executor_type> &reg) {

  boost::system::error_code ec = {};

  net::io_context cioc;
  // reuse server registration.

  quic::basic_config_handle client_config(cioc.get_executor(), api.get());
  config_test_client_handle(client_config, reg.native_handle());

  quic::basic_connection_handle<net::io_context::executor_type> conn(
      cioc.get_executor(), api.get());
  // open connection
  conn.open(reg.native_handle(), ec);
  BOOST_REQUIRE(!ec.failed());

  auto clientf = [&conn]() -> net::awaitable<void> {
    try {
      auto executor = co_await net::this_coro::executor;
      boost::system::error_code cec = {};
      BOOST_TEST_MESSAGE("client async connect");
      co_await conn.async_connect(net::use_awaitable);

      // open a stream
      quic::basic_stream_handle<net::io_context::executor_type> stream(
          conn.get_executor(), conn.get_api());
      stream.open(conn.native_handle(), QUIC_STREAM_OPEN_FLAG_NONE, cec);
      BOOST_REQUIRE(!cec.failed());
      BOOST_TEST_MESSAGE("client stream async start");
      co_await stream.async_start(QUIC_STREAM_START_FLAG_NONE,
                                  net::use_awaitable);

      // send something
      std::string data = "Hello";
      QUIC_BUFFER b = {};
      b.Length = static_cast<std::uint32_t>(data.length());
      b.Buffer = (uint8_t *)data.data();
      BOOST_TEST_MESSAGE("client async_send");
      std::size_t wlen = co_await stream.async_send(
          &b, 1, QUIC_SEND_FLAG_FIN, nullptr, net::use_awaitable);
      BOOST_REQUIRE(wlen == b.Length);

      // recieve from server.
      BOOST_TEST_MESSAGE("client async_receive");
      std::string buff(123, 'a');
      std::size_t rlen = co_await stream.async_receive(buff.data(), buff.size(),
                                                       net::use_awaitable);
      std::string expected_resp = "mydata";
      BOOST_REQUIRE_EQUAL(rlen, expected_resp.size());
      BOOST_REQUIRE_EQUAL(expected_resp, buff.substr(0, 6));

      BOOST_TEST_MESSAGE("client async_wait_peer_shutdown");
      co_await stream.async_wait_peer_shutdown(net::use_awaitable);

      // we wait twice, the event should go though successfully immediately.
      BOOST_TEST_MESSAGE("client async_wait_peer_shutdown2");
      co_await stream.async_wait_peer_shutdown(net::use_awaitable);

      // after peer shutdown, try read. It should success and complete with 0
      // len
      std::size_t rlen2 = co_await stream.async_receive(
          buff.data(), buff.size(), net::use_awaitable);
      BOOST_REQUIRE_EQUAL(rlen2, 0);

      BOOST_TEST_MESSAGE("client async_wait_shutdown_complete");
      co_await stream.async_wait_shutdown_complete(net::use_awaitable);
      BOOST_TEST_MESSAGE("client stream end");
      stream.close();

      // wait connection shutdown
      BOOST_TEST_MESSAGE("conn async_wait_transport_shutdown");
      co_await conn.async_wait_transport_shutdown(net::use_awaitable);
      BOOST_TEST_MESSAGE("conn async_wait_shutdown_complete");
      co_await conn.async_wait_shutdown_complete(net::use_awaitable);
      // done close connection.
      conn.close();
    } catch (const std::exception &e) {
      BOOST_REQUIRE_MESSAGE(false, std::string("client coro has exception: ") +
                                       e.what());
    }
  };
  // post client task
  net::co_spawn(cioc, clientf, net::detached);
  // start and set callback
  conn.start(client_config.native_handle(), QUIC_ADDRESS_FAMILY_UNSPEC,
             "localhost" /*servername*/, 4567, ec);
  BOOST_REQUIRE(!ec.failed());
  cioc.run();
  BOOST_TEST_MESSAGE("Client end success.");
}

// --log_level=all --detect_memory_leak=0 --run_test=test_quic/Handle
BOOST_AUTO_TEST_CASE(Handle) {
  net::io_context ioc;
  quic::api api;

  quic::basic_registration_handle reg(ioc.get_executor(), api.get());
  set_registration_handle(reg);

  boost::system::error_code ec = {};

  quic::basic_config_handle config(ioc.get_executor(), api.get());
  config_test_server_handle(config, reg.native_handle());

  quic::basic_listener_handle listener(ioc.get_executor(), api.get(), config);
  listener.open(reg.native_handle(), ec);
  BOOST_REQUIRE(!ec.failed());

  // use coroutine
  auto f = [&listener]() -> net::awaitable<void> {
    try {
      auto executor = co_await net::this_coro::executor;
      for (;;) {
        quic::basic_connection_handle<net::io_context::executor_type> conn =
            co_await listener.async_accept(net::use_awaitable);

        auto fconn =
            [](quic::basic_connection_handle<net::io_context::executor_type> c)
            -> net::awaitable<void> {
          try {
            BOOST_TEST_MESSAGE("async_connect");
            co_await c.async_connect(net::use_awaitable);
            // resume
            boost::system::error_code ec = {};
            c.send_resumption_ticket(ec);
            BOOST_REQUIRE(!ec.failed());
            for (;;) {
              BOOST_TEST_MESSAGE("async_accept stream");
              quic::basic_stream_handle<net::io_context::executor_type> stream =
                  co_await c.async_accept(net::use_awaitable);

              // TODO: wait for receive
              BOOST_TEST_MESSAGE("async_receive stream");
              std::string buff(123, 'a');
              std::size_t rlen = co_await stream.async_receive(
                  buff.data(), buff.size(), net::use_awaitable);

              // peer finishes request and close its direction
              // wait for stream shutdown
              BOOST_TEST_MESSAGE("async_stream_peer_shutdown");
              co_await stream.async_wait_peer_shutdown(net::use_awaitable);

              BOOST_REQUIRE_GE(rlen, 5); // example client always send 100
              BOOST_CHECK_EQUAL(buff.substr(0, 5), "Hello");

              std::string data = "mydata";
              QUIC_BUFFER b = {};
              b.Length = static_cast<std::uint32_t>(data.length());
              b.Buffer = (uint8_t *)data.data();
              BOOST_TEST_MESSAGE("async_send");
              std::size_t wlen = co_await stream.async_send(
                  &b, 1, QUIC_SEND_FLAG_FIN, nullptr, net::use_awaitable);
              BOOST_REQUIRE(wlen == b.Length);

              BOOST_TEST_MESSAGE("stream async_await_shutdown_complete");
              co_await stream.async_wait_shutdown_complete(net::use_awaitable);

              // std::this_thread::sleep_for(std::chrono::milliseconds(10));
              //  peer does not send shutdown for conn.
              //  wait for transport shutdown.
              BOOST_TEST_MESSAGE("conn async_wait_transport_shutdown");
              co_await c.async_wait_transport_shutdown(net::use_awaitable);
              BOOST_TEST_MESSAGE("conn async_wait_shutdown_complete");
              co_await c.async_wait_shutdown_complete(net::use_awaitable);
              break; // for now we only accept 1 stream
            }
          } catch (boost::system::system_error const &e) {
            // if shutdown, ignore
            BOOST_TEST_MESSAGE(std::string("stream exception: ") + e.what());
            BOOST_REQUIRE_EQUAL(e.code(), net::error::basic_errors::shut_down);
            c.close();
          } catch (const std::exception &e) {

            BOOST_REQUIRE_MESSAGE(
                false,
                std::string("connection coro has exception: ") + e.what());
          }
        };

        BOOST_TEST_MESSAGE("async_accept connection");
        net::co_spawn(executor, fconn(std::move(conn)), net::detached);
      }
    } catch (const std::exception &e) {
      BOOST_REQUIRE_MESSAGE(false, std::string("listen coro has exception: ") +
                                       e.what());
    }
  };

  net::co_spawn(ioc, f, net::detached);

  QUIC_ADDR addr = {};
  QuicAddrSetFamily(&addr, QUIC_ADDRESS_FAMILY_UNSPEC);
  QuicAddrSetPort(&addr, 4567);

  listener.start(get_test_alpn(), addr, ec);
  BOOST_REQUIRE(!ec.failed());

  // run server in a separate thread
  std::latch lch(1);
  std::jthread j([&ioc, &lch]() {
    lch.count_down();
    ioc.run();
  });
  // wait for server to start up:
  lch.wait();
  // run server for 100 sec;
  // std::this_thread::sleep_for(std::chrono::seconds(5));

  // launch client operation:
  run_client_helper(api, reg);

  // let server clean up. if we stop server right away maybe async operations
  // have not finished and we have msquic worker waiting on semaphore and the
  // lib cannot unload
  std::this_thread::sleep_for(std::chrono::seconds(1));
  // stop server.
  ioc.stop();
}

// --log_level=all --detect_memory_leak=0 --run_test=test_quic/Client
BOOST_AUTO_TEST_CASE(Client) {
  net::io_context ioc;
  quic::api api;
  quic::basic_registration_handle reg(ioc.get_executor(), api.get());
  set_registration_handle(reg);
  // run_client_helper(api, reg);
}

BOOST_AUTO_TEST_SUITE_END()