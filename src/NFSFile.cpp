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

#include "p8-platform/threads/mutex.h"
#include <fcntl.h>
#include <sstream>
#include <iostream>

extern "C"
{
#include <nfsc/libnfs.h>
#include <nfsc/libnfs-raw-mount.h>
#include <nfsc/libnfs-raw-nfs.h>
}

#include "NFSFile.h"
#include <kodi/Filesystem.h>
#include <kodi/General.h>

void* CNFSFile::Open(const VFSURL& url)
{
  CNFSConnection::Get().AddActiveConnection();
  int ret = 0;
  // we can't open files like nfs://file.f or nfs://server/file.f
  // if a file matches the if below return false, it can't exist on a nfs share.
  if (!IsValidFile(url.filename))
  {
    kodi::Log(ADDON_LOG_NOTICE,"NFS: Bad URL : '%s'", url.filename);
    return nullptr;
  }

  std::string filename;

  P8PLATFORM::CLockObject lock(CNFSConnection::Get());

  if(!CNFSConnection::Get().Connect(url, filename))
  {
    return nullptr;
  }

  NFSContext* result = new NFSContext;

  result->pNfsContext = CNFSConnection::Get().GetNfsContext();
  result->exportPath = CNFSConnection::Get().GetContextMapId();

  ret = nfs_open(result->pNfsContext, filename.c_str(), O_RDONLY, &result->pFileHandle);

  if (ret != 0)
  {
    kodi::Log(ADDON_LOG_INFO, "CNFSFile::Open: Unable to open file : '%s'  error : '%s'", url.filename, nfs_get_error(result->pNfsContext));
    delete result;
    return nullptr;
  }

  kodi::Log(ADDON_LOG_DEBUG,"CNFSFile::Open - opened %s", filename.c_str());
  result->filename = url.filename;

  struct __stat64 tmpBuffer;

  if( Stat(url, &tmpBuffer) )
  {
    Close(result);
    return nullptr;
  }

  result->size = tmpBuffer.st_size;//cache the size of this file

  // We've successfully opened the file!
  return result;
}

void* CNFSFile::OpenForWrite(const VFSURL& url, bool bOverWrite)
{
  int ret = 0;
  // we can't open files like nfs://file.f or nfs://server/file.f
  // if a file matches the if below return false, it can't exist on a nfs share.
  if (!IsValidFile(url.filename))
    return nullptr;

  P8PLATFORM::CLockObject lock(CNFSConnection::Get());
  std::string filename;

  if(!CNFSConnection::Get().Connect(url, filename))
  {
    return nullptr;
  }

  NFSContext* result = new NFSContext;
  result->pNfsContext = CNFSConnection::Get().GetNfsContext();
  result->exportPath = CNFSConnection::Get().GetContextMapId();

  if (bOverWrite)
  {
    kodi::Log(ADDON_LOG_INFO, "FileNFS::OpenForWrite() called with overwriting enabled! - %s", filename.c_str());
    //create file with proper permissions
    ret = nfs_creat(result->pNfsContext, filename.c_str(), S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH, &result->pFileHandle);
    //if file was created the file handle isn't valid ... so close it and open later
    if(ret == 0)
    {
      nfs_close(result->pNfsContext, result->pFileHandle);
      result->pFileHandle = nullptr;
    }
  }

  ret = nfs_open(result->pNfsContext, filename.c_str(), O_RDWR, &result->pFileHandle);

  if (ret || result->pFileHandle == nullptr)
  {
    // write error to logfile
    kodi::Log(ADDON_LOG_ERROR, "CNFSFile::Open: Unable to open file : '%s' error : '%s'",
              filename.c_str(), nfs_get_error(CNFSConnection::Get().GetNfsContext()));
    delete result;
    return nullptr;
  }
  result->filename = url.filename;

  //only stat if file was not created
  if(!bOverWrite)
  {
    struct __stat64 tmpBuffer;

    if( Stat(url, &tmpBuffer) )
    {
      Close(result);
      return nullptr;
    }

    result->size = tmpBuffer.st_size;//cache the size of this file
  }
  else//file was created - filesize is zero
  {
    result->size = 0;
  }

  // We've successfully opened the file!
  return result;
}

