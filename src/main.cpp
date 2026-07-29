// SPDX-License-Identifier: MIT
// cpp-softirq-watch — Linux softirq distribution monitor
// Reads /proc/softirqs and displays per-CPU distribution of software interrupts.
//
// Build: g++ -std=c++23 -O2 -Wall -Wextra src/main.cpp -o softirq-watch
// Usage: ./softirq-watch          # one-shot display
//        ./softirq-watch --watch  # refresh every 1 second
//        ./softirq-watch -w 2     # refresh every 2 seconds

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <print>
#include <ranges>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// ── Softirq names in order as they appear in /proc/softirqs ──
static constexpr std::string_view kSoftirqNames[] = {
  "HI", "TIMER", "NET_TX", "NET_RX", "BLOCK", "IRQ_POLL",
  "TASKLET", "SCHED", "HRTIMER", "RCU"
};

// ── Forward declarations ──
struct SoftirqSnapshot;
SoftirqSnapshot read_softirqs();
void print_table(const SoftirqSnapshot& snap, bool header);
std::string fmt_count(unsigned long long n);
std::string fmt_pct(double pct);

// ── Data model ──
struct SoftirqSnapshot {
  std::vector<std::string> cpu_labels;       // CPU0, CPU1, ...
  std::vector<unsigned long long> cpu_total; // total softirqs per CPU
  std::vector<std::string> names;            // softirq names
  std::vector<std::vector<unsigned long long>> counts; // [name_idx][cpu_idx]
  unsigned long long grand_total = 0;        // total across all CPUs and types
};

// ── Parse /proc/softirqs ──
SoftirqSnapshot read_softirqs() {
  SoftirqSnapshot snap;
  std::ifstream file("/proc/softirqs");
  if (!file.is_open()) {
    std::println(std::cerr, "Error: cannot open /proc/softirqs");
    return snap;
  }

  std::string line;
  // First line: header with CPU labels
  if (!std::getline(file, line)) return snap;

  std::istringstream hdr(line);
  std::string label;
  while (hdr >> label) {
    snap.cpu_labels.push_back(label);
    snap.cpu_total.push_back(0);
  }

  size_t num_cpus = snap.cpu_labels.size();
  if (num_cpus == 0) return snap;

  // Remaining lines: name + per-CPU counts
  while (std::getline(file, line)) {
    if (line.empty()) continue;

    std::istringstream iss(line);
    std::string name;
    if (!(iss >> name)) continue;

    // Remove trailing colon from the name
    if (!name.empty() && name.back() == ':') name.pop_back();
    snap.names.push_back(name);

    std::vector<unsigned long long> cpu_counts(num_cpus, 0);
    for (size_t i = 0; i < num_cpus; ++i) {
      unsigned long long val = 0;
      iss >> val;
      cpu_counts[i] = val;
      snap.cpu_total[i] += val;
      snap.grand_total += val;
    }
    snap.counts.push_back(std::move(cpu_counts));
  }

  return snap;
}

// ── Format a number with thousands separator ──
std::string fmt_count(unsigned long long n) {
  std::string s = std::to_string(n);
  std::string result;
  int count = 0;
  for (auto it = s.rbegin(); it != s.rend(); ++it) {
    if (count > 0 && count % 3 == 0) result.push_back(',');
    result.push_back(*it);
    ++count;
  }
  std::ranges::reverse(result);
  return result;
}

// ── Format a percentage string ──
std::string fmt_pct(double pct) {
  char buf[8];
  std::snprintf(buf, sizeof(buf), "%5.1f%%", pct);
  return buf;
}

