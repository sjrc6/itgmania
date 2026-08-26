#ifndef CUSTOM_SONG_SNAPSHOT_H
#define CUSTOM_SONG_SNAPSHOT_H

#include <atomic>
#include <cstddef>
#include <string>
#include <vector>

#include "PlayerNumber.h"
#include "RageThreads.h"

class Song;

/**
 * Owns the temporary, gameplay-safe copy of one removable-media song.
 *
 * The copy is built on a worker thread in a unique /@mem namespace.  The Song
 * is pointed at it only after every required file has been copied, so callers
 * can never observe a partially populated gameplay directory.
 */
class CustomSongSnapshot {
 public:
  enum Status { Idle, Copying, Ready, Failed };

  CustomSongSnapshot();
  ~CustomSongSnapshot();

  bool Begin(
      Song* song, PlayerNumber player, const std::string& deviceIdentity,
      bool includeStaticBackgrounds, bool includeLua, size_t maximumBytes);
  void Cancel();
  void Wait();
  bool Publish();
  void Unpublish();
  bool MatchesSong(const Song* song) const { return m_Song == song; }
  void Reset();

  Status GetStatus() const { return m_Status.load(); }
  std::string GetError() const;
  size_t GetBytesCopied() const { return m_BytesCopied.load(); }
  const std::string& GetSnapshotDir() const { return m_SnapshotDir; }

 private:
  static int ThreadStart(void* data);
  void Run();
  bool CollectFiles(std::vector<std::string>& relativeFiles);
  bool CollectDirectory(
      const std::string& relativeDir, int depth,
      std::vector<std::string>& relativeFiles);
  bool CopyOne(const std::string& relativePath);
  void Fail(const std::string& error);
  void ReleaseCardLease();

  Song* m_Song;
  PlayerNumber m_Player;
  std::string m_DeviceIdentity;
  std::string m_SourceDir;
  std::string m_SnapshotDir;
  bool m_IncludeStaticBackgrounds;
  bool m_IncludeLua;
  bool m_HasCardLease;
  size_t m_MaximumBytes;

  std::atomic<Status> m_Status;
  std::atomic<bool> m_Cancelled;
  std::atomic<size_t> m_BytesCopied;
  mutable RageMutex m_Mutex;
  std::string m_Error;
  RageThread m_Thread;
};

#endif