ssize_t CNFSFile::Read(void* context, void* lpBuf, size_t uiBufSize)
{
  NFSContext* ctx = (NFSContext*)context;
  if (!ctx || !ctx->pFileHandle|| !ctx->pNfsContext)
    return -1;

  P8PLATFORM::CLockObject lock(CNFSConnection::Get());
  ssize_t numberOfBytesRead = nfs_read(ctx->pNfsContext, ctx->pFileHandle, uiBufSize, (char *)lpBuf);

  CNFSConnection::Get().resetKeepAlive(ctx->exportPath, ctx->pFileHandle);//triggers keep alive timer reset for this filehandle

  //something went wrong ...
  if (numberOfBytesRead < 0)
    kodi::Log(ADDON_LOG_ERROR, "%s - Error( %" PRId64", %s )", __FUNCTION__, (int64_t)numberOfBytesRead, nfs_get_error(ctx->pNfsContext));

  return numberOfBytesRead;
}

//this was a bitch!
//for nfs write to work we have to write chunked
//otherwise this could crash on big files
ssize_t CNFSFile::Write(void* context, const void* lpBuf, size_t uiBufSize)
{
  NFSContext* ctx = (NFSContext*)context;
  if (!ctx || !ctx->pFileHandle|| !ctx->pNfsContext)
    return -1;

  ssize_t numberOfBytesWritten = 0;
  ssize_t writtenBytes = 0;
  size_t leftBytes = uiBufSize;
  //clamp max write chunksize to 32kb - fixme - this might be superfluous with future libnfs versions
  size_t chunkSize = CNFSConnection::Get().GetMaxWriteChunkSize() > 32768 ? 32768 : CNFSConnection::Get().GetMaxWriteChunkSize();

  P8PLATFORM::CLockObject lock(CNFSConnection::Get());

  //write as long as some bytes are left to be written
  while( leftBytes )
  {
    //the last chunk could be smalle than chunksize
    if(leftBytes < chunkSize)
    {
      chunkSize = leftBytes;//write last chunk with correct size
    }
    //write chunk
    writtenBytes = nfs_write(ctx->pNfsContext,
                             ctx->pFileHandle,
                             chunkSize,
                             (char *)lpBuf + numberOfBytesWritten);
    //decrease left bytes
    leftBytes-= writtenBytes;
    //increase overall written bytes
    numberOfBytesWritten += writtenBytes;

    //danger - something went wrong
    if (writtenBytes < 0)
    {
      kodi::Log(ADDON_LOG_ERROR, "Failed to pwrite(%s) %s",
                ctx->filename.c_str(), nfs_get_error(ctx->pNfsContext));
      break;
    }
  }

  //return total number of written bytes
  return numberOfBytesWritten;
}

int64_t CNFSFile::Seek(void* context, int64_t iFilePosition, int iWhence)
{
  NFSContext* ctx = (NFSContext*)context;
  if (!ctx || !ctx->pFileHandle|| !ctx->pNfsContext)
    return 0;

  int ret = 0;
  uint64_t offset = 0;

  P8PLATFORM::CLockObject lock(CNFSConnection::Get());

  ret = (int)nfs_lseek(ctx->pNfsContext, ctx->pFileHandle, iFilePosition, iWhence, &offset);
  if (ret < 0)
  {
    kodi::Log(ADDON_LOG_ERROR, "%s - Error( seekpos: %" PRId64 ", whence: %i, fsize: %" PRId64 ", %s)",
              __FUNCTION__, iFilePosition, iWhence, ctx->size, nfs_get_error(ctx->pNfsContext));
    return -1;
  }
  return (int64_t)offset;
}

int CNFSFile::Truncate(void* context, int64_t size)
{
  NFSContext* ctx = (NFSContext*)context;
  if (!ctx || !ctx->pFileHandle|| !ctx->pNfsContext)
    return -1;

  int ret = 0;
  P8PLATFORM::CLockObject lock(CNFSConnection::Get());
  ret = (int)nfs_ftruncate(ctx->pNfsContext, ctx->pFileHandle, size);
  if (ret < 0)
  {
    kodi::Log(ADDON_LOG_ERROR, "%s - Error( ftruncate: %" PRId64 ", fsize: %" PRId64 ", %s)",
              __FUNCTION__, size, ctx->size, nfs_get_error(ctx->pNfsContext));
    return -1;
  }
  return ret;
}

