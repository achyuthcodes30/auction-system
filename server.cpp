#include "crow.h"
#include "crow/middlewares/cookie_parser.h"
#include "crow/middlewares/cors.h"
#include <crow/app.h>
#include <crow/common.h>
#include <jwt-cpp/jwt.h>
#include <unordered_set>

std::string domain = "bidblitz.com";
std::string jwtSecret = "kohligoat";

std::string generateRoomId() {
  static const std::string chars =
      "abcdefghijklmnopqrstuvwxyzABCDEFGHJKLMNPQRSTUVWXYZ23456789";
  static std::mt19937 rng(std::time(nullptr));
  std::uniform_int_distribution<std::size_t> distribution(0, chars.size() - 1);

  std::string roomId;
  roomId.reserve(8);

  for (int i = 0; i < 8; ++i) {
    roomId += chars[distribution(rng)];
  }

  return roomId;
}

class Auction {
public:
  enum class Status { TeamSelection, InProgress, Paused, Cancelled };
  Status status;
  Auction() : status(Status::TeamSelection) {}

private:
};

struct Player {
  std::string username;
  std::string team;
  std::string role;
};

struct ConnData {
  std::string roomId;
  Player player;
};

class Room {
public:
  std::unordered_map<crow::websocket::connection *, Player> players;
  std::unordered_set<crow::websocket::connection *> connections;
  Auction auction;
};

std::string generate_username() {

  static const std::vector<std::string> firstNames = {
      "Sachin",       "Virat",   "Rohit",   "Mahendra",  "Rahul",
      "Sourav",       "Jasprit", "Yuvraj",  "Ravindra",  "Hardik",
      "Shikhar",      "Kane",    "Steve",   "Joe",       "Ben",
      "James",        "Eoin",    "Jos",     "David",     "Mitchell",
      "Pat",          "Glenn",   "Kagiso",  "Quinton",   "Faf",
      "AB",           "Dale",    "Hashim",  "Babar",     "Shaheen",
      "Imran",        "Wasim",   "Javed",   "Shoaib",    "Inzamam",
      "Shane",        "Ricky",   "Brett",   "Michael",   "Justin",
      "Matthew",      "Adam",    "Chris",   "Brian",     "Viv",
      "Garfield",     "Curtly",  "Malcolm", "Clive",     "Kumar",
      "Angelo",       "Rangana", "Muttiah", "Sanath",    "Aravinda",
      "Tillakaratne", "Ross",    "Trent",   "Tim",       "Martin",
      "Brendon",      "Daniel",  "Shakib",  "Mushfiqur", "Tamim",
      "Mashrafe",     "Anil",    "Sunil",   "Kapil",     "Gautam",
      "Harbhajan",    "Zaheer"};

  static const std::vector<std::string> lastNames = {
      "Tendulkar",   "Kohli",      "Sharma",       "Dhoni",      "Dravid",
      "Ganguly",     "Bumrah",     "Singh",        "Jadeja",     "Pandya",
      "Dhawan",      "Williamson", "Smith",        "Root",       "Stokes",
      "Anderson",    "Morgan",     "Buttler",      "Warner",     "Starc",
      "Cummins",     "Maxwell",    "Rabada",       "de Kock",    "du Plessis",
      "de Villiers", "Steyn",      "Amla",         "Azam",       "Afridi",
      "Khan",        "Akram",      "Miandad",      "Akhtar",     "ul-Haq",
      "Warne",       "Ponting",    "Lee",          "Clarke",     "Langer",
      "Hayden",      "Gilchrist",  "Gayle",        "Lara",       "Richards",
      "Sobers",      "Ambrose",    "Marshall",     "Lloyd",      "Sangakkara",
      "Mathews",     "Herath",     "Muralitharan", "Jayasuriya", "de Silva",
      "Dilshan",     "Taylor",     "Boult",        "Southee",    "Guptill",
      "McCullum",    "Vettori",    "Al Hasan",     "Rahim",      "Iqbal",
      "Mortaza",     "Kumble",     "Gavaskar",     "Dev",        "Gambhir",
      "Bhajji",      "Khan"};
  static std::mt19937 rng(std::time(nullptr));
  std::uniform_int_distribution<std::size_t> firstNameDist(
      0, firstNames.size() - 1);
  std::uniform_int_distribution<std::size_t> lastNameDist(0,
                                                          lastNames.size() - 1);

  std::string firstName = firstNames[firstNameDist(rng)];
  std::string lastName = lastNames[lastNameDist(rng)];

  return firstName + " " + lastName;
}

