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

struct NFSContext
{
  struct nfsfh* pFileHandle;
  int64_t       size;
  struct nfs_context* pNfsContext;
  std::string exportPath;
  std::string filename;
};

class CNFSFile : public kodi::addon::CInstanceVFS
{
public:
  CNFSFile(KODI_HANDLE instance) : CInstanceVFS(instance) { }
  virtual void* Open(const VFSURL& url) override;
  virtual void* OpenForWrite(const VFSURL& url, bool bOverWrite = false) override;
  virtual ssize_t Read(void* context, void* lpBuf, size_t uiBufSize) override;
  virtual ssize_t Write(void* context, const void* lpBuf, size_t uiBufSize) override;
  virtual int64_t Seek(void* context, int64_t iFilePosition, int iWhence) override;
  virtual int Truncate(void* context, int64_t size) override;
  virtual int64_t GetLength(void* context) override;
  virtual int64_t GetPosition(void* context) override;
  virtual int GetChunkSize(void* context) override {return CNFSConnection::Get().GetMaxReadChunkSize();}
  virtual int IoControl(void* context, XFILE::EIoControl request, void* param) override;
  virtual int Stat(const VFSURL& url, struct __stat64* buffer) override;
  virtual bool Close(void* context) override;
  virtual bool Exists(const VFSURL& url) override;
  virtual void ClearOutIdle();
  virtual void DisconnectAll();
  virtual bool Delete(const VFSURL& url) override;
  virtual bool Rename(const VFSURL& url, const VFSURL& url2) override;
  virtual bool DirectoryExists(const VFSURL& url) override;
  virtual bool RemoveDirectory(const VFSURL& url2) override;
  virtual bool CreateDirectory(const VFSURL& url2) override;
  virtual bool GetDirectory(const VFSURL& url, std::vector<kodi::vfs::CDirEntry>& items, CVFSCallbacks callbacks) override;
protected:
  bool GetDirectoryFromExportList(const std::string& strPath, std::vector<kodi::vfs::CDirEntry>& items);
  bool GetServerList(std::vector<kodi::vfs::CDirEntry>& items);
  bool ResolveSymlink(const VFSURL& url, struct nfsdirent *dirent, std::string& resolvedUrl);
  bool IsValidFile(const std::string& strFileName);
  int64_t m_fileSize = -1;
  struct nfsfh *m_pFileHandle = nullptr;
  struct nfs_context *m_pNfsContext = nullptr;//current nfs context
  std::string m_exportPath;
};

class ATTRIBUTE_HIDDEN CMyAddon : public kodi::addon::CAddonBase
{
public:
  CMyAddon() { }
  virtual ADDON_STATUS CreateInstance(int instanceType, std::string instanceID, KODI_HANDLE instance, KODI_HANDLE& addonInstance) override
  {
    addonInstance = new CNFSFile(instance);
    return ADDON_STATUS_OK;
  }
};

ADDONCREATOR(CMyAddon);
