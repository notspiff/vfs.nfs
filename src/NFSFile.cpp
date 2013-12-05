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

#include "xbmc/libXBMC_addon.h"
#include "xbmc/threads/mutex.h"
#include <fcntl.h>
#include <map>
#include <sstream>
#include <iostream>

extern "C"
{
#include <nfsc/libnfs.h>
#include <nfsc/libnfs-raw-mount.h>
#include <nfsc/libnfs-raw-nfs.h>
}

#include "NFSConnection.h"

ADDON::CHelper_libXBMC_addon *XBMC           = NULL;

extern "C" {

#include "xbmc/xbmc_vfs_dll.h"
#include "xbmc/IFileTypes.h"

static bool GetDirectoryFromExportList(const std::string& strPath, std::vector<VFSDirEntry>& items)
{
  std::string nonConstStrPath(strPath);
  std::list<std::string> exportList=CNFSConnection::Get().GetExportList();
  std::list<std::string>::iterator it;
  
  for(it=exportList.begin();it!=exportList.end();it++)
  {
    std::string currentExport(*it);     
    if (nonConstStrPath.empty() && nonConstStrPath[nonConstStrPath.size()-1] == '/')
      nonConstStrPath.erase(nonConstStrPath.end()-1);
           
    VFSDirEntry pItem;
    pItem.label = strdup(currentExport.c_str());
    std::string path(std::string("nfs://")+nonConstStrPath + currentExport);
    if (path[path.size()-1] != '/')
      path += '/'; 
    pItem.path = strdup(path.c_str());
    pItem.mtime=0;

    pItem.folder = true;
    pItem.properties = NULL;
    pItem.num_props = 0;
    items.push_back(pItem);
  }
  
  return exportList.empty()? false : true;
}

static bool GetServerList(std::vector<VFSDirEntry>& items)
{
  struct nfs_server_list *srvrs;
  struct nfs_server_list *srv;
  bool ret = false;

  srvrs = nfs_find_local_servers();	

  for (srv=srvrs; srv; srv = srv->next) 
  {
    std::string currentExport(srv->addr);

    VFSDirEntry pItem;
    std::string path(std::string("nfs://") + currentExport);
    if (path[path.size()-1] != '/')
      path += '/'; 
    pItem.path = strdup(path.c_str());
    pItem.label = strdup(currentExport.c_str());
    pItem.mtime=0;

    pItem.folder = true;
    pItem.properties = NULL;
    pItem.num_props = 0;
    items.push_back(pItem);
    ret = true; //added at least one entry
  }
  free_nfs_srvr_list(srvrs);

  return ret;
}

static bool ResolveSymlink(VFSURL* url, struct nfsdirent *dirent,
                           std::string& resolvedUrl)
{
  PLATFORM::CLockObject lock(CNFSConnection::Get());
  int ret = 0;  
  bool retVal = true;
  std::string fullpath = url->filename;
  char resolvedLink[MAX_PATH];
  
  if (fullpath[fullpath.size()-1] != '/')
    fullpath += '/'; 
  fullpath.append(dirent->name);
  
  ret = nfs_readlink(CNFSConnection::Get().GetNfsContext(), fullpath.c_str(), resolvedLink, MAX_PATH);    
  
  if(ret == 0)
  {
    struct stat tmpBuffer = {0};      
    fullpath = url->filename;
    if (fullpath[fullpath.size()-1] != '/')
      fullpath += '/'; 
    fullpath.append(resolvedLink);
  
    //special case - if link target is absolute it could be even another export
    //intervolume symlinks baby ...
    if(resolvedLink[0] == '/')
    {    
      //use the special stat function for using an extra context
      //because we are inside of a dir traversation
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
      XBMC->Log(ADDON::LOG_ERROR, "NFS: Failed to stat(%s) on link resolve %s\n",
                fullpath.c_str(), nfs_get_error(CNFSConnection::Get().GetNfsContext()));
      retVal = false;;
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
    XBMC->Log(ADDON::LOG_ERROR, "Failed to readlink(%s) %s\n",
              fullpath.c_str(), nfs_get_error(CNFSConnection::Get().GetNfsContext()));
    retVal = false;
  }
  return retVal;
}

//-- Create -------------------------------------------------------------------
// Called on load. Addon should fully initalize or return error status
//-----------------------------------------------------------------------------
ADDON_STATUS ADDON_Create(void* hdl, void* props)
{
  if (!XBMC)
    XBMC = new ADDON::CHelper_libXBMC_addon;

  if (!XBMC->RegisterMe(hdl))
  {
    delete XBMC, XBMC=NULL;
    return ADDON_STATUS_PERMANENT_FAILURE;
  }

  return ADDON_STATUS_OK;
}

//-- Stop ---------------------------------------------------------------------
// This dll must cease all runtime activities
// !!! Add-on master function !!!
//-----------------------------------------------------------------------------
void ADDON_Stop()
{
}

//-- Destroy ------------------------------------------------------------------
// Do everything before unload of this add-on
// !!! Add-on master function !!!
//-----------------------------------------------------------------------------
void ADDON_Destroy()
{
  XBMC=NULL;
}

//-- HasSettings --------------------------------------------------------------
// Returns true if this add-on use settings
// !!! Add-on master function !!!
//-----------------------------------------------------------------------------
bool ADDON_HasSettings()
{
  return false;
}

//-- GetStatus ---------------------------------------------------------------
// Returns the current Status of this visualisation
// !!! Add-on master function !!!
//-----------------------------------------------------------------------------
ADDON_STATUS ADDON_GetStatus()
{
  return ADDON_STATUS_OK;
}

//-- GetSettings --------------------------------------------------------------
// Return the settings for XBMC to display
// !!! Add-on master function !!!
//-----------------------------------------------------------------------------
unsigned int ADDON_GetSettings(ADDON_StructSetting ***sSet)
{
  return 0;
}

//-- FreeSettings --------------------------------------------------------------
// Free the settings struct passed from XBMC
// !!! Add-on master function !!!
//-----------------------------------------------------------------------------

void ADDON_FreeSettings()
{
}

//-- SetSetting ---------------------------------------------------------------
// Set a specific Setting value (called from XBMC)
// !!! Add-on master function !!!
//-----------------------------------------------------------------------------
ADDON_STATUS ADDON_SetSetting(const char *strSetting, const void* value)
{
  return ADDON_STATUS_OK;
}

//-- Announce -----------------------------------------------------------------
// Receive announcements from XBMC
// !!! Add-on master function !!!
//-----------------------------------------------------------------------------
void ADDON_Announce(const char *flag, const char *sender, const char *message, const void *data)
{
}

struct NFSContext
{
  struct nfsfh* pFileHandle;
  int64_t       size;
  struct nfs_context* pNfsContext;
  std::string exportPath;
  std::string filename;
};

static bool IsValidFile(const std::string& strFileName)
{
  if (strFileName.find('/') == std::string::npos || /* doesn't have sharename */
      strFileName.substr(strFileName.size()-2) == "/." || /* not current folder */
      strFileName.substr(strFileName.size()-3) == "/..")  /* not parent folder */
    return false;
  return true;
}

void* Open(VFSURL* url)
{
  CNFSConnection::Get().AddActiveConnection();
  int ret = 0;
  // we can't open files like nfs://file.f or nfs://server/file.f
  // if a file matches the if below return false, it can't exist on a nfs share.
  if (!IsValidFile(url->filename))
  {
    XBMC->Log(ADDON::LOG_NOTICE,"NFS: Bad URL : '%s'", url->filename);
    return NULL;
  }
  
  std::string filename;

  PLATFORM::CLockObject lock(CNFSConnection::Get());
  
  if(!CNFSConnection::Get().Connect(url, filename))
  {
    return NULL;
  }

  NFSContext* result = new NFSContext;
  
  result->pNfsContext = CNFSConnection::Get().GetNfsContext(); 
  result->exportPath = CNFSConnection::Get().GetContextMapId();
  
  ret = nfs_open(result->pNfsContext, filename.c_str(), O_RDONLY, &result->pFileHandle);
  
  if (ret != 0) 
  {
    XBMC->Log(ADDON::LOG_INFO, "CNFSFile::Open: Unable to open file : '%s'  error : '%s'", url, nfs_get_error(result->pNfsContext));
    delete result;
    return NULL;
  } 
  
  XBMC->Log(ADDON::LOG_DEBUG,"CNFSFile::Open - opened %s", filename.c_str());
  result->filename = url->filename;
  
  struct __stat64 tmpBuffer;

  if( Stat(url, &tmpBuffer) )
  {
    Close(result);
    return NULL;
  }
  
  result->size = tmpBuffer.st_size;//cache the size of this file

  // We've successfully opened the file!
  return result;
}

unsigned int Read(void* context, void* lpBuf, int64_t uiBufSize)
{
  NFSContext* ctx = (NFSContext*)context;
  if (!ctx || !ctx->pFileHandle|| !ctx->pNfsContext)
    return 0;

  int numberOfBytesRead = 0;
  PLATFORM::CLockObject lock(CNFSConnection::Get());
  numberOfBytesRead = nfs_read(ctx->pNfsContext, ctx->pFileHandle, uiBufSize, (char *)lpBuf);  
  
  CNFSConnection::Get().resetKeepAlive(ctx->exportPath, ctx->pFileHandle);//triggers keep alive timer reset for this filehandle
  
  //something went wrong ...
  if (numberOfBytesRead < 0) 
  {
    XBMC->Log(ADDON::LOG_ERROR, "%s - Error( %d, %s )", __FUNCTION__, numberOfBytesRead, nfs_get_error(ctx->pNfsContext));
    return 0;
  }
  return (unsigned int)numberOfBytesRead;
}

bool Close(void* context)
{
  NFSContext* ctx = (NFSContext*)context;
  if (!ctx)
    return false;

  PLATFORM::CLockObject lock(CNFSConnection::Get());
  CNFSConnection::Get().AddIdleConnection();
  
  if (ctx->pFileHandle != NULL && ctx->pNfsContext != NULL)
  {
    int ret = 0;
    XBMC->Log(ADDON::LOG_DEBUG,"CNFSFile::Close closing file %s", ctx->filename.c_str());
    // remove it from keep alive list before closing
    // so keep alive code doens't process it anymore
    CNFSConnection::Get().removeFromKeepAliveList(ctx->pFileHandle);
    ret = nfs_close(ctx->pNfsContext, ctx->pFileHandle);
        
    if (ret < 0) 
    {
      XBMC->Log(ADDON::LOG_ERROR, "Failed to close(%s) - %s\n", ctx->filename.c_str(), nfs_get_error(ctx->pNfsContext));
    }
  }

  delete ctx;

  return true;
}

int64_t GetLength(void* context)
{
  if (!context)
    return 0;

  NFSContext* ctx = (NFSContext*)context;
  return ctx->size;
}

//*********************************************************************************************
int64_t GetPosition(void* context)
{
  if (!context)
    return 0;

  NFSContext* ctx = (NFSContext*)context;
  int ret = 0;
  uint64_t offset = 0;
  
  if (CNFSConnection::Get().GetNfsContext() == NULL || ctx->pFileHandle == NULL)
    return 0;

  PLATFORM::CLockObject lock(CNFSConnection::Get());
  
  ret = (int)nfs_lseek(CNFSConnection::Get().GetNfsContext(), ctx->pFileHandle, 0, SEEK_CUR, &offset);
  
  if (ret < 0) 
  {
    XBMC->Log(ADDON::LOG_ERROR, "NFS: Failed to lseek(%s)",nfs_get_error(CNFSConnection::Get().GetNfsContext()));
  }
  return offset;
}


int64_t Seek(void* context, int64_t iFilePosition, int iWhence)
{
  NFSContext* ctx = (NFSContext*)context;
  if (!ctx || !ctx->pFileHandle|| !ctx->pNfsContext)
    return 0;

  int ret = 0;
  uint64_t offset = 0;

  PLATFORM::CLockObject lock(CNFSConnection::Get());
  
  ret = (int)nfs_lseek(ctx->pNfsContext, ctx->pFileHandle, iFilePosition, iWhence, &offset);
  if (ret < 0) 
  {
    XBMC->Log(ADDON::LOG_ERROR, "%s - Error( seekpos: %"PRId64", whence: %i, fsize: %"PRId64", %s)",
              __FUNCTION__, iFilePosition, iWhence, ctx->size, nfs_get_error(ctx->pNfsContext));
    return -1;
  }
  return (int64_t)offset;
}

bool Exists(VFSURL* url)
{
  return Stat(url, NULL) == 0;
}

int Stat(VFSURL* url, struct __stat64* buffer)
{
  int ret = 0;
  PLATFORM::CLockObject lock(CNFSConnection::Get());
  std::string filename;
  
  if(!CNFSConnection::Get().Connect(url, filename))
  {
    return -1;
  }
   
  struct stat tmpBuffer = {0};

  ret = nfs_stat(CNFSConnection::Get().GetNfsContext(), filename.c_str(), &tmpBuffer);
  
  //if buffer == NULL we where called from Exists - in that case don't spam the log with errors
  if (ret != 0 && buffer != NULL) 
  {
    XBMC->Log(ADDON::LOG_ERROR, "NFS: Failed to stat(%s) %s\n", url->filename, nfs_get_error(CNFSConnection::Get().GetNfsContext()));
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

int IoControl(void* context, XFILE::EIoControl request, void* param)
{
  if(request == XFILE::IOCTRL_SEEK_POSSIBLE)
    return 1;

  return -1;
}

void ClearOutIdle()
{
  CNFSConnection::Get().CheckIfIdle();
}

void DisconnectAll()
{
  CNFSConnection::Get().Deinit();
}

bool DirectoryExists(VFSURL* url)
{
  int ret = 0;

  PLATFORM::CLockObject lock(CNFSConnection::Get());
  std::string folderName(url->filename);
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

void* GetDirectory(VFSURL* url,VFSDirEntry** items, int* num_items)
{
  PLATFORM::CLockObject lock(CNFSConnection::Get());
  CNFSConnection::Get().AddActiveConnection();
  // We accept nfs://server/path[/file]]]]
  int ret = 0;
  uint64_t fileTime, localTime;    
  std::string strDirName;
  std::string myStrPath(url->filename);
  std::vector<VFSDirEntry>* itms = new std::vector<VFSDirEntry>;
  if (myStrPath[myStrPath.size()-1] != '/')
  {
    myStrPath += '/';
    url->filename = myStrPath.c_str();
  }
  if(!CNFSConnection::Get().Connect(url, strDirName))
  {
    //connect has failed - so try to get the exported filesystms if no path is given to the url
    if (myStrPath == "/")
    {
      if(strcmp(url->hostname, "") == 0)
      {
        GetServerList(*itms);
      }
      else 
      {
        GetDirectoryFromExportList(myStrPath, *itms);
      }
      if (!itms->empty())
        *items = &(*itms)[0];
      *num_items = itms->size();
      return itms->empty()?NULL:itms;
    }
  }
      
  struct nfsdir *nfsdir = NULL;
  struct nfsdirent *nfsdirent = NULL;

  ret = nfs_opendir(CNFSConnection::Get().GetNfsContext(), strDirName.c_str(), &nfsdir);

  if(ret != 0)
  {
    XBMC->Log(ADDON::LOG_ERROR, "Failed to open(%s) %s\n", strDirName.c_str(), nfs_get_error(CNFSConnection::Get().GetNfsContext()));
    return NULL;
  }
  lock.Unlock();
  
  while((nfsdirent = nfs_readdir(CNFSConnection::Get().GetNfsContext(), nfsdir)) != NULL) 
  {
    std::string strName = nfsdirent->name;
    std::string path(std::string("nfs://")+url->url + strName);    
    int64_t iSize = 0;
    bool bIsDir = false;
    int64_t lTimeDate = 0;

    //reslove symlinks
    if(nfsdirent->type == NF3LNK)
    {
      //resolve symlink changes nfsdirent and strName
      if(!ResolveSymlink(url, nfsdirent, path))
      { 
        continue;
      }
    }
    
    iSize = nfsdirent->size;
    bIsDir = nfsdirent->type == NF3DIR;
    lTimeDate = nfsdirent->mtime.tv_sec;

    if (strName != "." && strName != ".."
                       && strName != "lost+found")
    {
      if(lTimeDate == 0) // if modification date is missing, use create date
      {
        lTimeDate = nfsdirent->ctime.tv_sec;
      }

      int64_t localTime = ((lTimeDate & 0xffffffffff) << 32) + 10000000 + 116444736000000000ll; // FIXME

      VFSDirEntry pItem;
      pItem.label = strdup(nfsdirent->name);
      pItem.mtime=localTime;   
      pItem.size = iSize;        
      
      if (bIsDir)
      {
        if (path[path.size()-1] != '/')
          path += '/';
        pItem.folder = true;
      }
      else
      {
        pItem.folder = false;
      }

      if (strName[0] == '.')
      {
        pItem.properties = new VFSProperty;
        pItem.properties->name = strdup("file:hidden");
        pItem.properties->val = strdup("true");
        pItem.num_props = 1;
      }
      else
      {
        pItem.properties = NULL;
        pItem.num_props = 0;
      }
      pItem.path = strdup(path.c_str());
      itms->push_back(pItem);
    }
  }

  lock.Lock();
  nfs_closedir(CNFSConnection::Get().GetNfsContext(), nfsdir);//close the dir

  if (!itms->empty())
    *items = &(*itms)[0];
  *num_items = itms->size();
  return itms->empty()?NULL:itms;
}

void FreeDirectory(void* items)
{
  CNFSConnection::Get().AddIdleConnection();
  std::vector<VFSDirEntry>& ctx = *(std::vector<VFSDirEntry>*)items;
  for (size_t i=0;i<ctx.size();++i)
  {
    free(ctx[i].label);
    for (size_t j=0;j<ctx[i].num_props;++j)
    {
      free(ctx[i].properties[j].name);
      free(ctx[i].properties[j].val);
    }
    delete ctx[i].properties;
    free(ctx[i].path);
  }
  delete &ctx;
}

bool CreateDirectory(VFSURL* url)
{
  int ret = 0;
  bool success=true;
  
  PLATFORM::CLockObject lock(CNFSConnection::Get());
  std::string folderName(url->filename);
  if (folderName[folderName.size()-1] == '/')
  {
    folderName.erase(folderName.end()-1);
    url->filename = folderName.c_str();
  }
  
  if(!CNFSConnection::Get().Connect(url, folderName))
  {
    return false;
  }
  
  ret = nfs_mkdir(CNFSConnection::Get().GetNfsContext(), folderName.c_str());

  success = (ret == 0 || -EEXIST == ret);
  if(!success)
    XBMC->Log(ADDON::LOG_ERROR, "NFS: Failed to create(%s) %s\n",
              folderName.c_str(), nfs_get_error(CNFSConnection::Get().GetNfsContext()));

  return success;
}

bool RemoveDirectory(VFSURL* url)
{
  int ret = 0;

  PLATFORM::CLockObject lock(CNFSConnection::Get());
  std::string folderName(url->filename);
  if (folderName[folderName.size()-1] == '/')
  {
    folderName.erase(folderName.end()-1);
    url->filename = folderName.c_str();
  }
  
  if(!CNFSConnection::Get().Connect(url, folderName))
  {
    return false;
  }
  
  ret = nfs_rmdir(CNFSConnection::Get().GetNfsContext(), folderName.c_str());

  if(ret != 0 && errno != ENOENT)
  {
    XBMC->Log(ADDON::LOG_ERROR, "%s - Error( %s )", __FUNCTION__,
              nfs_get_error(CNFSConnection::Get().GetNfsContext()));
    return false;
  }
  return true;
}

int Truncate(void* context, int64_t size)
{
  NFSContext* ctx = (NFSContext*)context;
  if (!ctx || !ctx->pFileHandle|| !ctx->pNfsContext)
    return -1;

  int ret = 0;
  PLATFORM::CLockObject lock(CNFSConnection::Get());
  ret = (int)nfs_ftruncate(ctx->pNfsContext, ctx->pFileHandle, size);
  if (ret < 0) 
  {
    XBMC->Log(ADDON::LOG_ERROR, "%s - Error( ftruncate: %"PRId64", fsize: %"PRId64", %s)",
              __FUNCTION__, size, ctx->size, nfs_get_error(ctx->pNfsContext));
    return -1;
  }
  return ret;
}

//this was a bitch!
//for nfs write to work we have to write chunked
//otherwise this could crash on big files
int Write(void* context, const void* lpBuf, int64_t uiBufSize)
{
  NFSContext* ctx = (NFSContext*)context;
  if (!ctx || !ctx->pFileHandle|| !ctx->pNfsContext)
    return -1;

  int numberOfBytesWritten = 0;
  int writtenBytes = 0;
  int64_t leftBytes = uiBufSize;
  //clamp max write chunksize to 32kb - fixme - this might be superfluious with future libnfs versions
  int64_t chunkSize = CNFSConnection::Get().GetMaxWriteChunkSize() > 32768 ? 32768 : CNFSConnection::Get().GetMaxWriteChunkSize();
  
  PLATFORM::CLockObject lock(CNFSConnection::Get());
  
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
      XBMC->Log(ADDON::LOG_ERROR, "Failed to pwrite(%s) %s\n",
                ctx->filename.c_str(), nfs_get_error(ctx->pNfsContext));
      break;
    }     
  }

  //return total number of written bytes
  return numberOfBytesWritten;
}

bool Delete(VFSURL* url)
{
  int ret = 0;
  PLATFORM::CLockObject lock(CNFSConnection::Get());
  std::string filename;
  
  if(!CNFSConnection::Get().Connect(url, filename))
  {
    return false;
  }
  
  ret = nfs_unlink(CNFSConnection::Get().GetNfsContext(), filename.c_str());
  
  if(ret != 0)
  {
    XBMC->Log(ADDON::LOG_ERROR, "%s - Error( %s )", __FUNCTION__,
              nfs_get_error(CNFSConnection::Get().GetNfsContext()));
  }

  return (ret == 0);
}

bool Rename(VFSURL* url, VFSURL* url2)
{
  int ret = 0;
  PLATFORM::CLockObject lock(CNFSConnection::Get());
  std::string strFile;
  
  if(!CNFSConnection::Get().Connect(url, strFile))
  {
    return false;
  }
  
  std::string strFileNew;
  std::string strDummy;
  CNFSConnection::Get().splitUrlIntoExportAndPath(url2->hostname, url2->filename, strDummy, strFileNew);
  
  ret = nfs_rename(CNFSConnection::Get().GetNfsContext(), strFile.c_str(), strFileNew.c_str());
  
  if(ret != 0)
  {
    XBMC->Log(ADDON::LOG_ERROR, "%s - Error( %s )",
              __FUNCTION__, nfs_get_error(CNFSConnection::Get().GetNfsContext()));
  } 

  return (ret == 0);
}

void* OpenForWrite(VFSURL* url, bool bOverWrite)
{ 
  int ret = 0;
  // we can't open files like nfs://file.f or nfs://server/file.f
  // if a file matches the if below return false, it can't exist on a nfs share.
  if (!IsValidFile(url->filename))
    return NULL;
  
  PLATFORM::CLockObject lock(CNFSConnection::Get());
  std::string filename;
  
  if(!CNFSConnection::Get().Connect(url, filename))
  {
    return NULL;
  }
  
  NFSContext* result = new NFSContext;
  result->pNfsContext = CNFSConnection::Get().GetNfsContext();
  result->exportPath = CNFSConnection::Get().GetContextMapId();
  
  if (bOverWrite)
  {
    XBMC->Log(ADDON::LOG_INFO, "FileNFS::OpenForWrite() called with overwriting enabled! - %s", filename.c_str());
    //create file with proper permissions
    ret = nfs_creat(result->pNfsContext, filename.c_str(), S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH, &result->pFileHandle);    
    //if file was created the file handle isn't valid ... so close it and open later
    if(ret == 0)
    {
      nfs_close(result->pNfsContext, result->pFileHandle);
      result->pFileHandle = NULL;          
    }
  }

  ret = nfs_open(result->pNfsContext, filename.c_str(), O_RDWR, &result->pFileHandle);
  
  if (ret || result->pFileHandle == NULL)
  {
    // write error to logfile
    XBMC->Log(ADDON::LOG_ERROR, "CNFSFile::Open: Unable to open file : '%s' error : '%s'",
              filename.c_str(), nfs_get_error(CNFSConnection::Get().GetNfsContext()));
    delete result;
    return NULL;
  }
  result->filename = url->filename;
  
  //only stat if file was not created
  if(!bOverWrite) 
  {
    struct __stat64 tmpBuffer;

    if( Stat(url, &tmpBuffer) )
    {
      Close(result);
      return NULL;
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

void* ContainsFiles(VFSURL* url, VFSDirEntry** items, int* num_items)
{
  return NULL;
}

int GetStartTime(void* ctx)
{
  return 0;
}

int GetTotalTime(void* ctx)
{
  return 0;
}

bool NextChannel(void* context, bool preview)
{
  return false;
}

bool PrevChannel(void* context, bool preview)
{
  return false;
}

bool SelectChannel(void* context, unsigned int uiChannel)
{
  return false;
}

bool UpdateItem(void* context)
{
  return false;
}

int GetChunkSize(void* context)
{
  return 1;
}

}
