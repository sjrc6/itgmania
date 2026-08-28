#include "RageUtil_BackgroundLoader.h"

#include <limits>
#include <map>
#include <string>
#include <vector>

#include "MemoryCardManager.h"
#include "PrefsManager.h"
#include "RageFile.h"
#include "RageFileManager.h"
#include "RageLog.h"
#include "RageSurface.h"
#include "RageSurface_Load.h"
#include "RageThreads.h"
#include "RageUtil.h"
#include "global.h"

/* If we're on an OS with a good caching system, writing to our own cache will
 * only waste memory.  In that case, just read the file, to force it into system
 * cache. If we're on a system with an unreliable cache, read it into our own
 * cache.  This helps in Windows, where even reading directory entries off of a
 * CD--even if they've just been read--can cause long delays if the disk is in
 * use. */
static const bool g_bWriteToCache = true;

static const bool g_bEnableBackgroundLoading = true;
static const int g_iMaximumCachedAssetDimension = 4096;
static const int64_t g_iMaximumCachedAssetPixels = 4 * 1000 * 1000;

static int MaximumCachedAssetBytes() {
  if (PREFSMAN == nullptr) {
    return 0;
  }
  const double bytes = PREFSMAN->m_custom_songs_max_megabytes.Get() * 1000000.0;
  if (bytes <= 0) {
    return 0;
  }
  return bytes >= std::numeric_limits<int>::max()
             ? std::numeric_limits<int>::max()
             : static_cast<int>(bytes);
}

static bool NeedsPrivateCache(const std::string& path) {
  return MEMCARDMAN != nullptr && MEMCARDMAN->PathIsMemCard(path);
}

BackgroundLoader::BackgroundLoader()
    : m_StartSem("BackgroundLoaderSem"),
      m_Mutex("BackgroundLoaderMutex"),
      m_ThreadIdleEvent("BackgroundLoaderIdle") {
  if (!g_bEnableBackgroundLoading) {
    return;
  }

  m_sCachePathPrefix =
      ssprintf("/@mem/wheel-assets/%p", static_cast<void*>(this));

  m_bShutdownThread = false;
  m_sThreadIsActive = m_sThreadShouldAbort = false;
  m_LoadThread.SetName("BackgroundLoader");
  m_LoadThread.Create(LoadThread_Start, this);
}

static void DeleteEmptyDirectories(std::string sDir) {
  std::vector<std::string> asNewDirs;
  GetDirListing(sDir + "/*", asNewDirs, false, true);
  for (unsigned i = 0; i < asNewDirs.size(); ++i) {
    ASSERT_M(IsADirectory(asNewDirs[i]), asNewDirs[i]);
    DeleteEmptyDirectories(asNewDirs[i]);
  }

  FILEMAN->Remove(sDir);
}

BackgroundLoader::~BackgroundLoader() {
  if (!g_bEnableBackgroundLoading) {
    return;
  }

  Abort();

  m_bShutdownThread = true;
  m_StartSem.Post();
  m_LoadThread.Wait();

  /* Delete all leftover cached files. */
  for (;;) {
    std::string file;
    {
      LockMut(m_Mutex);
      if (m_FinishedRequests.empty()) {
        break;
      }
      file = m_FinishedRequests.begin()->first;
    }
    FinishedWithCachedFile(file);
  }

  /* m_sCachePathPrefix should be filled with several empty directories.  Delete
   * them and m_sCachePathPrefix, so we don't leak them. */
  DeleteEmptyDirectories(m_sCachePathPrefix);
}

/* Pull a request out of m_CacheRequests. */
std::string BackgroundLoader::GetRequest() {
  if (!g_bEnableBackgroundLoading) {
    return std::string();
  }

  LockMut(m_Mutex);
  if (!m_CacheRequests.size()) {
    return std::string();
  }

  std::string ret;
  ret = m_CacheRequests.front();
  m_CacheRequests.erase(m_CacheRequests.begin(), m_CacheRequests.begin() + 1);
  m_sThreadIsActive = true;
  return ret;
}

