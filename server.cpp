#include "crow.h"
#include "crow/middlewares/cookie_parser.h"
#include "crow/middlewares/cors.h"
#include <crow/app.h>
#include <crow/common.h>
#include <crow/http_response.h>
#include <crow/logging.h>
#include <jwt-cpp/jwt.h>
#include <unordered_set>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <mutex>
#include <atomic>
#include <condition_variable>
#include <future>

std::string domain = "bidblitz.com";
std::string jwtSecret = "kohligoat";

struct AuctionPlayer {
  std::string name;
  std::string country;
  std::string role;
  std::string prev_team;
  std::string cap_status;
  float base_price;
  float current_bid;
  std::string winning_team;
  std::string category;
};

std::vector<std::string> read_category_order(const std::string &filename) {
  std::ifstream env_file(filename);
  std::string line;

  while (std::getline(env_file, line)) {
    if (line.rfind("CATEGORY_ORDER=", 0) == 0) {
      std::string categories = line.substr(15);
      std::vector<std::string> result;
      std::stringstream ss(categories);
      std::string token;
      while (std::getline(ss, token, ',')) {
        result.push_back(token);
      }
      return result;
    }
  }

  return {"MQ1", "BA1", "AL1", "WK1", "FA1", "SP1"};  // Default categories if file not found
}

float get_bid_increment(float current_bid) {
  if (current_bid < 1.0f)
    return 0.05f;
  else if (current_bid < 2.0f)
    return 0.1f;
  else if (current_bid < 5.0f)
    return 0.2f;
  else
    return 0.25f;
}

std::string format_price(float price) {
  return price >= 1 ? std::to_string(price) + "Cr"
                    : std::to_string(price * 100) + "L";
}

