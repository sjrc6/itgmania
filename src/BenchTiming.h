#ifndef BENCH_TIMING_H
#define BENCH_TIMING_H

// Benchmark-only instrumentation for USB custom song loading. Not for upstream.
//
// Environment variables:
//   ITGMANIA_BENCH_CSV        per-song timing rows are appended here
//   ITGMANIA_BENCH_EVENTS     per-profile-load timing rows are appended here
//   ITGMANIA_BENCH_BLOCKSTAT  /sys/block/<dev>/stat of the card, for I/O counts
//   ITGMANIA_BENCH_TAG        free-form label copied into every row

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <mutex>
#include <string>

namespace bench {

enum Phase {
  NOTES,
  EDITS,
  TIDY,
  MUSIC_PROBE,
  PREVIEW_PROBE,
  STATS,
  SD_NOTEDATA,
  SD_RADAR,
  SD_TECH,
  SD_MEASURE,
  SD_HASH,
  MUSIC_CHECK,
  COMPRESS,
  NUM_PHASES
};

inline const char* PhaseName(int p) {
  static const char* names[NUM_PHASES] = {
      "notes",    "edits",    "tidy",       "music_probe", "preview_probe",
      "stats",    "sd_notedata", "sd_radar", "sd_tech",    "sd_measure",
      "sd_hash",  "music_check", "compress"};
  return names[p];
}

struct Counters {
  double wall = 0;
  double cpu = 0;
  uint64_t ios = 0;
  uint64_t sectors = 0;
};

struct SongTimes {
  Counters phase[NUM_PHASES];
};

inline SongTimes*& Current() {
  thread_local SongTimes* current = nullptr;
  return current;
}

inline double WallClock() {
  return std::chrono::duration<double>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// Thread CPU time in seconds (not measured on Windows).
inline double ThreadCpuClock() {
#ifdef _WIN32
  return 0;
#else
  timespec ts;
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
  return double(ts.tv_sec) + double(ts.tv_nsec) * 1e-9;
#endif
}

struct Snapshot {
  double wall = 0;
  double cpu = 0;
  uint64_t ios = 0;
  uint64_t sectors = 0;

  void Take() {
    static const char* path = std::getenv("ITGMANIA_BENCH_BLOCKSTAT");
    wall = WallClock();
    cpu = ThreadCpuClock();
    if (path == nullptr) {
      return;
    }
    if (FILE* f = std::fopen(path, "r")) {
      unsigned long long reads = 0, merged = 0, sects = 0;
      if (std::fscanf(f, "%llu %llu %llu", &reads, &merged, &sects) == 3) {
        ios = reads;
        sectors = sects;
      }
      std::fclose(f);
    }
  }
};

inline void Add(Counters& c, const Snapshot& s, const Snapshot& e) {
  c.wall += e.wall - s.wall;
  c.cpu += e.cpu - s.cpu;
  c.ios += e.ios - s.ios;
  c.sectors += e.sectors - s.sectors;
}

// Accumulates into the current song's phase; does nothing outside a song.
class Scope {
 public:
  explicit Scope(Phase p) : phase_(p), times_(Current()) {
    if (times_ != nullptr) {
      start_.Take();
    }
  }
  ~Scope() {
    if (times_ != nullptr) {
      Snapshot end;
      end.Take();
      Add(times_->phase[phase_], start_, end);
    }
  }
  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;

 private:
  Phase phase_;
  SongTimes* times_;
  Snapshot start_;
};

template <typename F>
auto Timed(Phase p, F&& f) {
  Scope scope(p);
  return f();
}

// Memory card access timeout for profile loads, or 0 for the engine default.
inline int MountTimeout() {
  const char* v = std::getenv("ITGMANIA_BENCH_MOUNT_TIMEOUT");
  return v != nullptr ? std::atoi(v) : 0;
}

inline FILE* OpenCsv(const char* env, const std::string& header) {
  const char* path = std::getenv(env);
  if (path == nullptr) {
    return nullptr;
  }
  FILE* f = std::fopen(path, "a");
  if (f != nullptr && std::ftell(f) == 0) {
    std::fprintf(f, "%s\n", header.c_str());
  }
  return f;
}

inline std::string Tag() {
  const char* tag = std::getenv("ITGMANIA_BENCH_TAG");
  return tag != nullptr ? tag : "";
}

inline std::string Quote(const std::string& s) {
  std::string out = "\"";
  for (char c : s) {
    out += c == '"' ? std::string("\"\"") : std::string(1, c);
  }
  return out + "\"";
}

// One row per song load attempt.
inline void SongRow(
    const std::string& song_dir, bool ok, int n_steps, double music_len,
    const std::string& music_file, const Snapshot& s, const Snapshot& e,
    const SongTimes& t) {
  static FILE* f = [] {
    std::string header =
        "tag,song_dir,ok,n_steps,music_len,music_file,total_wall,total_cpu,"
        "total_ios,total_sectors";
    for (int p = 0; p < NUM_PHASES; ++p) {
      std::string n = PhaseName(p);
      header += "," + n + "_wall," + n + "_cpu," + n + "_ios," + n + "_sectors";
    }
    return OpenCsv("ITGMANIA_BENCH_CSV", header);
  }();
  if (f == nullptr) {
    return;
  }
  std::fprintf(
      f, "%s,%s,%d,%d,%.3f,%s,%.6f,%.6f,%llu,%llu", Quote(Tag()).c_str(),
      Quote(song_dir).c_str(), ok ? 1 : 0, n_steps, music_len,
      Quote(music_file).c_str(), e.wall - s.wall, e.cpu - s.cpu,
      (unsigned long long)(e.ios - s.ios),
      (unsigned long long)(e.sectors - s.sectors));
  for (const Counters& c : t.phase) {
    std::fprintf(
        f, ",%.6f,%.6f,%llu,%llu", c.wall, c.cpu, (unsigned long long)c.ios,
        (unsigned long long)c.sectors);
  }
  std::fprintf(f, "\n");
  std::fflush(f);
}

// One row per named event (mount, profile load, song folder load, unmount).
inline void EventRow(
    const char* name, int player, const Snapshot& s, const Snapshot& e,
    long long count = -1) {
  static FILE* f = OpenCsv(
      "ITGMANIA_BENCH_EVENTS", "tag,event,player,wall,cpu,ios,sectors,count");
  if (f == nullptr) {
    return;
  }
  std::fprintf(
      f, "%s,%s,%d,%.6f,%.6f,%llu,%llu,%lld\n", Quote(Tag()).c_str(), name,
      player, e.wall - s.wall, e.cpu - s.cpu,
      (unsigned long long)(e.ios - s.ios),
      (unsigned long long)(e.sectors - s.sectors), count);
  std::fflush(f);
}

// One row per chart analysed (any song, any thread), when ITGMANIA_BENCH_CHARTS
// is set. Times are thread CPU seconds.
inline bool ChartsEnabled() {
  static const bool enabled = std::getenv("ITGMANIA_BENCH_CHARTS") != nullptr;
  return enabled;
}

inline void ChartRow(
    const std::string& key, const std::string& steps_type, int taps,
    float last_beat, double notedata, double tech, double hash) {
  static std::mutex mutex;
  static FILE* f = OpenCsv(
      "ITGMANIA_BENCH_CHARTS",
      "tag,key,steps_type,taps,last_beat,notedata_s,tech_s,hash_s");
  if (f == nullptr) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex);
  std::fprintf(
      f, "%s,%s,%s,%d,%.3f,%.9f,%.9f,%.9f\n", Quote(Tag()).c_str(),
      Quote(key).c_str(), steps_type.c_str(), taps, last_beat, notedata, tech,
      hash);
}

}  // namespace bench

#endif  // BENCH_TIMING_H