std::string BackgroundLoader::GetCachePath(std::string sPath) const {
  return m_sCachePathPrefix + sPath;
}

void BackgroundLoader::LoadThread() {
  while (!m_bShutdownThread) {
    /* Wait for a request.  It's normal for this to wait for a long time; don't
     * fail on timeout. */
    m_StartSem.Wait(false);

    std::string sFile = GetRequest();
    if (sFile.empty()) {
      continue;
    }

    {
      /* If the file already exists, short circuit. */
      LockMut(m_Mutex);
      std::map<std::string, int>::iterator it;
      it = m_FinishedRequests.find(sFile);
      if (it != m_FinishedRequests.end()) {
        ++it->second;
        LOG->Trace(
            "XXX: request %s done loading (already done), cnt now %i",
            sFile.c_str(), m_FinishedRequests[sFile]);
        MarkThreadIdle();
        continue;
      }
    }

    LOG->Trace("XXX: reading %s", sFile.c_str());

    std::string sCachePath = GetCachePath(sFile);

    /* Open the file and read it. */
    RageFile src;
    bool readSucceeded = false;
    int sourceSize = -1;
    if (src.Open(sFile) && (sourceSize = src.GetFileSize()) >= 0 &&
        sourceSize <= MaximumCachedAssetBytes()) {
      /* If we're writing to a file cache ... */
      RageFile dst;

      bool bWriteToCache = g_bWriteToCache;
      if (bWriteToCache) {
        bWriteToCache = dst.Open(sCachePath, RageFile::WRITE);
      }
      LOG->Trace("XXX: go on '%s' to '%s'", sFile.c_str(), sCachePath.c_str());

      char buf[1024 * 4];
      bool copySucceeded = !g_bWriteToCache || bWriteToCache;
      while (copySucceeded && !m_sThreadShouldAbort && !src.AtEOF()) {
        int got = src.Read(buf, sizeof(buf));
        if (got < 0) {
          copySucceeded = false;
          break;
        }
        if (got > 0 && bWriteToCache && dst.Write(buf, got) != got) {
          copySucceeded = false;
          break;
        }
      }
      if (bWriteToCache) {
        dst.Close();
      }

      readSucceeded = (!g_bWriteToCache || bWriteToCache) && copySucceeded &&
                      src.GetError().empty() &&
                      (!bWriteToCache || dst.GetError().empty()) &&
                      !m_sThreadShouldAbort;

      // Validate and decode away from the screen thread.  The decoded surface
      // is deliberately disposable; the bounded /@mem copy is what the main
      // thread later uploads as a texture.
      if (readSucceeded && bWriteToCache) {
        std::string error;
        RageSurface* header =
            RageSurfaceUtils::LoadFile(sCachePath, error, true);
        readSucceeded = header != nullptr && header->w > 0 && header->h > 0 &&
                        header->w <= g_iMaximumCachedAssetDimension &&
                        header->h <= g_iMaximumCachedAssetDimension &&
                        static_cast<int64_t>(header->w) * header->h <=
                            g_iMaximumCachedAssetPixels;
        delete header;
        if (readSucceeded) {
          RageSurface* decoded =
              RageSurfaceUtils::LoadFile(sCachePath, error, false);
          readSucceeded = decoded != nullptr;
          delete decoded;
        }
      }
      LOG->Trace("XXX: done");
    }
    src.Close();
    if (!readSucceeded) {
      FILEMAN->Remove(sCachePath);
    }

    LockMut(m_Mutex);
    if (!m_sThreadShouldAbort) {
      ++m_FinishedRequests[sFile];
      LOG->Trace(
          "XXX: request %s done loading, cnt now %i", sFile.c_str(),
          m_FinishedRequests[sFile]);
    } else {
      FILEMAN->Remove(sCachePath);

      LOG->Trace("XXX: request %s aborted", sFile.c_str());
    }

    MarkThreadIdle();
  }
}

