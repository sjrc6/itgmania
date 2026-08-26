#include "CustomSongSnapshot.h"

#include <algorithm>
#include <atomic>
#include <set>
#include <vector>

#include "ActorUtil.h"
#include "MemoryCardManager.h"
#include "RageFile.h"
#include "RageFileManager.h"
#include "RageLog.h"
#include "RageUtil.h"
#include "Song.h"
#include "Steps.h"
#include "global.h"

namespace {
constexpr size_t MAX_SNAPSHOT_FILES = 4096;
constexpr int MAX_SNAPSHOT_DEPTH = 16;
constexpr size_t COPY_BUFFER_BYTES = 64 * 1024;
std::atomic<unsigned> g_SnapshotSequence{0};

bool IsSafeRelativePath(const std::string& path) {
  return !path.empty() && path[0] != '/' &&
         path.find("../") == std::string::npos && path != "..";
}
}  // namespace

CustomSongSnapshot::CustomSongSnapshot()
    : m_Song(nullptr),
      m_Player(PLAYER_INVALID),
      m_IncludeStaticBackgrounds(false),
      m_IncludeLua(false),
      m_HasCardLease(false),
      m_MaximumBytes(0),
      m_Status(Idle),
      m_Cancelled(false),
      m_BytesCopied(0),
      m_Mutex("CustomSongSnapshot") {
  m_Thread.SetName("Custom song snapshot");
}

CustomSongSnapshot::~CustomSongSnapshot() { Reset(); }

bool CustomSongSnapshot::Begin(
    Song* song, PlayerNumber player, const std::string& deviceIdentity,
    bool includeStaticBackgrounds, bool includeLua, size_t maximumBytes) {
  Reset();
  if (song == nullptr || player == PLAYER_INVALID || deviceIdentity.empty() ||
      maximumBytes == 0) {
    return false;
  }

  m_Song = song;
  m_Player = player;
  m_DeviceIdentity = deviceIdentity;
  m_SourceDir = song->GetSourceSongDir();
  m_IncludeStaticBackgrounds = includeStaticBackgrounds;
  m_IncludeLua = includeLua;
  m_MaximumBytes = maximumBytes;
  m_Cancelled.store(false);
  m_BytesCopied.store(0);

  // MemoryCardManager owns main-thread lifecycle state.  Acquire its nested
  // lease before starting the copier, and let the worker only perform file
  // I/O through the already-mounted timeout driver.
  m_HasCardLease = MEMCARDMAN->AcquireCardReadLease(player);
  if (!m_HasCardLease) {
    return false;
  }

  const unsigned sequence = ++g_SnapshotSequence;
  const std::string session =
      ssprintf("%08x-%u", GetHashForString(m_SourceDir), sequence);
  const std::string device = ssprintf("%08x", GetHashForString(deviceIdentity));
  const std::string songId = ssprintf(
      "%08x", GetHashForString(m_SourceDir + song->GetTranslitFullTitle()));
  m_SnapshotDir = "/@mem/custom/" + session + "/" + device + "/" + songId + "/";

  m_Status.store(Copying);
  m_Thread.Create(ThreadStart, this);
  return true;
}

void CustomSongSnapshot::Cancel() { m_Cancelled.store(true); }

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

void CustomSongSnapshot::Unpublish() {
  if (m_Song != nullptr && m_Song->GetGameplaySnapshotDir() == m_SnapshotDir) {
    m_Song->ClearGameplaySnapshotDir();
  }
}

void CustomSongSnapshot::Reset() {
  Cancel();
  Wait();
  ReleaseCardLease();

  Unpublish();
  if (!m_SnapshotDir.empty()) {
    FILEMAN->DeleteRecursive(m_SnapshotDir);
  }

  m_Song = nullptr;
  m_Player = PLAYER_INVALID;
  m_DeviceIdentity.clear();
  m_SourceDir.clear();
  m_SnapshotDir.clear();
  m_Error.clear();
  m_BytesCopied.store(0);
  m_Status.store(Idle);
}

std::string CustomSongSnapshot::GetError() const {
  LockMut(m_Mutex);
  return m_Error;
}

int CustomSongSnapshot::ThreadStart(void* data) {
  static_cast<CustomSongSnapshot*>(data)->Run();
  return 0;
}

