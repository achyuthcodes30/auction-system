#include "crow.h"
#include "crow/middlewares/cookie_parser.h"
#include "crow/middlewares/cors.h"
#include <crow/app.h>
#include <crow/common.h>
#include <crow/http_response.h>
#include <crow/logging.h>
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
      .allow_credentials()
      .methods("GET"_method, "POST"_method, "PUT"_method, "DELETE"_method,
               "OPTIONS"_method, "HEAD"_method)
      .headers("Content-Type", "Authorization", "Accept", "Origin", "Refresh");

  CROW_ROUTE(app, "/")([]() { return "Hello world"; });

  CROW_ROUTE(app, "/create-room")
      .methods(
          crow::HTTPMethod::POST)([&app, &wsrooms](const crow::request &req) {
        auto &ctx = app.get_context<crow::CookieParser>(req);
        auto token_cookie = ctx.get_cookie("token");
        crow::response res;
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
        wsrooms[roomId] = Room{};
        CROW_LOG_INFO << "Room created: " << roomId;
        CROW_LOG_INFO << "players.size(): " << wsrooms[roomId].players.size();
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
        res.code = 200;
        res.write(x.dump());
        ctx.set_cookie("token", token)
            .path("/")
            .same_site(crow::CookieParser::Cookie::SameSitePolicy::Lax)
            //.secure()
            .httponly();
        return res;
      });

  CROW_ROUTE(app, "/wsrooms")
      .websocket(&app)
      .onaccept([&](const crow::request &req, void **userdata) {
        auto &ctx = app.get_context<crow::CookieParser>(req);
        auto token_cookie = ctx.get_cookie("token");

        if (token_cookie.empty()) {
          CROW_LOG_WARNING << "WebSocket rejected: No token cookie";
          return false;
        }

        try {
          auto decoded = jwt::decode(token_cookie);
          auto verifier = jwt::verify()
                              .allow_algorithm(jwt::algorithm::hs256{jwtSecret})
                              .with_issuer(domain);
          verifier.verify(decoded);

          std::string username =
              decoded.get_payload_claim("username").as_string();
          std::string roomId = decoded.get_payload_claim("room-id").as_string();
          std::string role = decoded.get_payload_claim("role").as_string();

          if (username.empty() || roomId.empty() || role.empty()) {
            CROW_LOG_WARNING << "WebSocket rejected: Incomplete token payload";
            return false;
          }

          std::lock_guard<std::mutex> lock(rooms_mutex);
          if (wsrooms.find(roomId) == wsrooms.end()) {
            CROW_LOG_WARNING << "WebSocket rejected: Room " << roomId
                             << " not found";
            return false;
          }

          Player player{username, "observer", role};

          auto &room = wsrooms[roomId];
          auto it = std::find_if(room.players.begin(), room.players.end(),
                                 [&](const auto &entry) {
                                   return entry.second.username == username;
                                 });

          if (it != room.players.end()) {
            player.team = it->second.team;
            CROW_LOG_INFO << "Returning player " << username << " with team "
                          << player.team;
          } else {
            CROW_LOG_INFO << "New player " << username
                          << " joining as observer";
          }
          ConnData *cd = new ConnData{roomId, player};
          *userdata = cd;
          CROW_LOG_INFO << "WebSocket accepted: " << username << " in room "
                        << roomId;
          return true;

        } catch (const std::exception &e) {
          CROW_LOG_WARNING << "WebSocket rejected: JWT error - " << e.what();
          return false;
        }
      })
      .onopen([&](crow::websocket::connection &conn) {
        auto *cd = static_cast<ConnData *>(conn.userdata());

        if (!cd) {
          CROW_LOG_ERROR << "WebSocket open called with null userdata";
          conn.close("Invalid connection");
          return;
        }

        const std::string roomId = cd->roomId;
        const Player player = cd->player;

        CROW_LOG_INFO << "Assigned player successfully";
        std::lock_guard<std::mutex> lock(rooms_mutex);

        auto roomIt = wsrooms.find(roomId);
        if (roomIt == wsrooms.end()) {
          CROW_LOG_ERROR << "WebSocket open: Room " << roomId << " not found";
          conn.close("Room not found");
          delete cd;
          return;
        }

        Room &room = roomIt->second;
        room.players[&conn] = player;
        room.connections.insert(&conn);
        connections[&conn] = ConnData{roomId, player};

        delete cd;

        conn.send_text(crow::json::wvalue{
            {"type", "Your Team"},
            {"team",
             player.team}}.dump());
        crow::json::wvalue::list playerList;
        CROW_LOG_INFO << "Collecting Player List";
        for (const auto &[_, p] : room.players) {
          if (p.username != player.username) {
            playerList.push_back(crow::json::wvalue{
                {"username", p.username}, {"team", p.team}, {"role", p.role}});
          }
        }

        try {
          conn.send_text(crow::json::wvalue{
              {"type", "Player List"},
              {"list",
               playerList}}.dump());
          CROW_LOG_DEBUG << "Sent player list to " << player.username;
        } catch (const std::exception &e) {
          CROW_LOG_ERROR << "send_text failed: " << e.what();
        }

        for (auto *other : wsrooms[roomId].connections) {
          if (other != &conn) {
            other->send_text(crow::json::wvalue{
                {"type", "New Player"},
                {"username", player.username},
                {"team", player.team},
                {"role",
                 player.role}}.dump());
          }
        }
      })
      .onmessage([&](crow::websocket::connection &conn,
                     const std::string &message, bool is_binary) {
        auto json = crow::json::load(message);
        if (!json)
          return;

        std::string type = json["type"].s();
        if (type == "Change Username") {
          std::string newUsername = json["newUsername"].s();
          std::lock_guard<std::mutex> lock(rooms_mutex);
          auto it = connections.find(&conn);
          if (it != connections.end()) {
            ConnData &connData = it->second;
            auto oldUsername = connData.player.username;
            connData.player.username = newUsername;
            wsrooms[connData.roomId].players[&conn].username = newUsername;
            conn.send_text(crow::json::wvalue{
                {"type", "Your Username"},
                {"username",
                 newUsername}}.dump());
            for (auto *other : wsrooms[connData.roomId].connections) {
              if (other != &conn) {
                other->send_text(crow::json::wvalue{
                    {"type", "Username Change"},
                    {"oldUsername", oldUsername},
                    {"newUsername",
                     newUsername}}.dump());
              }
            }
          }
        }
        if (type == "Change Team") {
          std::string newTeam = json["newTeam"].s();
          std::lock_guard<std::mutex> lock(rooms_mutex);
          auto it = connections.find(&conn);
          if (it != connections.end()) {
            ConnData &connData = it->second;
            auto username = connData.player.username;
            connData.player.team = newTeam;
            wsrooms[connData.roomId].players[&conn].team = newTeam;
            conn.send_text(crow::json::wvalue{
                {"type", "Your Team"},
                {"team",
                 newTeam}}.dump());
            for (auto *other : wsrooms[connData.roomId].connections) {
              if (other != &conn) {
                other->send_text(crow::json::wvalue{
                    {"type", "Team Change"},
                    {"username", username},
                    {"team",
                     newTeam}}.dump());
              }
            }
          }
        }
      });

  app.loglevel(crow::LogLevel::Debug);
  app.port(18080).multithreaded().run();
}