std::vector<AuctionPlayer>
load_players_from_csv(const std::string &csv_file, const std::vector<std::string> &categories) {
  std::ifstream file(csv_file);
  std::vector<AuctionPlayer> players;
  std::string line;

  if (!file.is_open()) {
    return players;
  }

  // Skip header
  std::getline(file, line);

  while (std::getline(file, line)) {
    std::stringstream ss(line);
    std::string item;
    AuctionPlayer p;

    std::getline(ss, p.category, ',');
    
    // Check if this category is in our list
    if (std::find(categories.begin(), categories.end(), p.category) == categories.end()) {
      continue;
    }

    std::getline(ss, p.name, ',');
    std::getline(ss, p.country, ',');
    std::getline(ss, p.prev_team, ',');
    std::getline(ss, p.cap_status, ',');
    std::getline(ss, p.role, ',');
    std::getline(ss, item, ',');
    
    try {
      p.base_price = std::stof(item);
      p.current_bid = 0.0f;
      players.push_back(p);
    } catch (const std::exception& e) {
      // Skip invalid entries
    }
  }

  return players;
}

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
  enum class Status { TeamSelection, InProgress, Paused, Completed, Cancelled };
  
  Status status;
  std::vector<AuctionPlayer> players;
  std::vector<std::string> category_order;
  size_t current_player_index;
  std::atomic<bool> auction_running;
  std::atomic<int> timer_seconds;
  std::mutex auction_mutex;
  std::string current_bidder;
  std::condition_variable timer_cv;
  std::future<void> timer_future;
  std::string csv_file;
  
  Auction() : status(Status::TeamSelection), current_player_index(0), auction_running(false), timer_seconds(0), csv_file("Auction_List.csv") {}
  
  void initialize() {
    std::string env_file = ".env";
    category_order = read_category_order(env_file);
    players = load_players_from_csv(csv_file, category_order);
    
    // Shuffle players within each category
    std::map<std::string, std::vector<AuctionPlayer>> players_by_category;
    for (const auto& player : players) {
      players_by_category[player.category].push_back(player);
    }
    
    players.clear();
    std::random_device rd;
    std::mt19937 rng(rd());
    
    for (const auto& category : category_order) {
      auto& cat_players = players_by_category[category];
      std::shuffle(cat_players.begin(), cat_players.end(), rng);
      players.insert(players.end(), cat_players.begin(), cat_players.end());
    }
    
    current_player_index = 0;
    status = Status::TeamSelection;
  }
  
  bool start_auction() {
    if (status != Status::TeamSelection && status != Status::Paused) {
      return false;
    }
    
    if (players.empty()) {
      initialize();
    }
    
    if (current_player_index >= players.size()) {
      status = Status::Completed;
      return false;
    }
    
    status = Status::InProgress;
    auction_running = true;
    timer_seconds = 20; // 20 seconds initial timer
    
    // Start timer in a separate thread
    timer_future = std::async(std::launch::async, [this]() {
      std::unique_lock<std::mutex> lock(auction_mutex);
      while (auction_running && timer_seconds > 0) {
        timer_cv.wait_for(lock, std::chrono::seconds(1), [this] {
          return !auction_running || timer_seconds <= 0;
        });
        
        if (auction_running && timer_seconds > 0) {
          timer_seconds--;
        }
      }
      
      if (auction_running && timer_seconds <= 0) {
        // Timer expired, finalize current player
        finalize_current_player();
      }
    });
    
    return true;
  }
  
  void pause_auction() {
    if (status == Status::InProgress) {
      status = Status::Paused;
      auction_running = false;
      timer_cv.notify_all();
      if (timer_future.valid()) {
        timer_future.wait();
      }
    }
  }
  
  void cancel_auction() {
    status = Status::Cancelled;
    auction_running = false;
    timer_cv.notify_all();
    if (timer_future.valid()) {
      timer_future.wait();
    }
  }
  
  bool place_bid(const std::string& team_name) {
    std::lock_guard<std::mutex> lock(auction_mutex);
    
    if (status != Status::InProgress || current_player_index >= players.size()) {
      return false;
    }
    
    AuctionPlayer& current_player = players[current_player_index];
    
    if (current_player.winning_team.empty()) {
      // First bid at base price
      current_player.current_bid = current_player.base_price;
    } else {
      // Increment bid
      float increment = get_bid_increment(current_player.current_bid);
      current_player.current_bid += increment;
    }
    
    current_player.winning_team = team_name;
    current_bidder = team_name;
    timer_seconds = 20; // Reset timer on new bid
    timer_cv.notify_all();
    
    return true;
  }
  
  void finalize_current_player() {
    std::lock_guard<std::mutex> lock(auction_mutex);
    
    if (current_player_index < players.size()) {
      auto& player = players[current_player_index];
      
      // Mark the player as sold or unsold
      if (player.winning_team.empty()) {
        CROW_LOG_INFO << "Player " << player.name << " UNSOLD";
      } else {
        CROW_LOG_INFO << "Player " << player.name << " SOLD to " << player.winning_team 
                     << " for " << format_price(player.current_bid);
      }
      
      // Move to next player
      current_player_index++;
      
      if (current_player_index < players.size()) {
        // Reset for next player
        timer_seconds = 20;
      } else {
        // No more players
        status = Status::Completed;
        auction_running = false;
      }
    }
  }
  
  AuctionPlayer* get_current_player() {
    std::lock_guard<std::mutex> lock(auction_mutex);
    
    if (current_player_index < players.size()) {
      return &players[current_player_index];
    }
    
    return nullptr;
  }
  
  crow::json::wvalue get_auction_state() {
    std::lock_guard<std::mutex> lock(auction_mutex);
    
    crow::json::wvalue state;
    state["status"] = static_cast<int>(status);
    state["timer"] = timer_seconds;
    
    if (current_player_index < players.size()) {
      AuctionPlayer& player = players[current_player_index];
      state["currentPlayer"] = crow::json::wvalue{
        {"name", player.name},
        {"role", player.role},
        {"country", player.country},
        {"category", player.category},
        {"basePrice", player.base_price},
        {"currentBid", player.current_bid},
        {"winningTeam", player.winning_team}
      };
    }
    
    state["playerIndex"] = static_cast<int>(current_player_index);
    state["totalPlayers"] = static_cast<int>(players.size());
    
    return state;
  }
  
  void next_player() {
    std::lock_guard<std::mutex> lock(auction_mutex);
    
    finalize_current_player();
    
    if (current_player_index < players.size()) {
      timer_seconds = 20;
      timer_cv.notify_all();
    } else {
      status = Status::Completed;
      auction_running = false;
    }
  }
  
  void reset_auction() {
    std::lock_guard<std::mutex> lock(auction_mutex);
    
    status = Status::TeamSelection;
    auction_running = false;
    timer_cv.notify_all();
    if (timer_future.valid()) {
      timer_future.wait();
    }
    
    // Reset player data
    for (auto& player : players) {
      player.current_bid = 0.0f;
      player.winning_team = "";
    }
    current_player_index = 0;
  }
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
  std::mutex room_mutex;
  std::thread timer_broadcast_thread;
  std::atomic<bool> should_broadcast;
  
  Room() : should_broadcast(false) {}
  
  ~Room() {
    should_broadcast = false;
    if (timer_broadcast_thread.joinable()) {
      timer_broadcast_thread.join();
    }
  }
  
  void start_timer_broadcast() {
    should_broadcast = true;
    timer_broadcast_thread = std::thread([this]() {
      while (should_broadcast) {
        // Only broadcast if auction is in progress
        if (auction.status == Auction::Status::InProgress) {
          std::lock_guard<std::mutex> lock(room_mutex);
          
          auto state = auction.get_auction_state();
          std::string state_json = state.dump();
          
          // Broadcast timer updates to all connections
          for (auto conn : connections) {
            conn->send_text(crow::json::wvalue{
              {"type", "AuctionUpdate"},
              {"state", state}
            }.dump());
          }
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
      }
    });
  }
  
  void broadcast_auction_result() {
    std::lock_guard<std::mutex> lock(room_mutex);
    auto* player = auction.get_current_player();
    
    if (player) {
      for (auto conn : connections) {
        conn->send_text(crow::json::wvalue{
          {"type", "AuctionResult"},
          {"player", crow::json::wvalue{
            {"name", player->name},
            {"role", player->role},
            {"country", player->country},
            {"basePrice", player->base_price},
            {"soldPrice", player->current_bid},
            {"soldTo", player->winning_team},
            {"status", player->winning_team.empty() ? "UNSOLD" : "SOLD"}
          }}
        }.dump());
      }
    }
  }
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
        std::lock_guard<std::mutex> global_lock(rooms_mutex);
        auto it = connections.find(&conn);
        if (it == connections.end()) {
          return;
        }

        ConnData &connData = it->second;
        auto roomId = connData.roomId;
        auto &room = wsrooms[roomId];

        if (type == "Change Username") {
          std::string newUsername = json["newUsername"].s();
          auto oldUsername = connData.player.username;
          connData.player.username = newUsername;
          room.players[&conn].username = newUsername;
          conn.send_text(crow::json::wvalue{
              {"type", "Your Username"},
              {"username", newUsername}}.dump());
          for (auto *other : room.connections) {
            if (other != &conn) {
              other->send_text(crow::json::wvalue{
                  {"type", "Username Change"},
                  {"oldUsername", oldUsername},
                  {"newUsername", newUsername}}.dump());
            }
          }
        }
        else if (type == "Change Team") {
          std::string newTeam = json["newTeam"].s();
          auto username = connData.player.username;
          connData.player.team = newTeam;
          room.players[&conn].team = newTeam;
          conn.send_text(crow::json::wvalue{
              {"type", "Your Team"},
              {"team", newTeam}}.dump());
          for (auto *other : room.connections) {
            if (other != &conn) {
              other->send_text(crow::json::wvalue{
                  {"type", "Team Change"},
                  {"username", username},
                  {"team", newTeam}}.dump());
            }
          }
        }
        else if (type == "Start Auction") {
          // Only leader can start the auction
          if (connData.player.role == "leader") {
            CROW_LOG_INFO << "Starting auction in room " << roomId;
            // Initialize auction if needed
            if (room.auction.players.empty()) {
              room.auction.initialize();
            }
            
            if (room.auction.start_auction()) {
              // Start broadcasting timer updates to all clients
              if (!room.should_broadcast) {
                room.start_timer_broadcast();
              }
              
              // Notify all clients that auction has started
              auto state = room.auction.get_auction_state();
              for (auto *client : room.connections) {
                client->send_text(crow::json::wvalue{
                  {"type", "Auction Started"},
                  {"state", state}
                }.dump());
              }
            } else {
              // Failed to start auction (e.g., no players or already completed)
              conn.send_text(crow::json::wvalue{
                {"type", "Error"},
                {"message", "Cannot start auction. It may be completed or already in progress."}
              }.dump());
            }
          } else {
            conn.send_text(crow::json::wvalue{
              {"type", "Error"},
              {"message", "Only the room leader can start the auction"}
            }.dump());
          }
        }
        else if (type == "Place Bid") {
          if (room.auction.status == Auction::Status::InProgress) {
            std::string teamName = connData.player.team;
            
            // Only players with team names can bid
            if (!teamName.empty()) {
              CROW_LOG_INFO << "Bid from " << connData.player.username << " for team " << teamName;
              
              if (room.auction.place_bid(teamName)) {
                // Broadcast the new bid to all clients
                auto state = room.auction.get_auction_state();
                for (auto *client : room.connections) {
                  client->send_text(crow::json::wvalue{
                    {"type", "Bid Placed"},
                    {"bidder", connData.player.username},
                    {"team", teamName},
                    {"state", state}
                  }.dump());
                }
              } else {
                conn.send_text(crow::json::wvalue{
                  {"type", "Error"},
                  {"message", "Failed to place bid"}
                }.dump());
              }
            } else {
              conn.send_text(crow::json::wvalue{
                {"type", "Error"},
                {"message", "You must be part of a team to place bids"}
              }.dump());
            }
          } else {
            conn.send_text(crow::json::wvalue{
              {"type", "Error"},
              {"message", "Auction is not in progress"}
            }.dump());
          }
        }
        else if (type == "Next Player") {
          // Only leader can move to next player
          if (connData.player.role == "leader") {
            if (room.auction.status == Auction::Status::InProgress) {
              // First broadcast the result for the current player
              room.broadcast_auction_result();
              
              // Then move to the next player
              room.auction.next_player();
              
              if (room.auction.status == Auction::Status::Completed) {
                // Auction has ended after this player
                for (auto *client : room.connections) {
                  client->send_text(crow::json::wvalue{
                    {"type", "Auction Completed"}
                  }.dump());
                }
                room.should_broadcast = false;
              } else {
                // Continue with the next player
                auto state = room.auction.get_auction_state();
                for (auto *client : room.connections) {
                  client->send_text(crow::json::wvalue{
                    {"type", "Next Player"},
                    {"state", state}
                  }.dump());
                }
              }
            } else {
              conn.send_text(crow::json::wvalue{
                {"type", "Error"},
                {"message", "Auction is not in progress"}
              }.dump());
            }
          } else {
            conn.send_text(crow::json::wvalue{
              {"type", "Error"},
              {"message", "Only the room leader can move to the next player"}
            }.dump());
          }
        }
        else if (type == "Pause Auction") {
          // Only leader can pause the auction
          if (connData.player.role == "leader") {
            room.auction.pause_auction();
            
            for (auto *client : room.connections) {
              client->send_text(crow::json::wvalue{
                {"type", "Auction Paused"}
              }.dump());
            }
          } else {
            conn.send_text(crow::json::wvalue{
              {"type", "Error"},
              {"message", "Only the room leader can pause the auction"}
            }.dump());
          }
        }
        else if (type == "Resume Auction") {
          // Only leader can resume the auction
          if (connData.player.role == "leader") {
            if (room.auction.start_auction()) {
              auto state = room.auction.get_auction_state();
              for (auto *client : room.connections) {
                client->send_text(crow::json::wvalue{
                  {"type", "Auction Resumed"},
                  {"state", state}
                }.dump());
              }
            } else {
              conn.send_text(crow::json::wvalue{
                {"type", "Error"},
                {"message", "Cannot resume auction"}
              }.dump());
            }
          } else {
            conn.send_text(crow::json::wvalue{
              {"type", "Error"},
              {"message", "Only the room leader can resume the auction"}
            }.dump());
          }
        }
        else if (type == "Reset Auction") {
          // Only leader can reset the auction
          if (connData.player.role == "leader") {
            room.auction.reset_auction();
            
            for (auto *client : room.connections) {
              client->send_text(crow::json::wvalue{
                {"type", "Auction Reset"}
              }.dump());
            }
          } else {
            conn.send_text(crow::json::wvalue{
              {"type", "Error"},
              {"message", "Only the room leader can reset the auction"}
            }.dump());
          }
        }
        else if (type == "Get Auction State") {
          auto state = room.auction.get_auction_state();
          conn.send_text(crow::json::wvalue{
            {"type", "Auction State"},
            {"state", state}
          }.dump());
        }
      })
      .onclose([&](crow::websocket::connection &conn, const std::string &reason) {
        std::lock_guard<std::mutex> lock(rooms_mutex);
        auto it = connections.find(&conn);
        if (it != connections.end()) {
          const std::string &roomId = it->second.roomId;
          const std::string &username = it->second.player.username;
          const std::string &team = it->second.player.team;
          const std::string &role = it->second.player.role;
          
          CROW_LOG_INFO << "WebSocket closed: " << username << " from room " << roomId;
          
          auto roomIt = wsrooms.find(roomId);
          if (roomIt != wsrooms.end()) {
            Room &room = roomIt->second;
            room.players.erase(&conn);
            room.connections.erase(&conn);
            
            // Notify other clients that this player has left
            for (auto *other : room.connections) {
              other->send_text(crow::json::wvalue{
                  {"type", "Player Left"},
                  {"username", username},
                  {"team", team},
                  {"role", role}
              }.dump());
            }
            
            // If this is the leader and they disconnect, maybe pause the auction
            if (role == "leader" && room.auction.status == Auction::Status::InProgress) {
              room.auction.pause_auction();
              
              for (auto *other : room.connections) {
                other->send_text(crow::json::wvalue{
                  {"type", "Auction Paused"},
                  {"message", "Room leader disconnected"}
                }.dump());
              }
            }
            
            // If room is empty, clean up
            if (room.connections.empty()) {
              CROW_LOG_INFO << "Room " << roomId << " is empty, cleaning up";
              room.should_broadcast = false; // Stop timer thread
              if (room.timer_broadcast_thread.joinable()) {
                room.timer_broadcast_thread.join();
              }
              
              wsrooms.erase(roomId);
            }
          }
          
          connections.erase(&conn);
        }
      });

  app.loglevel(crow::LogLevel::Debug);
  app.port(18080).multithreaded().run();
}
