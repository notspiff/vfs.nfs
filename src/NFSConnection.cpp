/*
 *      Copyright (C) 2005-2013 Team XBMC
 *      http://xbmc.org
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with XBMC; see the file COPYING.  If not, see
 *  <http://www.gnu.org/licenses/>.
 *
 */

#include "NFSConnection.h"
extern "C"
{
#include <nfsc/libnfs.h>
#include <nfsc/libnfs-raw-mount.h>
}

#include <p8-platform/util/timeutils.h>

#include <kodi/Filesystem.h>
#include <kodi/General.h>
#include <kodi/Network.h>

//KEEP_ALIVE_TIMEOUT is decremented every half a second
//360 * 0.5s == 180s == 3mins
//so when no read was done for 3mins and files are open
//do the nfs keep alive for the open files
#define KEEP_ALIVE_TIMEOUT 360

//6 mins (360s) cached context timeout
#define CONTEXT_TIMEOUT 360000

//return codes for getContextForExport
#define CONTEXT_INVALID  0    //getcontext failed
#define CONTEXT_NEW      1    //new context created
#define CONTEXT_CACHED   2    //context cached and therefore already mounted (no new mount needed)

CNFSConnection& CNFSConnection::Get()
{
  static CNFSConnection instance;

  return instance;
}

CNFSConnection::CNFSConnection()
: m_pNfsContext(nullptr)
, m_exportPath("")
, m_hostName("")
, m_resolvedHostName("")
, m_readChunkSize(0)
, m_writeChunkSize(0)
, m_OpenConnections(0)
, m_IdleTimeout(0)
, m_lastAccessedTime(0)
{
}

CNFSConnection::~CNFSConnection()
{
  Deinit();
}

void CNFSConnection::resolveHost(const std::string& hostname)
{
  //resolve if hostname has changed
  kodi::network::DNSLookup(hostname, m_resolvedHostName);
}

std::list<std::string> CNFSConnection::GetExportList()
{
  std::list<std::string> retList;

  struct exportnode *exportlist, *tmp;
  exportlist = mount_getexports(m_resolvedHostName.c_str());
  tmp = exportlist;

  for(tmp = exportlist; tmp!=nullptr; tmp=tmp->ex_next)
  {
    retList.push_back(std::string(tmp->ex_dir));
  }

  mount_free_export_list(exportlist);
  retList.sort();
  retList.reverse();

  return retList;
}

void CNFSConnection::clearMembers()
{
  // NOTE - DON'T CLEAR m_exportList HERE!
  // splitUrlIntoExportAndPath checks for m_exportList.empty()
  // and would query the server in an excessive unwanted fashion
  // also don't clear m_KeepAliveTimeouts here because we
  // would loose any "paused" file handles during export change
  m_exportPath.clear();
  m_hostName.clear();
  m_exportList.clear();
  m_writeChunkSize = 0;
  m_readChunkSize = 0;
  m_pNfsContext = nullptr;
}

void CNFSConnection::destroyOpenContexts()
{
  m_openContextLock.Lock();
  for (const auto& entry : m_openContextMap)
  {
    nfs_destroy_context(entry.second.pContext);
  }
  m_openContextMap.clear();
  m_openContextLock.Unlock();
}

void CNFSConnection::destroyContext(const std::string& exportName)
{
  m_openContextLock.Lock();
  auto it = m_openContextMap.find(exportName);
  if (it != m_openContextMap.end())
  {
    nfs_destroy_context(it->second.pContext);
    m_openContextMap.erase(it);
  }
  m_openContextLock.Unlock();
}

struct nfs_context *CNFSConnection::getContextFromMap(const std::string& exportname, bool forceCacheHit/* = false*/)
{
  struct nfs_context *pRet = nullptr;
  m_openContextLock.Lock();

  auto it = m_openContextMap.find(exportname);
  if(it != m_openContextMap.end())
  {
    //check if context has timed out already
    uint64_t now = P8PLATFORM::GetTimeMs();
    if((now - it->second.lastAccessedTime) < CONTEXT_TIMEOUT || forceCacheHit)
    {
      //its not timedout yet or caller wants the cached entry regardless of timeout
      //refresh access time of that
      //context and return it
      if (!forceCacheHit) // only log it if this isn't the resetkeepalive on each read ;)
        kodi::Log(ADDON_LOG_DEBUG, "NFS: Refreshing context for %s, old: %" PRId64 ", new: %" PRId64, exportname.c_str(), it->second.lastAccessedTime, now);
      it->second.lastAccessedTime = now;
      pRet = it->second.pContext;
    }
    else
    {
      //context is timed out
      //destroy it and return nullptr
      kodi::Log(ADDON_LOG_DEBUG, "NFS: Old context timed out - destroying it");
      nfs_destroy_context(it->second.pContext);
      m_openContextMap.erase(it);
    }
  }
  m_openContextLock.Unlock();
  return pRet;
}

