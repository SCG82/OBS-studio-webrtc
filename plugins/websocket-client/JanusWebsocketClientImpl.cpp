#include "JanusWebsocketClientImpl.h"
#include "json.hpp"
using json = nlohmann::json;
typedef websocketpp::config::asio_client::message_type::ptr message_ptr;

JanusWebsocketClientImpl::
JanusWebsocketClientImpl()
{
  // Set logging to be pretty verbose (everything except message payloads)
  client.set_access_channels(   websocketpp::log::alevel::all           );
  client.clear_access_channels( websocketpp::log::alevel::frame_payload );
  client.set_error_channels(    websocketpp::log::elevel::all           );
  // Initialize ASIO
  client.init_asio();
}

JanusWebsocketClientImpl::
~JanusWebsocketClientImpl()
{
  //Disconnect just in case
  disconnect(false);
}

bool
JanusWebsocketClientImpl::
connect(
  const std::string & url,
  const std::string & room,
  const std::string & username,
  const std::string & token,
  Listener* listener)
{
  websocketpp::lib::error_code ec;

  //reset loggin flag
  logged = false;
  try
  {
    // --- Register our message handler
    client.set_message_handler( [=](websocketpp::connection_hdl /* unused */, message_ptr frame) {

      // get response
      auto msg = json::parse(frame->get_payload());
      // const char* x = frame->get_payload().c_str();
      // std::cout << x << std::endl << std::endl << std::endl ;

      if (msg.find("janus") == msg.end())
        return;
      if (msg.find("ack")   != msg.end())
        return;
      // std::string response = msg["janus"];
      if (msg.find("jsep")  != msg.end()) {
        std::string sdp = msg["jsep"]["sdp"];
        listener->onOpened(sdp);
        return;
      }

      // Check type
      std::string id = msg["janus"];
      if (id.compare("success") == 0) {
        if (msg.find("transaction") == msg.end())
          return;
        if (msg.find("data")        == msg.end())
          return;

        // Get the Data session
        auto data = msg["data"];

        // Server is sending response twice, ingore second one
        if (!logged) {
          //Get response code
          session_id = data["id"];
          //Launch logged event
          //create handle command
          json attachPlugin = {
            {"janus",       "attach"                },
            {"transaction", std::to_string(rand())  },
            {"session_id",  session_id              },
            {"plugin",      "janus.plugin.videoroom"},
          };

          connection->send(attachPlugin.dump());
          logged = true;

          // Keep the connection alive
          is_running.store(true);
          thread_keepAlive = std::thread([&]() {
            JanusWebsocketClientImpl::keepConnectionAlive();
          });

        } else { // logged

          handle_id = data["id"];

          long long janusroom = std::stoll(room);
          json joinRoom = {
            {"janus",       "message"              },
            {"transaction", std::to_string(rand()) },
            {"session_id",  session_id             },
            {"handle_id",   handle_id              },
            { "body" ,
              {
                {"room",    janusroom   },
                {"display", "OBS"       },
                {"ptype" ,  "publisher" },
                {"request", "join"      }
              }
            }
          };

          connection->send(joinRoom.dump());
          listener->onLogged(session_id);

        } // logged

      } // if "success"

    }); // --- handler


    // --- open Handler
    client.set_open_handler([=](websocketpp::connection_hdl /* unused */){

      // Launch event
      listener->onConnected();

      // Login command
      json login = {
        {"janus",       "create"               },
        {"transaction", std::to_string(rand()) },
        { "payload",
          {
            { "username", username },
            { "token",    token    },
            { "room",     room     }
          }
        }
      };

      // Serialize and send
      connection->send(login.dump());

    }); // Open Handler


    //Set close hanlder
    client.set_close_handler([=](...) {
      listener->onDisconnected();
    });

    //Set failure handler
    client.set_fail_handler([=](...) {
      //Call listener
      listener->onDisconnected();
    });

    // --- Register our tls hanlder
    client.set_tls_init_handler([&](websocketpp::connection_hdl /* unused */ ) {
      // Create context
      auto ctx = websocketpp::lib::make_shared<asio::ssl::context>(asio::ssl::context::tlsv12_client);

      try {
        ctx->set_options(
          asio::ssl::context::default_workarounds |
          asio::ssl::context::no_sslv2 |
          asio::ssl::context::no_sslv3 |
          asio::ssl::context::single_dh_use
        );
      }
      catch (std::exception& e) {
        std::cout << e.what() << std::endl;
      }
      return ctx;
    }); // Handler

    //Get connection
    connection = client.get_connection(url, ec);

    if (ec) {
      std::cout << "could not create connection because: " << ec.message() << std::endl;
      return 0;
    }
    connection->add_subprotocol("janus-protocol");

    // Note that connect here only requests a connection. No network messages are
    // exchanged until the event loop starts running in the next line.
    client.connect(connection);

    //Async
    thread = std::thread([&]() {
      // Start the ASIO io_service run loop
      // this will cause a single connection to be made to the server. c.run()
      // will exit when this connection is closed.
      client.run();
    });

  } // try

  catch (websocketpp::exception const & e) {
    std::cout << e.what() << std::endl;
    return false;
  }

  // OK
  return true;
}