int64_t CNFSFile::GetLength(void* context)
{
  if (!context)
    return 0;

  NFSContext* ctx = (NFSContext*)context;
  return ctx->size;
}

int64_t CNFSFile::GetPosition(void* context)
{
  if (!context)
    return 0;

  NFSContext* ctx = (NFSContext*)context;
  int ret = 0;
  uint64_t offset = 0;

  if (CNFSConnection::Get().GetNfsContext() == nullptr || ctx->pFileHandle == nullptr)
    return 0;

  P8PLATFORM::CLockObject lock(CNFSConnection::Get());

  ret = (int)nfs_lseek(CNFSConnection::Get().GetNfsContext(), ctx->pFileHandle, 0, SEEK_CUR, &offset);

  if (ret < 0)
  {
    kodi::Log(ADDON_LOG_ERROR, "NFS: Failed to lseek(%s)", nfs_get_error(CNFSConnection::Get().GetNfsContext()));
  }
  return offset;
}

int CNFSFile::IoControl(void* context, XFILE::EIoControl request, void* param)
{
  if(request == XFILE::IOCTRL_SEEK_POSSIBLE)
    return 1;

  return -1;
}

int CNFSFile::Stat(const VFSURL& url, struct __stat64* buffer)
{
  int ret = 0;
  P8PLATFORM::CLockObject lock(CNFSConnection::Get());
  std::string filename;

  if(!CNFSConnection::Get().Connect(url, filename))
  {
    return -1;
  }

  struct stat tmpBuffer = {0};

  ret = nfs_stat(CNFSConnection::Get().GetNfsContext(), filename.c_str(), &tmpBuffer);

  //if buffer == nullptr we where called from Exists - in that case don't spam the log with errors
  if (ret != 0 && buffer != nullptr)
  {
    kodi::Log(ADDON_LOG_ERROR, "NFS: Failed to stat(%s) %s", url.filename, nfs_get_error(CNFSConnection::Get().GetNfsContext()));
    ret = -1;
  }
  else
  {
    if(buffer)
    {
      memset(buffer, 0, sizeof(struct __stat64));
      buffer->st_dev = tmpBuffer.st_dev;
      buffer->st_ino = tmpBuffer.st_ino;
      buffer->st_mode = tmpBuffer.st_mode;
      buffer->st_nlink = tmpBuffer.st_nlink;
      buffer->st_uid = tmpBuffer.st_uid;
      buffer->st_gid = tmpBuffer.st_gid;
      buffer->st_rdev = tmpBuffer.st_rdev;
      buffer->st_size = tmpBuffer.st_size;
      buffer->st_atime = tmpBuffer.st_atime;
      buffer->st_mtime = tmpBuffer.st_mtime;
      buffer->st_ctime = tmpBuffer.st_ctime;
    }
  }
  return ret;
}

bool CNFSFile::Close(void* context)
{
  NFSContext* ctx = (NFSContext*)context;
  if (!ctx)
    return false;

  P8PLATFORM::CLockObject lock(CNFSConnection::Get());
  CNFSConnection::Get().AddIdleConnection();

  if (ctx->pFileHandle != nullptr && ctx->pNfsContext != nullptr)
  {
    int ret = 0;
    kodi::Log(ADDON_LOG_DEBUG,"CNFSFile::Close closing file %s", ctx->filename.c_str());
    // remove it from keep alive list before closing
    // so keep alive code doesn't process it anymore
    CNFSConnection::Get().removeFromKeepAliveList(ctx->pFileHandle);
    ret = nfs_close(ctx->pNfsContext, ctx->pFileHandle);

    if (ret < 0)
    {
      kodi::Log(ADDON_LOG_ERROR, "Failed to close(%s) - %s", ctx->filename.c_str(), nfs_get_error(ctx->pNfsContext));
    }
  }

  delete ctx;

  return true;
}

bool CNFSFile::Exists(const VFSURL& url)
{
  return Stat(url, nullptr) == 0;
}

void CNFSFile::ClearOutIdle()
{
  CNFSConnection::Get().CheckIfIdle();
}

void CNFSFile::DisconnectAll()
{
  CNFSConnection::Get().Deinit();
}

