#ifndef CUSTOM_SONG_SNAPSHOT_H
#define CUSTOM_SONG_SNAPSHOT_H

#include <atomic>
#include <cstddef>
#include <string>

#include "PlayerNumber.h"
#include "RageThreads.h"

class Song;

// Builds the private in-memory copy used after leaving music selection.  The
// Song is not redirected until the complete copy is ready.
class CustomSongSnapshot {
 public:
  enum Status { Idle, Copying, Ready, Failed };

  CustomSongSnapshot();
  ~CustomSongSnapshot();

  bool Begin(Song* song, PlayerNumber player, size_t maximumBytes);
  void Wait();
  bool Publish();
  void PreventSourceReads();
  bool MatchesSong(const Song* song) const { return m_Song == song; }
  void Reset();

  Status GetStatus() const { return m_Status.load(); }

 private:
  static int ThreadStart(void* data);
  void Run();
  bool CopyOne(const std::string& relativePath);
  void Fail(const std::string& error);
  void ReleaseCardLease();

  Song* m_Song;
  PlayerNumber m_Player;
  std::string m_SourceDir;
  std::string m_SnapshotDir;
  bool m_HasCardLease;
  size_t m_MaximumBytes;

  std::atomic<Status> m_Status;
  std::atomic<bool> m_Cancelled;
  std::atomic<size_t> m_BytesCopied;
  RageThread m_Thread;
};

#endif