int main() {
  crow::App<crow::CORSHandler, crow::CookieParser> app;
  std::unordered_map<std::string, Room> wsrooms;
  std::unordered_map<crow::websocket::connection *, ConnData> connections;
  std::mutex rooms_mutex;

  auto &cors = app.get_middleware<crow::CORSHandler>();

  cors.global()
      .origin("http://localhost:5173")
      .methods("GET"_method, "POST"_method, "PUT"_method, "DELETE"_method)
      .headers("Content-Type", "Authorization")
      .allow_credentials()
      .max_age(1728000);

  CROW_ROUTE(app, "/")([]() { return "Hello world"; });

  CROW_ROUTE(app, "/create-room")
      .methods(
          crow::HTTPMethod::POST)([&app, &wsrooms](const crow::request &req) {
        auto &ctx = app.get_context<crow::CookieParser>(req);
        auto token_cookie = ctx.get_cookie("token");
        if (!token_cookie.empty()) {
          try {
            auto decoded = jwt::decode(token_cookie);
            auto verifier =
                jwt::verify()
                    .allow_algorithm(jwt::algorithm::hs256{jwtSecret})
                    .with_issuer(domain);
            verifier.verify(decoded);
            auto usernameClaim = decoded.get_payload_claim("username");
            auto roomIdClaim = decoded.get_payload_claim("room-id");
            auto roleClaim = decoded.get_payload_claim("role");

            if (wsrooms.find(roomIdClaim.as_string()) != wsrooms.end()) {

              crow::json::wvalue x(
                  {{"room-id", roomIdClaim.as_string()},
                   {"message", "Currently participating in another auction!"},
                   {"username", usernameClaim.as_string()},
                   {"role", roleClaim.as_string()}});
              crow::response res;
              res.code = 200;
              res.write(x.dump());
              return res;
            }
          } catch (const std::exception &e) {
          }
        }

        std::string roomId = generateRoomId();
        std::string leaderUsername = generate_username();
        auto token =
            jwt::create()
                .set_issuer(domain)
                .set_type("JWT")
                .set_issued_now()
                .set_payload_claim("username",
                                   jwt::claim(std::string(leaderUsername)))
                .set_payload_claim("room-id", jwt::claim(std::string(roomId)))
                .set_payload_claim("role", jwt::claim(std::string("leader")))
                .sign(jwt::algorithm::hs256{jwtSecret});
        crow::json::wvalue x({{"room-id", roomId},
                              {"message", "Room created!"},
                              {"username", leaderUsername},
                              {"role", "leader"}});
        crow::response res;
        Player p;
        p.username = leaderUsername;
        p.role = "leader";
        res.code = 200;
        res.write(x.dump());
        ctx.set_cookie("token", token)
            .path("/")
            .same_site(crow::CookieParser::Cookie::SameSitePolicy::Lax)
            //.secure()
            .httponly();
        Room r;
        wsrooms[roomId] = r;
        return res;
      });

  CROW_ROUTE(app, "/join-room/<string>")
      .methods(
          crow::HTTPMethod::POST)([&wsrooms, &app](const crow::request &req,
                                                   std::string roomId) {
        auto &ctx = app.get_context<crow::CookieParser>(req);
        auto token_cookie = ctx.get_cookie("token");
        if (!token_cookie.empty()) {
          try {
            auto decoded = jwt::decode(token_cookie);
            auto verifier =
                jwt::verify()
                    .allow_algorithm(jwt::algorithm::hs256{jwtSecret})
                    .with_issuer(domain);
            verifier.verify(decoded);
            auto usernameClaim = decoded.get_payload_claim("username");
            auto roomIdClaim = decoded.get_payload_claim("room-id");
            auto roleClaim = decoded.get_payload_claim("role");

            if (wsrooms.find(roomIdClaim.as_string()) != wsrooms.end()) {
              crow::json::wvalue x(
                  {{"room-id", roomIdClaim.as_string()},
                   {"message", "Currently participating in another auction!"},
                   {"username", usernameClaim.as_string()},
                   {"role", roleClaim.as_string()}});
              crow::response res;
              res.add_header("Access-Control-Allow-Origin",
                             "http://localhost:5173");
              res.add_header("Access-Control-Allow-Credentials", "true");
              res.add_header("Access-Control-Allow-Headers", "Content-Type");
              res.add_header("Access-Control-Allow-Methods",
                             "GET, POST, OPTIONS");
              res.code = 200;
              res.write(x.dump());
              return res;
            }
          } catch (const std::exception &e) {
          }
        }

        if (wsrooms.find(roomId) == wsrooms.end()) {
          crow::json::wvalue error_response;
          error_response["message"] = "Room does not exist";
          crow::response res;
          res.add_header("Access-Control-Allow-Origin",
                         "http://localhost:5173");
          res.add_header("Access-Control-Allow-Credentials", "true");
          res.add_header("Access-Control-Allow-Headers", "Content-Type");
          res.add_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
          res.code = 404;
          res.write(error_response.dump());
          return res;
        }
        std::string playerName = generate_username();
        crow::json::wvalue x({{"room-id", roomId},
                              {"username", playerName},
                              {"message", "Welcome to the auction!"},
                              {"role", "player"}});
        auto token =
            jwt::create()
                .set_issuer(domain)
                .set_type("JWT")
                .set_issued_now()
                .set_payload_claim("username",
                                   jwt::claim(std::string(playerName)))
                .set_payload_claim("room-id", jwt::claim(std::string(roomId)))
                .set_payload_claim("role", jwt::claim(std::string("player")))
                .sign(jwt::algorithm::hs256{jwtSecret});
        Player p;
        p.username = playerName;
        p.role = "player";
        crow::response res;
        res.add_header("Access-Control-Allow-Origin", "http://localhost:5173");
        res.add_header("Access-Control-Allow-Credentials", "true");
        res.add_header("Access-Control-Allow-Headers", "Content-Type");
        res.add_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
        res.code = 200;
        res.write(x.dump());
        ctx.set_cookie("token", token)
            .path("/")
            .same_site(crow::CookieParser::Cookie::SameSitePolicy::Lax)
            //.secure()
            .httponly();
        return res;
      });

  CROW_WEBSOCKET_ROUTE(app, "/wsrooms")
      .onaccept(
          [&wsrooms, &app, &rooms_mutex, &connections](const crow::request &req,
                                                       void **userdata) {
            auto &ctx = app.get_context<crow::CookieParser>(req);
            auto token_cookie = ctx.get_cookie("token");
            if (token_cookie.empty()) {
              return false;
            }
            auto decoded = jwt::decode(token_cookie);
            auto verifier =
                jwt::verify()
                    .allow_algorithm(jwt::algorithm::hs256{jwtSecret})
                    .with_issuer(domain);
            verifier.verify(decoded);
            auto username = decoded.get_payload_claim("username").as_string();
            auto roomId = decoded.get_payload_claim("room-id").as_string();
            auto role = decoded.get_payload_claim("role").as_string();

            if (roomId.empty()) {
              CROW_LOG_WARNING
                  << "WebSocket connection rejected: No room ID in token";
              return false;
            }

            std::lock_guard<std::mutex> lock(rooms_mutex);
            if (wsrooms.find(roomId) == wsrooms.end()) {
              CROW_LOG_WARNING << "WebSocket connection rejected: Room "
                               << roomId << " doesn't exist";
              return false;
            }

            Player p;
            p.role = role;
            p.username = username;
            p.team = "Observer";
            ConnData cd{roomId, p};
            connections[reinterpret_cast<crow::websocket::connection *>(
                *userdata)] = cd;
            return true;
          })
      .onopen([&wsrooms, &app, &rooms_mutex,
               &connections](crow::websocket::connection &conn) {
        auto details = connections[&conn];
        std::lock_guard<std::mutex> lock(rooms_mutex);

        for (auto it = wsrooms[details.roomId].players.begin();
             it != wsrooms[details.roomId].players.end(); ++it) {
          if (it->second.username == details.player.username) {
            it->first->close(
                crow::json::wvalue({{"type", "error"},
                                    {"message", "You were disconnected because "
                                                "you logged in elsewhere."}})
                    .dump());
            wsrooms[details.roomId].connections.erase(it->first);
            wsrooms[details.roomId].players.erase(it);
            connections.erase(it->first);
            break;
          }
        }

        wsrooms[details.roomId].players[&conn] = details.player;
        wsrooms[details.roomId].connections.insert(&conn);

        crow::json::wvalue::list playerList;
        for (auto &playerEntry : wsrooms[details.roomId].players) {
          Player &player = playerEntry.second;
          crow::json::wvalue playerJson({{"username", player.username},
                                         {"team", player.team},
                                         {"role", player.role}});
          playerList.push_back(playerJson);
        }

        conn.send_text(
            crow::json::wvalue({{"type", "Player List"}, {"list", playerList}})
                .dump());

        for (auto &existingConn : wsrooms[details.roomId].connections) {
          existingConn->send_text(
              crow::json::wvalue({{"type", "New Player"},
                                  {"username", details.player.username},
                                  {"role", details.player.role},
                                  {"team", "Observer"}})
                  .dump());
        }
      });
  /*.onopen([&wsrooms, &app,
           &rooms_mutex](crow::websocket::connection &conn) {
    auto &ctx = app.get_context<crow::CookieParser>(req);
    auto token_cookie = ctx.get_cookie("token");
    if (wsrooms.find(roomId) == wsrooms.end()) {
      conn.close(crow::json::wvalue(
                     {{"type", "error"}, {"message", "Invalid Room"}})
                     .dump());
      return;
    }
    if (token_cookie.empty()) {
      conn.close(
          crow::json::wvalue({{"type", "error"}, {"message", "No Token"}})
              .dump());
      return;
    }
    try {
      auto decoded = jwt::decode(token_cookie);
      auto verifier = jwt::verify()
                          .allow_algorithm(jwt::algorithm::hs256{jwtSecret})
                          .with_issuer(domain);
      verifier.verify(decoded);
      auto usernameClaim = decoded.get_payload_claim("username");
      auto roomIdClaim = decoded.get_payload_claim("room-id");
      auto roleClaim = decoded.get_payload_claim("role");

      if (roomIdClaim.as_string() != roomId) {
        conn.close(crow::json::wvalue(
                       {{"type", "error"}, {"message", "Room Mismatch"}})
                       .dump());
        return;
      }
      std::lock_guard<std::mutex> lock(rooms_mutex);

      for (auto it = wsrooms[roomId].players.begin();
           it != wsrooms[roomId].players.end(); ++it) {
        if (it->second.username == usernameClaim.as_string()) {
          it->first->close(crow::json::wvalue(
                               {{"type", "error"},
                                {"message", "You were disconnected because "
                                            "you logged in elsewhere."}})
                               .dump());
          wsrooms[roomId].connections.erase(it->first);
          wsrooms[roomId].players.erase(it);
          break;
        }
      }

      Player p{usernameClaim.as_string(), roleClaim.as_string()};

      wsrooms[roomId].players[&conn] = p;
      wsrooms[roomId].connections.insert(&conn);

      crow::json::wvalue::list playerList;
      for (auto &playerEntry : wsrooms[roomId].players) {
        Player &player = playerEntry.second;
        crow::json::wvalue playerJson({{"username", player.username},
                                       {"team", player.team},
                                       {"role", player.role}});
        playerList.push_back(playerJson);
      }

      conn.send_text(crow::json::wvalue(
                         {{"type", "Player List"}, {"list", playerList}})
                         .dump());

      for (auto &existingConn : wsrooms[roomId].connections) {
        existingConn->send_text(
            crow::json::wvalue({{"type", "New Player"},
                                {"username", usernameClaim.as_string()},
                                {"role", roleClaim.as_string()},
                                {"team", "Observer"}})
                .dump());
      }

    } catch (const std::exception &e) {
      CROW_LOG_WARNING << "JWT error: " << e.what();
      conn.close(crow::json::wvalue(
                     {{"type", "error"}, {"message", "Token Error"}})
                     .dump());
      return;
    }
  })
  .onclose([&wsrooms, &rooms_mutex](crow::websocket::connection &conn,
                                    const std::string &reason, uint16_t) {
    std::lock_guard<std::mutex> lock(rooms_mutex);
    auto roomIt = wsrooms.find(roomId);
    if (roomIt == wsrooms.end())
      return;

    Room &room = roomIt->second;
    Player &p = room.players[&conn];
    room.connections.erase(&conn);
    room.players.erase(&conn);
    for (auto &existingConn : wsrooms[roomId].connections) {
      existingConn->send_text(crow::json::wvalue({{"type", "Player Left"},
                                                  {"username", p.username},
                                                  {"role", p.role},
                                                  {"team", p.team}})
                                  .dump());
    }
  })
  .onmessage([&wsrooms, &rooms_mutex](crow::websocket::connection &conn,
                                      const std::string &data,
                                      bool is_binary) {
    crow::json::rvalue message = crow::json::load(data);
    if (!message || !message.has("type"))
      return;

    std::string type = message["type"].s();

    if (type == "Username Change" && message.has("newUsername")) {
      std::string newUsername = message["newUsername"].s();

      std::lock_guard<std::mutex> lock(rooms_mutex);
      auto &room = wsrooms[roomId];
      std::string oldUsername;

      for (auto &[c, player] : room.players) {
        if (c == &conn) {
          oldUsername = player.username;
          player.username = newUsername;
          break;
        }
      }
      crow::json::wvalue updateMsg;
      updateMsg["type"] = "Username Change";
      updateMsg["newUsername"] = newUsername;
      updateMsg["oldUsername"] = oldUsername;

      for (auto *c : room.connections) {
        c->send_text(updateMsg.dump());
      }
    }

    if (type == "Team Change" && message.has("newTeam")) {

      std::string newTeam = message["newTeam"].s();

      std::lock_guard<std::mutex> lock(rooms_mutex);
      auto &room = wsrooms[roomId];
      std::string username;
      for (auto &[c, player] : room.players) {
        if (c == &conn) {
          username = player.username;
          player.team = newTeam;
          break;
        }
      }
      crow::json::wvalue updateMsg;
      updateMsg["type"] = "Team Change";
      updateMsg["username"] = username;
      updateMsg["newTeam"] = newTeam;

      for (auto *c : room.connections) {
        c->send_text(updateMsg.dump());
      }
    }
  }); */

  app.loglevel(crow::LogLevel::Debug);
  app.port(18080).multithreaded().run();
}
