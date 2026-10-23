// Benchmark-only: a differential stress test of memory card file access.
//
// With ITGMANIA_BENCH_STRESS=<seconds>, ITGMANIA_BENCH_STRESS_THREADS threads
// (default 8) open random files on P1's card and do random reads, seeks,
// copies, GetLine()s and directory listings through /@mc1/ (the timeout
// driver), repeat each one on /@mc1int/ (the plain directory driver under it)
// and compare the results. They also write files through /@mc1/ and check them
// through the OS.
//
// With ITGMANIA_BENCH_STRESS_COMPARE=0 nothing is compared, for stall tests,
// where reference reads would block on the stalled device. The summary, with
// per-second counts of good and failed file accesses, is written to
// ITGMANIA_BENCH_STRESS_OUT.
#pragma once

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <filesystem>
#include <system_error>

#include "MemoryCardManager.h"
#include "RageFile.h"
#include "RageFileManager.h"
#include "RageLog.h"
#include "RageTimer.h"
#include "RageUtil.h"

namespace bench_stress {

struct Stats {
  std::atomic<long long> files{0}, ops{0}, bytes{0}, errors{0}, mismatches{0},
      copies{0}, lines{0}, listings{0}, writes{0};
  std::mutex m;
  std::vector<std::pair<int, int>> per_second;  // good, failed