bool CNFSFile::Delete(const VFSURL& url)
{
  int ret = 0;
  P8PLATFORM::CLockObject lock(CNFSConnection::Get());
  std::string filename;

  if(!CNFSConnection::Get().Connect(url, filename))
  {
    return false;
  }

  ret = nfs_unlink(CNFSConnection::Get().GetNfsContext(), filename.c_str());

  if(ret != 0)
  {
    kodi::Log(ADDON_LOG_ERROR, "%s - Error( %s )", __FUNCTION__, nfs_get_error(CNFSConnection::Get().GetNfsContext()));
  }

  return (ret == 0);
}

bool CNFSFile::Rename(const VFSURL& url, const VFSURL& url2)
{
  int ret = 0;
  P8PLATFORM::CLockObject lock(CNFSConnection::Get());
  std::string strFile;

  if(!CNFSConnection::Get().Connect(url, strFile))
  {
    return false;
  }

  std::string strFileNew;
  std::string strDummy;
  CNFSConnection::Get().splitUrlIntoExportAndPath(url2.hostname, url2.filename, strDummy, strFileNew);

  ret = nfs_rename(CNFSConnection::Get().GetNfsContext(), strFile.c_str(), strFileNew.c_str());

  if(ret != 0)
  {
    kodi::Log(ADDON_LOG_ERROR, "%s - Error( %s )", __FUNCTION__, nfs_get_error(CNFSConnection::Get().GetNfsContext()));
  }

  return (ret == 0);
}

bool CNFSFile::DirectoryExists(const VFSURL& url)
{
  int ret = 0;

  P8PLATFORM::CLockObject lock(CNFSConnection::Get());
  std::string folderName(url.filename);
  if (folderName[folderName.size()-1] == '/')
    folderName.erase(folderName.end()-1);

  if(!CNFSConnection::Get().Connect(url, folderName))
  {
    return false;
  }

  struct stat info;
  ret = nfs_stat(CNFSConnection::Get().GetNfsContext(), folderName.c_str(), &info);

  if (ret != 0)
  {
    return false;
  }
  return S_ISDIR(info.st_mode) ? true : false;
}

bool CNFSFile::RemoveDirectory(const VFSURL& url2)
{
  int ret = 0;

  P8PLATFORM::CLockObject lock(CNFSConnection::Get());
  std::string folderName(url2.filename);
  VFSURL url = url2;
  if (folderName[folderName.size()-1] == '/')
  {
    folderName.erase(folderName.end()-1);
    url.filename = folderName.c_str();
  }

  if(!CNFSConnection::Get().Connect(url, folderName))
  {
    return false;
  }

  ret = nfs_rmdir(CNFSConnection::Get().GetNfsContext(), folderName.c_str());

  if(ret != 0 && errno != ENOENT)
  {
    kodi::Log(ADDON_LOG_ERROR, "%s - Error( %s )", __FUNCTION__, nfs_get_error(CNFSConnection::Get().GetNfsContext()));
    return false;
  }
  return true;
}

bool CNFSFile::CreateDirectory(const VFSURL& url2)
{
  int ret = 0;
  bool success=true;

  P8PLATFORM::CLockObject lock(CNFSConnection::Get());
  std::string folderName(url2.filename);
  VFSURL url = url2;

  if (folderName[folderName.size()-1] == '/')
  {
    folderName.erase(folderName.end()-1);
    url.filename = folderName.c_str();
  }

  if(!CNFSConnection::Get().Connect(url, folderName))
  {
    return false;
  }

  ret = nfs_mkdir(CNFSConnection::Get().GetNfsContext(), folderName.c_str());

  success = (ret == 0 || -EEXIST == ret);
  if(!success)
    kodi::Log(ADDON_LOG_ERROR, "NFS: Failed to create(%s) %s", folderName.c_str(), nfs_get_error(CNFSConnection::Get().GetNfsContext()));

  return success;
}

bool CNFSFile::GetDirectory(const VFSURL& url, std::vector<kodi::vfs::CDirEntry>& items, CVFSCallbacks callbacks)
{
  P8PLATFORM::CLockObject lock(CNFSConnection::Get());
  CNFSConnection::Get().AddActiveConnection();
  // We accept nfs://server/path[/file]]]]
  int ret = 0;
  //FILETIME fileTime, localTime;
  std::string strDirName;
  std::string myStrPath(url.url);
  if (myStrPath[myStrPath.size()-1] != '/')
    myStrPath += '/';

  if(!CNFSConnection::Get().Connect(url, strDirName))
  {
    std::cout << "conn fail" << std::endl;
    //connect has failed - so try to get the exported filesystems if no path is given to the url
    if (!strlen(url.sharename))
    {
      if(!strlen(url.hostname))
      {
        return GetServerList(items);
      }
      else
      {
        return GetDirectoryFromExportList(myStrPath, items);
      }
    }
    else
    {
      return false;
    }
  }

  struct nfsdir *nfsdir = nullptr;
  struct nfsdirent *nfsdirent = nullptr;

  ret = nfs_opendir(CNFSConnection::Get().GetNfsContext(), strDirName.c_str(), &nfsdir);

  if(ret != 0)
  {
    kodi::Log(ADDON_LOG_ERROR, "Failed to open(%s) %s", strDirName.c_str(), nfs_get_error(CNFSConnection::Get().GetNfsContext()));
    return false;
  }
  lock.Unlock();

  while((nfsdirent = nfs_readdir(CNFSConnection::Get().GetNfsContext(), nfsdir)) != nullptr)
  {
    struct nfsdirent tmpDirent = *nfsdirent;
    std::string strName = tmpDirent.name;
    std::string path(myStrPath + strName);
    int64_t iSize = 0;
    bool bIsDir = false;
    int64_t lTimeDate = 0;

    //reslove symlinks
    if(tmpDirent.type == NF3LNK)
    {
      //resolve symlink changes tmpDirent and strName
      if(!ResolveSymlink(url, &tmpDirent, path))
      {
        continue;
      }
    }

    iSize = tmpDirent.size;
    bIsDir = tmpDirent.type == NF3DIR;
    lTimeDate = tmpDirent.mtime.tv_sec;

    if (strName != "." && strName != ".."
                       && strName != "lost+found")
    {
      if(lTimeDate == 0) // if modification date is missing, use create date
      {
        lTimeDate = tmpDirent.ctime.tv_sec;
      }

      /*LONGLONG ll = P8PLATFORM::Int32x32To64(lTimeDate & 0xffffffff, 10000000) + 116444736000000000ll;
      fileTime.dwLowDateTime = (DWORD) (ll & 0xffffffff);
      fileTime.dwHighDateTime = (DWORD)(ll >> 32);
      P8PLATFORM::FileTimeToLocalFileTime(&fileTime, &localTime);*/

      kodi::vfs::CDirEntry pItem;
      pItem.SetLabel(tmpDirent.name);
      //pItem.mtime = localTime;
      pItem.SetSize(iSize);

      if (bIsDir)
      {
        if (path[path.size()-1] != '/')
          path += '/';
        pItem.SetFolder(true);
      }
      else
      {
        pItem.SetFolder(false);
      }

      if (strName[0] == '.')
      {
        pItem.AddProperty("file:hidden", "true");
      }
      else
      {
        pItem.ClearProperties();
      }
      pItem.SetPath(path);
      items.push_back(pItem);
    }
  }

  lock.Lock();
  nfs_closedir(CNFSConnection::Get().GetNfsContext(), nfsdir);//close the dir
  return true;
}

bool CNFSFile::GetDirectoryFromExportList(const std::string& strPath, std::vector<kodi::vfs::CDirEntry>& items)
{
  std::string nonConstStrPath(strPath);
  std::list<std::string> exportList=CNFSConnection::Get().GetExportList();
  std::list<std::string>::iterator it;

  for (const auto& entry : exportList)
  {
    std::string currentExport(entry);
    if (!nonConstStrPath.empty() && nonConstStrPath[nonConstStrPath.size()-1] == '/')
      nonConstStrPath.erase(nonConstStrPath.end()-1);

    kodi::vfs::CDirEntry pItem;
    pItem.SetLabel(currentExport);
    std::string path(nonConstStrPath + currentExport);
    if (path[path.size()-1] != '/')
      path += '/';
    pItem.SetPath(path);

    pItem.SetFolder(true);
    pItem.ClearProperties();
    items.push_back(pItem);
  }

  return exportList.empty() ? false : true;
}