int CNFSConnection::getContextForExport(const std::string& exportname)
{
  int ret = CONTEXT_INVALID;

  clearMembers();

  m_pNfsContext = getContextFromMap(exportname);

  if(!m_pNfsContext)
  {
    kodi::Log(ADDON_LOG_DEBUG,"NFS: Context for %s not open - get a new context.", exportname.c_str());
    m_pNfsContext = nfs_init_context();

    if(!m_pNfsContext)
    {
      kodi::Log(ADDON_LOG_ERROR,"NFS: Error initcontext in getContextForExport.");
    }
    else
    {
      struct contextTimeout tmp;
      m_openContextLock.Lock();
      tmp.pContext = m_pNfsContext;
      tmp.lastAccessedTime = P8PLATFORM::GetTimeMs();
      m_openContextMap[exportname] = tmp; //add context to list of all contexts
      ret = CONTEXT_NEW;
      m_openContextLock.Unlock();
    }
  }
  else
  {
    ret = CONTEXT_CACHED;
    kodi::Log(ADDON_LOG_DEBUG,"NFS: Using cached context.");
  }
  m_lastAccessedTime = P8PLATFORM::GetTimeMs(); //refresh last access time of m_pNfsContext

  return ret;
}

bool CNFSConnection::splitUrlIntoExportAndPath(const std::string& hostname,
                                               const std::string& filename,
                                               std::string& exportPath,
                                               std::string& relativePath)
{
  bool ret = false;

  //refresh exportlist if empty or hostname change
  if(m_exportList.empty() || m_hostName != hostname)
  {
    m_exportList = GetExportList();
  }

  if(!m_exportList.empty())
  {
    relativePath = "";
    exportPath = "";

    std::string path = filename;

    //GetFileName returns path without leading "/"
    //but we need it because the export paths start with "/"
    //and path.Find(*it) wouldn't work else
    if(!path.empty() && path[0] != '/')
    {
      path = "/" + path;
    }

    std::list<std::string>::iterator it;

    for (const auto& entry : m_exportList)
    {
      //if path starts with the current export path
      if(path.compare(0, entry.size(), entry) == 0)
      {
        exportPath = entry;
        //handle special case where root is exported
        //in that case we don't want to strip off to
        //much from the path
        if( exportPath == "/" )
          relativePath = "//" + path.substr(exportPath.length()-1);
        else
          relativePath = "//" + path.substr(exportPath.length());
        ret = true;
        break;
      }
    }
  }
  return ret;
}

bool CNFSConnection::Connect(const VFSURL& url, std::string& relativePath)
{
  P8PLATFORM::CLockObject lock(*this);
  int nfsRet = 0;
  std::string exportPath;

  resolveHost(url.hostname);
  bool ret = splitUrlIntoExportAndPath(url.hostname, url.filename, exportPath, relativePath);

  if( (ret && (exportPath != m_exportPath  ||
      m_hostName != url.hostname))    ||
      (P8PLATFORM::GetTimeMs() - m_lastAccessedTime) > CONTEXT_TIMEOUT )
  {
    int contextRet = getContextForExport(std::string(url.hostname) + exportPath);

    if(contextRet == CONTEXT_INVALID)//we need a new context because sharename or hostname has changed
    {
      return false;
    }

    if(contextRet == CONTEXT_NEW) //new context was created - we need to mount it
    {
      //we connect to the directory of the path. This will be the "root" path of this connection then.
      //So all fileoperations are relative to this mountpoint...
      nfsRet = nfs_mount(m_pNfsContext, m_resolvedHostName.c_str(), exportPath.c_str());

      if(nfsRet != 0)
      {
        kodi::Log(ADDON_LOG_ERROR,"NFS: Failed to mount nfs share: %s %s (%s)", m_resolvedHostName.c_str(), exportPath.c_str(), nfs_get_error(m_pNfsContext));
        destroyContext(std::string(url.hostname) + exportPath);
        return false;
      }
      kodi::Log(ADDON_LOG_DEBUG,"NFS: Connected to server %s and export %s", url.hostname, exportPath.c_str());
    }
    m_exportPath = exportPath;
    m_hostName = url.hostname;
    //read chunksize only works after mount
    m_readChunkSize = nfs_get_readmax(m_pNfsContext);
    m_writeChunkSize =nfs_get_writemax(m_pNfsContext);

    if(contextRet == CONTEXT_NEW)
    {
      kodi::Log(ADDON_LOG_DEBUG,"NFS: chunks: r/w %i/%i\n", (int)m_readChunkSize,(int)m_writeChunkSize);
    }
  }
  return ret;
}

void CNFSConnection::Deinit()
{
  if(m_pNfsContext)
  {
    destroyOpenContexts();
    m_pNfsContext = nullptr;
  }
  clearMembers();
  // clear any keep alive timouts on deinit
  m_KeepAliveTimeouts.clear();
}