void CustomSongSnapshot::Run() {
  if (MEMCARDMAN->GetCardDeviceIdentity(m_Player) != m_DeviceIdentity) {
    Fail("The USB drive changed before the song could be copied.");
    return;
  }

  std::vector<std::string> files;
  if (!CollectFiles(files)) {
    if (MEMCARDMAN->IsCardSourceDegraded(m_Player)) {
      Fail("The USB drive was disconnected while reading the song.");
    } else if (GetStatus() != Failed) {
      Fail("The song directory could not be read.");
    }
    return;
  }
  if (files.empty()) {
    Fail(
        MEMCARDMAN->IsCardSourceDegraded(m_Player)
            ? "The USB drive was disconnected while reading the song."
            : "The song has no gameplay files to copy.");
    return;
  }

  for (const std::string& file : files) {
    if (m_Cancelled.load()) {
      Fail("Song loading was cancelled.");
      return;
    }
    if (!CopyOne(file)) {
      if (m_Cancelled.load()) {
        Fail("Song loading was cancelled.");
        return;
      }
      if (MEMCARDMAN->IsCardSourceDegraded(m_Player)) {
        Fail("The USB drive was disconnected while copying the song.");
        return;
      }
      if (GetStatus() != Failed) {
        Fail("A required song file could not be copied from USB.");
      }
      return;
    }
  }

  if (m_Cancelled.load()) {
    Fail("Song loading was cancelled.");
    return;
  }

  // Everything above is private staging data.  The main thread publishes the
  // completed path after observing Ready, avoiding cross-thread Song mutation.
  m_Status.store(Ready);
  LOG->Info(
      "Custom song snapshot ready at %s (%zu bytes)", m_SnapshotDir.c_str(),
      m_BytesCopied.load());
}

bool CustomSongSnapshot::CollectFiles(std::vector<std::string>& relativeFiles) {
  std::set<std::string> unique;

  std::vector<std::string> criticalExtensions =
      ActorUtil::GetTypeExtensionList(FT_Sound);
  const char* chartExtensions[] = {"sm",  "ssc", "sma", "dwi",
                                   "bms", "ksf", "lrc"};
  criticalExtensions.insert(
      criticalExtensions.end(), std::begin(chartExtensions),
      std::end(chartExtensions));

  std::vector<std::string> files;
  FILEMAN->GetDirListingWithMultipleExtensions(
      m_SourceDir, criticalExtensions, files, false, false);
  unique.insert(files.begin(), files.end());

  // Root extension scans cover conventional song layouts.  Also collect
  // explicitly referenced files so nested audio and chart layouts remain
  // complete without ever permitting a reference to escape the song folder.
  const auto addReferencedFile = [this, &unique](
                                     std::string path, bool required) {
    if (path.empty()) {
      return !required;
    }
    FixSlashesInPlace(path);
    CollapsePath(path);
    if (CompareNoCase(Left(path, m_SourceDir.size()), m_SourceDir) != 0) {
      if (required) {
        Fail("The song references a required file outside its USB folder.");
      }
      return !required;
    }
    const std::string relative = path.substr(m_SourceDir.size());
    if (!IsSafeRelativePath(relative) ||
        !FILEMAN->IsAFile(m_SourceDir + relative)) {
      if (required) {
        Fail("A required gameplay file is missing from the USB song.");
      }
      return !required;
    }
    unique.insert(relative);
    return true;
  };

  if (!addReferencedFile(m_Song->GetMusicPath(), true)) {
    return false;
  }
  addReferencedFile(m_Song->GetLyricsPath(), false);
  for (int track = 0; track < NUM_InstrumentTrack; ++track) {
    if (m_Song->HasInstrumentTrack(static_cast<InstrumentTrack>(track)) &&
        !addReferencedFile(
            m_Song->GetInstrumentTrackPath(static_cast<InstrumentTrack>(track)),
            true)) {
      return false;
    }
  }
  for (const std::string& keysound : m_Song->m_vsKeysoundFile) {
    if (!addReferencedFile(
            Song::GetSongAssetPath(keysound, m_SourceDir), true)) {
      return false;
    }
  }
  for (const Steps* steps : m_Song->GetAllSteps()) {
    if (!steps->GetFilename().empty() &&
        !addReferencedFile(steps->GetFilename(), true)) {
      return false;
    }
    if (!addReferencedFile(steps->GetMusicPath(), true)) {
      return false;
    }
  }

  if (m_IncludeStaticBackgrounds && !m_IncludeLua) {
    std::vector<std::string> recursive;
    if (!CollectDirectory("", 0, recursive)) {
      return false;
    }
    for (const std::string& file : recursive) {
      if (ActorUtil::GetFileType(m_SourceDir + file) == FT_Bitmap) {
        unique.insert(file);
      }
    }
  }

  if (m_IncludeLua) {
    std::vector<std::string> recursive;
    if (!CollectDirectory("", 0, recursive)) {
      return false;
    }
    unique.insert(recursive.begin(), recursive.end());
  }

  for (const std::string& path : unique) {
    if (IsSafeRelativePath(path) && FILEMAN->IsAFile(m_SourceDir + path)) {
      relativeFiles.push_back(path);
    }
  }
  return relativeFiles.size() <= MAX_SNAPSHOT_FILES;
}