bool
JanusWebsocketClientImpl::
open(
  const std::string& sdp,
  const std::string& /* unused codec   */,
  const std::string& /* unused milliId */
  )
{
  try
  {
    //Login command
    json open = {
      { "janus"       , "message" },
      { "session_id"  , session_id},
      { "handle_id"   , handle_id },
      { "transaction" , std::to_string(rand()) },
      { "body" ,
        {
          {"request", "configure" },
          {"muted",   false       },
          {"video",   true        },
          {"audio",   true        },
        }
      },
      { "jsep" ,
        {
          {"type",   "offer"},
          {"sdp",     sdp},
          {"trickle", true},
        }
      }
    };
    //Serialize and send
    if (connection->send(open.dump()))
      return false;
  }
  catch (websocketpp::exception const & e) {
    std::cout << e.what() << std::endl;
    return false;
  }
  //OK
  return true;
}

bool
JanusWebsocketClientImpl::
trickle(const std::string &mid, int index, const std::string &candidate, bool last)
{
  try
  {
    //Check if it is last
    if (!last) {
      //Login command
      json trickle = {
        { "janus"    , "trickle" },
        { "handle_id"  , handle_id },
        { "session_id" , session_id },
        { "transaction" , "trickle" + std::to_string(rand()) },
        { "candidate" ,
          {
            { "sdpMid"   , mid  },
            { "sdpMLineIndex" , index  } ,
            { "candidate"  , candidate }
          }
        }
      };
      //Serialize and send
      if (connection->send(trickle.dump()))
        return false;

      //OK
      return true;
    } else {
      json trickle = {
        { "janus"    , "trickle" },
        { "handle_id"  , handle_id },
        { "session_id" , session_id },
        { "transaction" , "trickle" + std::to_string(rand()) },
        { "candidate"  ,
          {
            { "completed", true },
          }
        }
      };
      if (connection->send(trickle.dump()))
        return false;
    }
  }
  catch (websocketpp::exception const & e) {
    std::cout << e.what() << std::endl;
    return false;
  }
  //OK
  return true;
}

void
JanusWebsocketClientImpl::
keepConnectionAlive()
{
  while (is_running.load()) {
    if (connection) {
      json keepaliveMsg = {
        { "janus",       "keepalive"                           },
        { "session_id",  session_id                            },
        { "transaction", "keepalive-" + std::to_string(rand()) },
      };
      try
      {
        connection->send(keepaliveMsg.dump());
      }
      catch (websocketpp::exception const & e)
      {
        std::cout << e.what() << std::endl;
      }
    }
    std::this_thread::sleep_for(std::chrono::seconds(2));
  }
};

bool
JanusWebsocketClientImpl::
disconnect(bool wait)
{
  try {
    // Stop keepAlive
    if (thread_keepAlive.joinable()){
      is_running.store(false);
      thread_keepAlive.join();
    }
    //Stop client
    client.close(connection, websocketpp::close::status::normal, std::string("disconnect"));
    client.stop();
    //Don't wait for connection close
    if (thread.joinable()) {
      //If sswe have to wait
      if (wait) {
        thread.join();
      } else {
        //Remov hanlders
        client.set_open_handler([](...){});
        client.set_close_handler([](...){});
        client.set_fail_handler([](...) {});
        //Detach trhead
        thread.detach();
      }
    }
  }

  catch (websocketpp::exception const & e) {
    std::cout << e.what() << std::endl;
    return false;
  }

  //OK
  return true;
}