/* This is called from CApplication::ProcessSlow() and is used to tell if nfs have been idle for too long */
void CNFSConnection::CheckIfIdle()
{
  /* We check if there are open connections. This is done without a lock to not halt the mainthread. It should be thread safe as
   worst case scenario is that m_OpenConnections could read 0 and then changed to 1 if this happens it will enter the if wich will lead to another check, wich is locked.  */
  if (m_OpenConnections == 0 && m_pNfsContext != nullptr)
  { /* I've set the the maximum IDLE time to be 1 min and 30 sec. */
    P8PLATFORM::CLockObject lock(*this);
    if (m_OpenConnections == 0 /* check again - when locked */)
    {
      if (m_IdleTimeout > 0)
      {
        m_IdleTimeout--;
      }
      else
      {
        kodi::Log(ADDON_LOG_NOTICE, "NFS is idle. Closing the remaining connections.");
        Deinit();
      }
    }
  }

  if( m_pNfsContext != nullptr )
  {
    P8PLATFORM::CLockObject lock(m_keepAliveLock);
    //handle keep alive on opened files
    for (auto& entry : m_KeepAliveTimeouts)
    {
      if(entry.second.refreshCounter > 0)
      {
        entry.second.refreshCounter--;
      }
      else
      {
        keepAlive(entry.second.exportPath, entry.first);
        //reset timeout
        resetKeepAlive(entry.second.exportPath, entry.first);
      }
    }
  }
}

//remove file handle from keep alive list on file close
void CNFSConnection::removeFromKeepAliveList(struct nfsfh  *_pFileHandle)
{
  P8PLATFORM::CLockObject lock(m_keepAliveLock);
  m_KeepAliveTimeouts.erase(_pFileHandle);
}

//reset timeouts on read
void CNFSConnection::resetKeepAlive(std::string _exportPath, struct nfsfh  *_pFileHandle)
{
  P8PLATFORM::CLockObject lock(m_keepAliveLock);
  //refresh last access time of the context aswell
  struct nfs_context *pContext = getContextFromMap(_exportPath, true);

  // if we keep alive using m_pNfsContext we need to mark
  // its last access time too here
  if (m_pNfsContext == pContext)
  {
    m_lastAccessedTime = P8PLATFORM::GetTimeMs();
  }

  //adds new keys - refreshs existing ones
  m_KeepAliveTimeouts[_pFileHandle].exportPath = _exportPath;
  m_KeepAliveTimeouts[_pFileHandle].refreshCounter = KEEP_ALIVE_TIMEOUT;
}

//keep alive the filehandles nfs connection
//by blindly doing a read 32bytes - seek back to where
//we were before
void CNFSConnection::keepAlive(std::string _exportPath, struct nfsfh  *_pFileHandle)
{
  uint64_t offset = 0;
  char buffer[32];
  // this also refreshs the last accessed time for the context
  // true forces a cachehit regardless the context is timedout
  // on this call we are sure its not timedout even if the last accessed
  // time suggests it.
  struct nfs_context *pContext = getContextFromMap(_exportPath, true);

  if (!pContext)// this should normally never happen - paranoia
    pContext = m_pNfsContext;

  kodi::Log(ADDON_LOG_NOTICE, "NFS: sending keep alive after %i s.", KEEP_ALIVE_TIMEOUT/2);
  P8PLATFORM::CLockObject lock(*this);
  nfs_lseek(pContext, _pFileHandle, 0, SEEK_CUR, &offset);
  nfs_read(pContext, _pFileHandle, 32, buffer);
  nfs_lseek(pContext, _pFileHandle, offset, SEEK_SET, &offset);
}

int CNFSConnection::stat(const VFSURL& url, struct stat *statbuff)
{
  P8PLATFORM::CLockObject lock(*this);
  int nfsRet = 0;
  std::string exportPath;
  std::string relativePath;
  struct nfs_context *pTmpContext = nullptr;

  resolveHost(url.hostname);

  if(splitUrlIntoExportAndPath(url.hostname, url.filename, exportPath, relativePath))
  {
    pTmpContext = nfs_init_context();

    if(pTmpContext)
    {
      //we connect to the directory of the path. This will be the "root" path of this connection then.
      //So all fileoperations are relative to this mountpoint...
      nfsRet = nfs_mount(pTmpContext, m_resolvedHostName.c_str(), exportPath.c_str());

      if(nfsRet == 0)
      {
        nfsRet = nfs_stat(pTmpContext, relativePath.c_str(), statbuff);
      }
      else
      {
        kodi::Log(ADDON_LOG_ERROR,"NFS: Failed to mount nfs share: %s (%s)", exportPath.c_str(), nfs_get_error(m_pNfsContext));
      }

      nfs_destroy_context(pTmpContext);
      kodi::Log(ADDON_LOG_DEBUG,"NFS: Connected to server %s and export %s in tmpContext", url.hostname, exportPath.c_str());
    }
  }

  return nfsRet;
}

/* The following two function is used to keep track on how many Opened files/directories there are.
needed for unloading the dylib*/
void CNFSConnection::AddActiveConnection()
{
  P8PLATFORM::CLockObject lock(*this);
  m_OpenConnections++;
}

void CNFSConnection::AddIdleConnection()
{
  P8PLATFORM::CLockObject lock(*this);
  m_OpenConnections--;
  /* If we close a file we reset the idle timer so that we don't have any wierd behaviours if a user
   leaves the movie paused for a long while and then press stop */
  m_IdleTimeout = 180;
}
