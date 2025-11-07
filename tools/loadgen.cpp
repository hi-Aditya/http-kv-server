#include <atomic>
#include <chrono>
#include <httplib.h>
#include <iostream>
#include <nlohmann/json.hpp>
#include <random>
#include <string>
#include <thread>
#include <vector>

using json = nlohmann::json;
using clk = std::chrono::steady_clock;

struct Stats {
  std::atomic<uint64_t> ok{0}, total{0};
  std::atomic<uint64_t> lat_ns{0};
};

int main(int argc, char **argv) {
  if (argc < 6) {
    std::cerr << "Usage: loadgen <host> <port> <threads> <duration_s> "
                 "<workload:get_popular|get_all|put_all|mix>\n";
    return 1;
  }
  std::string host = argv[1];
  int port = std::stoi(argv[2]);
  int threads = std::stoi(argv[3]);
  int duration = std::stoi(argv[4]);
  std::string workload = argv[5];

  httplib::Client cli(host, port);
  cli.set_connection_timeout(5, 0);
  cli.set_read_timeout(5, 0);
  cli.set_write_timeout(5, 0);

  Stats stats;
  std::atomic<bool> stop{false};

  auto worker = [&](int id) {
    std::mt19937_64 rng(id * 1337 + 1);
    std::uniform_int_distribution<uint64_t> dist(1, 1000000);
    std::uniform_int_distribution<int> pick(0, 99);
    std::vector<std::string> hot;
    for (int i = 0; i < 100; i++)
      hot.push_back("hot-" + std::to_string(i));

    while (!stop.load()) {
      std::string op;
      std::string key;
      std::string value = "v" + std::to_string(dist(rng));

      if (workload == "get_popular") {
        op = "GET";
        key = hot[dist(rng) % hot.size()];
      } else if (workload == "get_all") {
        op = "GET";
        key = "k" + std::to_string(dist(rng));
      } else if (workload == "put_all") {
        op = "PUT";
        key = "k" + std::to_string(dist(rng));
      } else { // mix: 70% GET, 30% PUT
        if (pick(rng) < 70) {
          op = "GET";
          key = hot[dist(rng) % hot.size()];
        } else {
          op = "PUT";
          key = "k" + std::to_string(dist(rng));
        }
      }

      auto t0 = clk::now();
      bool ok = false;
      if (op == "GET") {
        auto res = cli.Get(("/read?key=" + key).c_str());
        ok = res && (res->status == 200 || res->status == 404);
      } else {
        json j{{"key", key}, {"value", value}};
        auto res = cli.Put("/update", j.dump(), "application/json");
        ok = res && (res->status == 200);
      }
      auto dt =
          std::chrono::duration_cast<std::chrono::nanoseconds>(clk::now() - t0)
              .count();
      stats.total.fetch_add(1, std::memory_order_relaxed);
      stats.lat_ns.fetch_add(dt, std::memory_order_relaxed);
      if (ok)
        stats.ok.fetch_add(1, std::memory_order_relaxed);
    }
  };

  std::vector<std::thread> pool;
  pool.reserve(threads);
  auto start = clk::now();
  for (int i = 0; i < threads; i++)
    pool.emplace_back(worker, i);

  std::this_thread::sleep_for(std::chrono::seconds(duration));
  stop = true;
  for (auto &t : pool)
    t.join();
  auto end = clk::now();

  double sec = std::chrono::duration<double>(end - start).count();
  uint64_t total = stats.total.load();
  uint64_t ok = stats.ok.load();
  double throughput = total / sec;
  double avg_ms = (stats.lat_ns.load() / 1e6) / (total ? total : 1);

  std::cout << "threads=" << threads << " workload=" << workload
            << " duration_s=" << sec << " total=" << total << " ok=" << ok
            << " throughput_rps=" << throughput << " avg_latency_ms=" << avg_ms
            << std::endl;
  return 0;
}
