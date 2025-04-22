#include <algorithm>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
struct Player {
  std::string name;
  std::string country;
  std::string role;
  std::string prev_team;
  std::string cap_status;
  float base_price;
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

  return {};
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

std::vector<Player>
load_players_for_category(const std::string &csv_file,
                          const std::string &target_category) {
  std::ifstream file(csv_file);
  std::vector<Player> players;
  std::string line;

  std::getline(file, line);

  while (std::getline(file, line)) {
    std::stringstream ss(line);
    std::string item;
    Player p;
    std::string player_category;

    std::getline(ss, player_category, ',');
    if (player_category != target_category)
      continue;

    std::getline(ss, p.name, ',');
    std::getline(ss, p.country, ',');
    std::getline(ss, p.prev_team, ',');
    std::getline(ss, p.cap_status, ',');
    std::getline(ss, p.role, ',');
    std::getline(ss, item, ',');
    p.base_price = std::stof(item);

    players.push_back(p);
  }

  return players;
}

std::string format_price(float price) {

  return price >= 1 ? std::to_string(price) + "Cr"
                    : std::to_string(price * 100) + "L";
}

void auction_players_from_csv(const std::string &csv_file,
                              const std::vector<std::string> &category_order) {
  std::random_device rd;
  std::mt19937 rng(rd());

  int count = 1;

  for (const auto &category : category_order) {
    auto players = load_players_for_category(csv_file, category);
    if (!players.empty()) {
      std::cout << category << "\n";
      std::shuffle(players.begin(), players.end(), rng);

      for (const auto &p : players) {
        std::cout << count++ << ". " << p.name << " - " << p.role << " - ₹"
                  << format_price(p.base_price) << "\n";

        std::string winning_team;
        std::string input;
        float current_bid;
        auto last_bid_time = std::chrono::steady_clock::now();

        while (true) {
          std::cout << "(Enter bidder or 'close'): ";
          std::getline(std::cin, input);

          // Check timer expiry (20 seconds idle)
          auto now = std::chrono::steady_clock::now();
          auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                             now - last_bid_time)
                             .count();

          if (elapsed >= 20) {
            std::cout
                << "⏰ Bidding auto-closed after 20 seconds of inactivity.\n";
            break;
          }

          if (input.empty()) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
          }

          if (input == "close") {
            std::cout << "🔒 Bidding manually closed.\n";
            break;
          }

          std::string bidder = input;
          if (winning_team.empty()) {
            // First bidder starts at base price
            current_bid = p.base_price;
          } else {
            float increment = get_bid_increment(current_bid);
            current_bid += increment;
          }
          winning_team = bidder;
          last_bid_time = std::chrono::steady_clock::now();

          std::cout << bidder << " bids ₹" << format_price(current_bid)
                    << "Cr\n";
        }

        if (!winning_team.empty()) {
          std::cout << "✅ " << p.name << " SOLD to " << winning_team
                    << " for ₹" << format_price(current_bid) << "Cr\n";
        } else {
          std::cout << "❌ " << p.name << " UNSOLD" << "\n";
        }
      }
    }
  }
  std::cout << "\n";
}

int main() {
  std::string csv_file = "Auction_List.csv";
  std::string env_file = ".env";

  auto category_order = read_category_order(env_file);
  auction_players_from_csv(csv_file, category_order);

  return 0;
}