void BackgroundLoader::CacheFile(const std::string& sFile) {
  if (!g_bEnableBackgroundLoading) {
    return;
  }

  if (sFile == "" || !NeedsPrivateCache(sFile)) {
    return;
  }

  LockMut(m_Mutex);
  m_CacheRequests.push_back(sFile);
  m_StartSem.Post();
}

bool BackgroundLoader::IsCacheFileFinished(
    const std::string& sFile, std::string& sActualPath) {
  if (!g_bEnableBackgroundLoading) {
    sActualPath = sFile;
    return true;
  }

  if (!NeedsPrivateCache(sFile)) {
    sActualPath = sFile;
    return true;
  }

  LockMut(m_Mutex);

  if (sFile == "") {
    sActualPath = "";
    return true;
  }

  std::map<std::string, int>::iterator it;
  it = m_FinishedRequests.find(sFile);
  if (it == m_FinishedRequests.end()) {
    return false;
  }

  LOG->Trace("XXX: %s finished (%i)", sFile.c_str(), it->second);
  if (g_bWriteToCache) {
    sActualPath = GetCachePath(sFile);
  } else {
    sActualPath = sFile;
  }

  return true;
}

void BackgroundLoader::FinishedWithCachedFile(std::string sFile) {
  if (!g_bEnableBackgroundLoading) {
    return;
  }

  if (sFile == "" || !NeedsPrivateCache(sFile)) {
    return;
  }

  bool remove = false;
  {
    LockMut(m_Mutex);
    std::map<std::string, int>::iterator it;
    it = m_FinishedRequests.find(sFile);
    if (it == m_FinishedRequests.end()) {
      return;
    }

    --it->second;
    ASSERT_M(it->second >= 0, ssprintf("%i", it->second));
    if (!it->second) {
      m_FinishedRequests.erase(it);
      remove = true;
    }
  }
  if (remove) {
    FILEMAN->Remove(GetCachePath(sFile));
  }
}

void BackgroundLoader::Abort() {
  if (!g_bEnableBackgroundLoading) {
    return;
  }

  std::vector<std::string> finished;
  {
    LockMut(m_Mutex);
    m_CacheRequests.clear();
    for (const auto& request : m_FinishedRequests) {
      finished.push_back(request.first);
    }
    m_FinishedRequests.clear();
    if (m_sThreadIsActive) {
      m_sThreadShouldAbort = true;
    }
  }
  for (const std::string& file : finished) {
    FILEMAN->Remove(GetCachePath(file));
  }
}

void BackgroundLoader::AbortAndWait() {
  Abort();
  m_ThreadIdleEvent.Lock();
  while (m_sThreadIsActive) {
    m_ThreadIdleEvent.Wait();
  }
  m_ThreadIdleEvent.Unlock();
}

void BackgroundLoader::MarkThreadIdle() {
  m_ThreadIdleEvent.Lock();
  m_sThreadShouldAbort = false;
  m_sThreadIsActive = false;
  m_ThreadIdleEvent.Broadcast();
  m_ThreadIdleEvent.Unlock();
}

/*
 * (c) 2004 Glenn Maynard
 * All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, and/or sell copies of the Software, and to permit persons to
 * whom the Software is furnished to do so, provided that the above
 * copyright notice(s) and this permission notice appear in all copies of
 * the Software and that both the above copyright notice(s) and this
 * permission notice appear in supporting documentation.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT OF
 * THIRD PARTY RIGHTS. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR HOLDERS
 * INCLUDED IN THIS NOTICE BE LIABLE FOR ANY CLAIM, OR ANY SPECIAL INDIRECT
 * OR CONSEQUENTIAL DAMAGES, OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS
 * OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR
 * OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */
