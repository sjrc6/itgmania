#include "CustomSongSnapshot.h"

#include <atomic>
#include <vector>

#include "MemoryCardManager.h"
#include "RageFile.h"
#include "RageFileManager.h"
#include "RageLog.h"
#include "RageUtil.h"
#include "Song.h"
#include "global.h"

namespace {
constexpr size_t COPY_BUFFER_BYTES = 64 * 1024;
std::atomic<unsigned> g_SnapshotSequence{0};

bool IsSafeRelativePath(const std::string& path) {
  return !path.empty() && path[0] != '/' && path != ".." &&
         path.find("../") == std::string::npos;
}
}  // namespace

CustomSongSnapshot::CustomSongSnapshot()
    : m_Song(nullptr),
      m_Player(PLAYER_INVALID),
      m_HasCardLease(false),
      m_MaximumBytes(0),
      m_Status(Idle),
      m_Cancelled(false),
      m_BytesCopied(0) {
  m_Thread.SetName("Custom song copy");
}

CustomSongSnapshot::~CustomSongSnapshot() { Reset(); }

bool CustomSongSnapshot::Begin(
    Song* song, PlayerNumber player, size_t maximumBytes) {
  Reset();
  if (song == nullptr || player == PLAYER_INVALID || maximumBytes == 0) {
    return false;
  }

  m_Song = song;
  m_Player = player;
  m_SourceDir = song->GetSourceSongDir();
  m_MaximumBytes = maximumBytes;
  m_Cancelled.store(false);
  m_BytesCopied.store(0);

  const unsigned sequence = ++g_SnapshotSequence;
  m_SnapshotDir = ssprintf(
      "/@mem/custom/%08x-%u/", GetHashForString(m_SourceDir), sequence);

  // Music selection also owns a lease.  This nested lease lets the copy
  // finish after that screen has transitioned away.
  m_HasCardLease = MEMCARDMAN->AcquireCardReadLease(player);
  if (!m_HasCardLease) {
    Fail("card lease could not be acquired");
    return false;
  }

  m_Status.store(Copying);
  m_Thread.Create(ThreadStart, this);
  return true;
}

void CustomSongSnapshot::Wait() {
  if (m_Thread.IsCreated()) {
    m_Thread.Wait();
  }
}

bool CustomSongSnapshot::Publish() {
  if (m_Status.load() != Ready || m_Song == nullptr) {
    return false;
  }
  if (m_Song->GetGameplaySnapshotDir() != m_SnapshotDir) {
    m_Song->SetGameplaySnapshotDir(m_SnapshotDir);
  }
  ReleaseCardLease();
  return true;
}

void CustomSongSnapshot::PreventSourceReads() {
  if (m_Song != nullptr && !m_SnapshotDir.empty() &&
      m_Song->GetGameplaySnapshotDir() != m_SnapshotDir) {
    // This directory is intentionally absent after a failed copy.  Publishing
    // its path makes subsequent loads fail locally instead of falling through
    // to removable media during gameplay.
    m_Song->SetGameplaySnapshotDir(m_SnapshotDir);
  }
  ReleaseCardLease();
}

void CustomSongSnapshot::Reset() {
  m_Cancelled.store(true);
  Wait();
  ReleaseCardLease();

  if (m_Song != nullptr && m_Song->GetGameplaySnapshotDir() == m_SnapshotDir) {
    m_Song->ClearGameplaySnapshotDir();
  }
  if (!m_SnapshotDir.empty()) {
    FILEMAN->DeleteRecursive(m_SnapshotDir);
  }

  m_Song = nullptr;
  m_Player = PLAYER_INVALID;
  m_SourceDir.clear();
  m_SnapshotDir.clear();
  m_BytesCopied.store(0);
  m_Status.store(Idle);
}

int CustomSongSnapshot::ThreadStart(void* data) {
  static_cast<CustomSongSnapshot*>(data)->Run();
  return 0;
}

void CustomSongSnapshot::Run() {
  const std::vector<std::string>& files = m_Song->GetCustomSongGameplayFiles();
  if (files.empty()) {
    Fail("no gameplay files were identified");
    return;
  }

  for (const std::string& file : files) {
    if (m_Cancelled.load() || !CopyOne(file)) {
      Fail(
          m_Cancelled.load() ? "copy cancelled"
                             : "a gameplay file could not be copied");
      return;
    }
  }

  for (const std::string& file : m_Song->GetCustomSongOptionalGameplayFiles()) {
    if (m_Cancelled.load()) {
      Fail("copy cancelled");
      return;
    }
    if (!CopyOne(file)) {
      if (m_Cancelled.load()) {
        Fail("copy cancelled");
        return;
      }
      LOG->Warn("Skipping optional custom song asset: %s", file.c_str());
    }
  }

  m_Status.store(Ready);
  LOG->Info(
      "Custom song copy ready at %s (%zu bytes)", m_SnapshotDir.c_str(),
      m_BytesCopied.load());
}

bool CustomSongSnapshot::CopyOne(const std::string& relativePath) {
  if (!IsSafeRelativePath(relativePath)) {
    return false;
  }

  RageFile source;
  if (!source.Open(m_SourceDir + relativePath, RageFile::READ)) {
    return false;
  }
  const int fileSize = source.GetFileSize();
  const size_t copiedBefore = m_BytesCopied.load();
  if (fileSize < 0 || copiedBefore > m_MaximumBytes ||
      static_cast<size_t>(fileSize) > m_MaximumBytes - copiedBefore) {
    return false;
  }

  RageFile destination;
  if (!destination.Open(m_SnapshotDir + relativePath, RageFile::WRITE)) {
    return false;
  }
  const auto discardDestination = [this, &destination, &relativePath]() {
    destination.Close();
    FILEMAN->Remove(m_SnapshotDir + relativePath);
    return false;
  };

  char buffer[COPY_BUFFER_BYTES];
  size_t copied = 0;
  while (!source.AtEOF()) {
    if (m_Cancelled.load()) {
      return discardDestination();
    }
    const int bytesRead = source.Read(buffer, sizeof(buffer));
    if (bytesRead < 0) {
      return discardDestination();
    }
    if (bytesRead == 0) {
      break;
    }
    if (destination.Write(buffer, bytesRead) != bytesRead) {
      return discardDestination();
    }
    copied += bytesRead;
  }

  if (!source.GetError().empty() || !destination.GetError().empty() ||
      copied != static_cast<size_t>(fileSize)) {
    return discardDestination();
  }
  m_BytesCopied.fetch_add(copied);
  return true;
}

void CustomSongSnapshot::Fail(const std::string& error) {
  if (!m_SnapshotDir.empty()) {
    FILEMAN->DeleteRecursive(m_SnapshotDir);
  }
  m_Status.store(Failed);
  LOG->Warn("Custom song copy failed: %s", error.c_str());
}

void CustomSongSnapshot::ReleaseCardLease() {
  if (!m_HasCardLease) {
    return;
  }
  MEMCARDMAN->ReleaseCardReadLease(m_Player);
  m_HasCardLease = false;
}