bool CNFSFile::GetServerList(std::vector<kodi::vfs::CDirEntry>& items)
{
  struct nfs_server_list *srvrs;
  struct nfs_server_list *srv;
  bool ret = false;

  srvrs = nfs_find_local_servers();

  for (srv=srvrs; srv; srv = srv->next)
  {
    std::string currentExport(srv->addr);

    kodi::vfs::CDirEntry pItem;
    std::string path(std::string("nfs://") + currentExport);
    if (path[path.size()-1] != '/')
      path += '/';
    pItem.SetPath(path);
    pItem.SetLabel(currentExport);
    pItem.SetTitle("");

    pItem.SetFolder(true);
    pItem.ClearProperties();
    items.push_back(pItem);
    ret = true; //added at least one entry
  }
  free_nfs_srvr_list(srvrs);

  return ret;
}

bool CNFSFile::ResolveSymlink(const VFSURL& url, struct nfsdirent *dirent, std::string& resolvedUrl)
{
  P8PLATFORM::CLockObject lock(CNFSConnection::Get());
  int ret = 0;
  bool retVal = true;
  std::string fullpath = url.filename;
  char resolvedLink[MAX_PATH];

  if (fullpath[fullpath.size()-1] != '/')
    fullpath += '/';
  fullpath.append(dirent->name);

  ret = nfs_readlink(CNFSConnection::Get().GetNfsContext(), fullpath.c_str(), resolvedLink, MAX_PATH);

  if(ret == 0)
  {
    struct stat tmpBuffer = {0};
    fullpath = url.filename;
    if (fullpath[fullpath.size()-1] != '/')
      fullpath += '/';
    fullpath.append(resolvedLink);

    //special case - if link target is absolute it could be even another export
    //intervolume symlinks baby ...
    if(resolvedLink[0] == '/')
    {
      //use the special stat function for using an extra context
      //because we are inside of a dir traversal
      //and just can't change the global nfs context here
      //without destroying something...
      fullpath = resolvedLink;
      resolvedUrl = fullpath;
      ret = CNFSConnection::Get().stat(url, &tmpBuffer);
    }
    else
    {
      ret = nfs_stat(CNFSConnection::Get().GetNfsContext(), fullpath.c_str(), &tmpBuffer);
      resolvedUrl = CNFSConnection::Get().GetConnectedExport() + fullpath;
    }

    if (ret != 0)
    {
      kodi::Log(ADDON_LOG_ERROR, "NFS: Failed to stat(%s) on link resolve %s",
                fullpath.c_str(), nfs_get_error(CNFSConnection::Get().GetNfsContext()));
      retVal = false;
    }
    else
    {
      dirent->inode = tmpBuffer.st_ino;
      dirent->mode = tmpBuffer.st_mode;
      dirent->size = tmpBuffer.st_size;
      dirent->atime.tv_sec = tmpBuffer.st_atime;
      dirent->mtime.tv_sec = tmpBuffer.st_mtime;
      dirent->ctime.tv_sec = tmpBuffer.st_ctime;

      //map stat mode to nf3type
      if(S_ISBLK(tmpBuffer.st_mode)){ dirent->type = NF3BLK; }
      else if(S_ISCHR(tmpBuffer.st_mode)){ dirent->type = NF3CHR; }
      else if(S_ISDIR(tmpBuffer.st_mode)){ dirent->type = NF3DIR; }
      else if(S_ISFIFO(tmpBuffer.st_mode)){ dirent->type = NF3FIFO; }
      else if(S_ISREG(tmpBuffer.st_mode)){ dirent->type = NF3REG; }
      else if(S_ISLNK(tmpBuffer.st_mode)){ dirent->type = NF3LNK; }
      else if(S_ISSOCK(tmpBuffer.st_mode)){ dirent->type = NF3SOCK; }
    }
  }
  else
  {
    kodi::Log(ADDON_LOG_ERROR, "Failed to readlink(%s) %s",
              fullpath.c_str(), nfs_get_error(CNFSConnection::Get().GetNfsContext()));
    retVal = false;
  }
  return retVal;
}

bool CNFSFile::IsValidFile(const std::string& strFileName)
{
  if (strFileName.find('/') == std::string::npos || /* doesn't have sharename */
      strFileName.substr(strFileName.size()-2) == "/." || /* not current folder */
      strFileName.substr(strFileName.size()-3) == "/..")  /* not parent folder */
    return false;
  return true;
}