bool CustomSongSnapshot::CollectDirectory(
    const std::string& relativeDir, int depth,
    std::vector<std::string>& relativeFiles) {
  if (depth > MAX_SNAPSHOT_DEPTH || relativeFiles.size() > MAX_SNAPSHOT_FILES ||
      m_Cancelled.load()) {
    return false;
  }

  const std::string current = m_SourceDir + relativeDir;
  std::vector<std::string> entries;
  FILEMAN->GetDirListing(current + "*", entries, false, false);
  for (const std::string& entry : entries) {
    if (entry.empty() || entry[0] == '.') {
      continue;
    }
    const std::string relative = relativeDir + entry;
    if (FILEMAN->IsAFile(m_SourceDir + relative)) {
      relativeFiles.push_back(relative);
      if (relativeFiles.size() > MAX_SNAPSHOT_FILES) {
        return false;
      }
    }
  }

  std::vector<std::string> directories;
  FILEMAN->GetDirListing(current + "*", directories, true, false);
  for (const std::string& directory : directories) {
    if (directory.empty() || directory[0] == '.') {
      continue;
    }
    if (!CollectDirectory(
            relativeDir + directory + "/", depth + 1, relativeFiles)) {
      return false;
    }
  }
  return true;
}

bool CustomSongSnapshot::CopyOne(const std::string& relativePath) {
  if (!IsSafeRelativePath(relativePath)) {
    Fail("The song contains an unsafe path.");
    return false;
  }

  RageFile source;
  if (!source.Open(m_SourceDir + relativePath, RageFile::READ)) {
    MEMCARDMAN->ReportCardRead(m_SourceDir, false);
    return false;
  }
  const int fileSize = source.GetFileSize();
  if (fileSize < 0 ||
      static_cast<size_t>(fileSize) >
          m_MaximumBytes - std::min(m_MaximumBytes, m_BytesCopied.load())) {
    Fail("The song exceeds the configured snapshot size limit.");
    return false;
  }

  RageFile destination;
  if (!destination.Open(m_SnapshotDir + relativePath, RageFile::WRITE)) {
    Fail("The in-memory song snapshot could not be created.");
    return false;
  }

  char buffer[COPY_BUFFER_BYTES];
  size_t copied = 0;
  while (!source.AtEOF()) {
    if (m_Cancelled.load()) {
      return false;
    }
    const int bytesRead = source.Read(buffer, sizeof(buffer));
    if (bytesRead <= 0) {
      break;
    }
    if (destination.Write(buffer, bytesRead) != bytesRead) {
      Fail("The in-memory song snapshot ran out of space.");
      return false;
    }
    copied += bytesRead;
    const size_t total = m_BytesCopied.fetch_add(bytesRead) + bytesRead;
    if (total > m_MaximumBytes) {
      Fail("The song exceeds the configured snapshot size limit.");
      return false;
    }
  }
  const bool sourceSuccess =
      source.GetError().empty() && copied == static_cast<size_t>(fileSize);
  MEMCARDMAN->ReportCardRead(m_SourceDir, sourceSuccess);
  return sourceSuccess && destination.GetError().empty();
}

void CustomSongSnapshot::Fail(const std::string& error) {
  {
    LockMut(m_Mutex);
    m_Error = error;
  }
  if (!m_SnapshotDir.empty()) {
    FILEMAN->DeleteRecursive(m_SnapshotDir);
  }
  m_Status.store(Failed);
  LOG->Warn("Custom song snapshot failed: %s", error.c_str());
}

void CustomSongSnapshot::ReleaseCardLease() {
  if (!m_HasCardLease) {
    return;
  }
  MEMCARDMAN->ReleaseCardReadLease(m_Player);
  m_HasCardLease = false;
}