  void Tick(double t, bool ok) {
    std::lock_guard<std::mutex> l(m);
    const size_t s = size_t(t);
    if (per_second.size() <= s) {
      per_second.resize(s + 1);
    }
    (ok ? per_second[s].first : per_second[s].second)++;
  }
};

// Logs the first few mismatches of each kind ("read", "eof", "tell", ...).
inline void Mismatch(Stats& st, const std::string& what) {
  ++st.mismatches;
  std::string kind = what.substr(what.rfind(" op ") == std::string::npos ? 0 : what.rfind(" op "));
  kind.erase(std::remove_if(kind.begin(), kind.end(), [](char c) { return c >= '0' && c <= '9'; }), kind.end());
  static std::mutex m;
  static std::map<std::string, int> seen;
  std::lock_guard<std::mutex> l(m);
  if (++seen[kind] <= 5) {
    LOG->Warn("STRESS MISMATCH: %s", what.c_str());
  }
}

inline void Error(Stats& st, const std::string& what) {
  if (++st.errors <= 20) {
    LOG->Warn("STRESS ERROR: %s", what.c_str());
  }
}

inline std::string Ref(const std::string& path) {
  return "/@mc1int/" + path.substr(std::strlen("/@mc1/"));
}

inline void List(
    const std::string& dir, std::vector<std::string>& files,
    std::vector<std::string>& dirs) {
  std::vector<std::string> all, subdirs;
  FILEMAN->GetDirListing(dir + "*", all, false, true);
  FILEMAN->GetDirListing(dir + "*", subdirs, true, true);
  std::sort(subdirs.begin(), subdirs.end());
  for (const std::string& p : all) {
    if (!std::binary_search(subdirs.begin(), subdirs.end(), p)) {
      files.push_back(p);
    }
  }
  for (const std::string& d : subdirs) {
    dirs.push_back(d + "/");
    List(d + "/", files, dirs);
  }
}

inline void Worker(
    int id, unsigned seed, double seconds, bool compare,
    const std::string& os_root, const std::vector<std::string>& files,
    const std::vector<std::string>& dirs, const RageTimer& start, Stats& st) {
  std::mt19937 rng(seed * 7919u + unsigned(id));
  auto rnd = [&](size_t n) { return n == 0 ? 0 : int(rng() % n); };
  int write_count = 0;

  while (start.Ago() < seconds) {
    const double t = start.Ago();
    const int choice = rnd(100);

    if (choice < 3) {
      const std::string& dir = dirs[rnd(dirs.size())];
      std::vector<std::string> a, b;
      FILEMAN->GetDirListing(dir + "*", a, false, false);
      st.listings++;
      if (compare) {
        FILEMAN->GetDirListing(Ref(dir) + "*", b, false, false);
        std::sort(a.begin(), a.end());
        std::sort(b.begin(), b.end());
        if (a != b) {
          Mismatch(st, "listing " + dir);
        }
      }
      continue;
    }

    if (choice < 5) {
      // Written files go in their own directory, which isn't listed, since
      // the timeout driver's directory cache never expires.
      const std::string rel = "stress-tmp/" + std::to_string(id) + "-" +
                              std::to_string(write_count++) + ".bin";
      std::string data(rnd(300000), '\0');
      for (char& c : data) {
        c = char(rng());
      }
      bool ok;
      std::string error;
      {
        RageFile w;
        ok = w.Open("/@mc1/" + rel, RageFile::WRITE);
        if (!ok) {
          error = "open: " + w.GetError();
        }
        size_t done = 0;
        while (ok && done < data.size()) {
          const size_t n = std::min(data.size() - done, size_t(1 + rnd(100000)));
          ok = w.Write(data.data() + done, n) == int(n);
          done += n;
        }
        if (ok) {
          ok = w.Flush() != -1;
        }
        if (!ok && error.empty()) {
          error = w.GetError();
        }
      }
      st.writes++;
      st.Tick(t, ok);
      if (!ok) {
        Error(st, "write " + rel + ": " + error);
        continue;
      }
      const std::string os_path = os_root + "/" + rel;
      if (compare) {
        std::ifstream f(os_path, std::ios::binary);
        const std::string back(
            (std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (back != data) {
          Mismatch(st, "write " + rel);
        }
      }
      std::remove(os_path.c_str());
      continue;
    }

    const std::string& path = files[rnd(files.size())];
    RageFile a, b;
    st.files++;
    if (!a.Open(path)) {
      Error(st, "open " + path + ": " + a.GetError());
      st.Tick(t, false);
      continue;
    }
    if (compare && !b.Open(Ref(path))) {
      Mismatch(st, "reference open " + path);
      continue;
    }
    const int size = a.GetFileSize();
    if (compare && size != b.GetFileSize()) {
      Mismatch(st, "size " + path);
    }

    std::unique_ptr<RageFile> ac, bc;
    bool ok = true;
    const int op_count = 1 + rnd(24);
    for (int k = 0; k < op_count && ok; ++k) {
      const bool use_copy = ac != nullptr && rnd(2) == 1;
      RageFile& x = use_copy ? *ac : a;
      RageFile* y = compare ? (use_copy ? bc.get() : &b) : nullptr;
      const std::string where =
          path + " op " + std::to_string(k) + (use_copy ? " (copy)" : "");
      const int op = rnd(11);

      if (op < 2) {
        const int target = rnd(size + 2000);
        const int rx = x.Seek(target);
        if (y != nullptr && rx != y->Seek(target)) {
          Mismatch(st, where + " seek");
        }
      } else if (op < 3) {
        const int whence = rnd(2) == 1 ? SEEK_CUR : SEEK_END;
        int offset = whence == SEEK_CUR
                         ? rnd(20000) - 10000
                         : -rnd(std::min(size, 100000) + 1);
        if (whence == SEEK_CUR && x.Tell() + offset < 0) {
          offset = -x.Tell();
        }
        const int rx = x.Seek(offset, whence);
        if (y != nullptr && rx != y->Seek(offset, whence)) {
          Mismatch(st, where + " relative seek");
        }
      } else if (op < 7) {
        static const int kMax[] = {16, 4096, 70000, 300000, 2500000};
        const int len = 1 + rnd(kMax[rnd(5)]);
        std::string bx(len, '\0'), by(len, '\0');
        const int gx = x.Read(&bx[0], len);
        if (gx > 0) {
          st.bytes += gx;
        }
        if (y != nullptr) {
          const int gy = y->Read(&by[0], len);
          if (gx != gy || (gx > 0 && std::memcmp(bx.data(), by.data(), gx))) {
            Mismatch(
                st, where + " read " + std::to_string(len) + " got " +
                        std::to_string(gx) + " expected " + std::to_string(gy));
          }
        }
        ok = gx >= 0;
      } else if (op < 8) {
        std::string lx, ly;
        const int rx = x.GetLine(lx);
        st.lines++;
        if (y != nullptr && (rx != y->GetLine(ly) || lx != ly)) {
          Mismatch(st, where + " getline");
        }
        ok = rx >= 0;
      } else if (op < 9) {
        std::string sx, sy;
        const int rx = x.Read(sx);
        if (y != nullptr && (rx != y->Read(sy) || sx != sy)) {
          Mismatch(st, where + " read rest");
        }
        ok = rx >= 0;
      } else if (op < 10) {
        // A copy continues from the same position.  Make the reference copy
        // by reopening and seeking, since the plain driver's own Copy() loses
        // buffered data.
        // x may be the copy being replaced, so don't use it afterwards.
        const int pos = x.Tell();
        auto copy = std::make_unique<RageFile>(x);
        if (compare) {
          bc = std::make_unique<RageFile>();
          if (!bc->Open(Ref(path))) {
            Mismatch(st, where + " reference reopen");
          }
          bc->Seek(pos);
        }
        ac = std::move(copy);
        st.copies++;
        st.ops++;
        if (compare && ac->Tell() != bc->Tell()) {
          Mismatch(st, where + " copy tell");
        }
        continue;
      } else if (y != nullptr && !use_copy && x.AtEOF() != y->AtEOF()) {
        // (A copy keeps the original's EOF flag; the reference copy doesn't.)
        Mismatch(st, where + " eof");
      }

      st.ops++;
      if (y != nullptr && x.Tell() != y->Tell()) {
        Mismatch(
            st, where + " tell " + std::to_string(x.Tell()) + " expected " +
                    std::to_string(y->Tell()));
      }
    }
    if (!ok) {
      Error(st, path + ": " + a.GetError() + " / " + (ac ? ac->GetError() : ""));
    }
    st.Tick(t, ok);
  }
}

// Returns a summary of the run.
inline std::string Run(double seconds) {
  const char* threads_env = std::getenv("ITGMANIA_BENCH_STRESS_THREADS");
  const int thread_count = threads_env != nullptr ? std::atoi(threads_env) : 8;
  const char* compare_env = std::getenv("ITGMANIA_BENCH_STRESS_COMPARE");
  const bool compare = compare_env == nullptr || std::atoi(compare_env) != 0;
  const char* seed_env = std::getenv("ITGMANIA_BENCH_STRESS_SEED");
  const unsigned seed = seed_env != nullptr ? unsigned(std::atoi(seed_env)) : 1;
  std::string os_root =
      MemoryCardManager::m_sMemoryCardOsMountPoint[PLAYER_1].Get();
  while (!os_root.empty() && (os_root.back() == '/' || os_root.back() == '\\')) {
    os_root.pop_back();
  }

  std::error_code ec;
  std::filesystem::create_directories(os_root + "/stress-tmp", ec);
  std::vector<std::string> files, dirs = {"/@mc1/"};
  List("/@mc1/", files, dirs);
  dirs.erase(
      std::remove(dirs.begin(), dirs.end(), "/@mc1/stress-tmp/"), dirs.end());
  files.erase(
      std::remove_if(
          files.begin(), files.end(),
          [](const std::string& f) { return f.rfind("/@mc1/stress-tmp/", 0) == 0; }),
      files.end());
  if (files.empty()) {
    return "stress: no files on the card";
  }

  Stats st;
  RageTimer start;
  std::vector<std::thread> threads;
  for (int i = 0; i < thread_count; ++i) {
    threads.emplace_back(
        Worker, i, seed, seconds, compare, std::cref(os_root), std::cref(files),
        std::cref(dirs), std::cref(start), std::ref(st));
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
  std::filesystem::remove(os_root + "/stress-tmp", ec);

  std::string out = ssprintf(
      "stress: %d threads, %.1fs, compare=%d, %zu files, %zu dirs: "
      "%lld file opens, %lld ops, %.1f MB read, %lld copies, %lld getlines, "
      "%lld listings, %lld writes, %lld errors, %lld MISMATCHES\n",
      thread_count, start.Ago(), int(compare), files.size(), dirs.size(),
      st.files.load(), st.ops.load(), st.bytes.load() / 1e6,
      st.copies.load(), st.lines.load(), st.listings.load(), st.writes.load(),
      st.errors.load(), st.mismatches.load());
  out += "per second (good/failed):";
  for (size_t s = 0; s < st.per_second.size(); ++s) {
    out += ssprintf(
        " %zu:%d/%d", s, st.per_second[s].first, st.per_second[s].second);
  }
  out += "\n";
  return out;
}

}  // namespace bench_stress