// ── Render the table ──
void print_table(const SoftirqSnapshot& snap, bool show_header) {
  size_t num_cpus = snap.cpu_labels.size();
  size_t num_types = snap.names.size();

  if (num_cpus == 0 || num_types == 0) {
    std::println(std::cout, "No data available from /proc/softirqs");
    return;
  }

  // Column widths
  const int name_width = 10;
  const int count_width = 14;
  const int pct_width = 8;

  // Header
  if (show_header) {
    std::print("{:<{}}", "TYPE", name_width);
    for (size_t c = 0; c < num_cpus; ++c) {
      std::print("{:>{}}", snap.cpu_labels[c], count_width);
    }
    std::print("{:>{}}", "TOTAL", count_width);
    std::print("{:>{}}", "%", pct_width);
    std::println(std::cout);
    std::println(std::cout, "{}", std::string(name_width + (count_width + 1) * num_cpus + count_width + pct_width, '='));
  }

  // Rows
  for (size_t i = 0; i < num_types; ++i) {
    unsigned long long type_total = 0;
    for (size_t c = 0; c < num_cpus; ++c) {
      type_total += snap.counts[i][c];
    }

    double pct_of_grand = (snap.grand_total > 0)
      ? (static_cast<double>(type_total) / snap.grand_total * 100.0)
      : 0.0;

    std::print("{:<{}}", snap.names[i], name_width);
    for (size_t c = 0; c < num_cpus; ++c) {
      std::print("{:>{}}", fmt_count(snap.counts[i][c]), count_width);
    }
    std::print("{:>{}}", fmt_count(type_total), count_width);
    std::print("{:>{}}", fmt_pct(pct_of_grand), pct_width);
    std::println(std::cout);
  }

  // Footer: per-CPU totals
  std::println(std::cout, "{}", std::string(name_width + (count_width + 1) * num_cpus + count_width + pct_width, '='));
  std::print("{:<{}}", "TOTAL", name_width);
  for (size_t c = 0; c < num_cpus; ++c) {
    std::print("{:>{}}", fmt_count(snap.cpu_total[c]), count_width);
  }
  std::print("{:>{}}", fmt_count(snap.grand_total), count_width);
  std::println(std::cout);

  // Balance metric: coefficient of variation across CPUs for total counts
  if (num_cpus > 1 && snap.grand_total > 0) {
    double mean = static_cast<double>(snap.grand_total) / num_cpus;
    double variance = 0.0;
    for (size_t c = 0; c < num_cpus; ++c) {
      double diff = static_cast<double>(snap.cpu_total[c]) - mean;
      variance += diff * diff;
    }
    variance /= num_cpus;
    double stddev = std::sqrt(variance);
    double cv = (mean > 0) ? (stddev / mean) : 0.0;
    std::println(std::cout);
    std::println(std::cout, "  Balance (CV): {:.4f}  ({})", cv,
      (cv < 0.1) ? "well balanced" :
      (cv < 0.3) ? "moderately balanced" : "unbalanced");
  }

  std::println(std::cout);
}

// ── Entry point ──
int main(int argc, char** argv) {
  bool watch_mode = false;
  int interval = 1; // seconds

  for (int i = 1; i < argc; ++i) {
    std::string_view arg{argv[i]};
    if (arg == "--watch" || arg == "-w") {
      watch_mode = true;
      if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9') {
        interval = std::atoi(argv[++i]);
        if (interval < 1) interval = 1;
      }
    } else if (arg == "--help" || arg == "-h") {
      std::println(std::cout, "softirq-watch — Linux softirq distribution monitor");
      std::println(std::cout);
      std::println(std::cout, "  Usage: {} [options]", argv[0]);
      std::println(std::cout);
      std::println(std::cout);
      std::println(std::cout, "  Options:");
      std::println(std::cout, "    -w, --watch [N]  Refresh every N seconds (default: 1)");
      std::println(std::cout, "    -h, --help       Show this help");
      return 0;
    }
  }

  if (watch_mode) {
    int iteration = 0;
    while (true) {
      auto snap = read_softirqs();
      std::println(std::cout, "  /proc/softirqs — iteration {}  (refresh every {}s)\n",
        ++iteration, interval);
      print_table(snap, true);
      std::this_thread::sleep_for(std::chrono::seconds(interval));
      std::println(std::cout, "\x1b[{}A", 30); // move cursor up to overwrite
    }
  } else {
    auto snap = read_softirqs();
    std::println(std::cout, "  /proc/softirqs — Linux Softirq Distribution\n");
    print_table(snap, true);
  }

  return 0;
}
