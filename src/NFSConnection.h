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

#include <list>
#include <map>
#include <stdint.h>
#include <string>

#include <kodi/addon-instance/VFS.h>
#include <p8-platform/threads/mutex.h>

class CNFSConnection : public P8PLATFORM::CMutex
{
public:
  struct keepAliveStruct
  {
    std::string exportPath;
    uint64_t refreshCounter;
  };
  typedef std::map<struct nfsfh  *, struct keepAliveStruct> tFileKeepAliveMap;

  struct contextTimeout
  {
    struct nfs_context *pContext;
    uint64_t lastAccessedTime;
  };

  typedef std::map<std::string, struct contextTimeout> tOpenContextMap;

  static CNFSConnection& Get();

  virtual ~CNFSConnection();
  bool Connect(const VFSURL& url, std::string& relativePath);
  struct nfs_context *GetNfsContext(){return m_pNfsContext;}
  uint64_t          GetMaxReadChunkSize(){return m_readChunkSize;}
  uint64_t          GetMaxWriteChunkSize(){return m_writeChunkSize;}
  std::list<std::string> GetExportList();
  //this functions splits the url into the exportpath (feed to mount) and the rest of the path
  //relative to the mounted export
  bool splitUrlIntoExportAndPath(const std::string& hostname,
                                 const std::string& filename,
                                 std::string& exportPath, std::string& relativePath);

  //special stat which uses its own context
  //needed for getting intervolume symlinks to work
  int stat(const VFSURL& url, struct stat *statbuff);

  void AddActiveConnection();
  void AddIdleConnection();
  void CheckIfIdle();
  void Deinit();
  //adds the filehandle to the keep alive list or resets
  //the timeout for this filehandle if already in list
  void resetKeepAlive(std::string _exportPath, struct nfsfh  *_pFileHandle);
  //removes file handle from keep alive list
  void removeFromKeepAliveList(struct nfsfh  *_pFileHandle);

  const std::string& GetConnectedIp() const {return m_resolvedHostName;}
  const std::string& GetConnectedExport() const {return m_exportPath;}
  const std::string  GetContextMapId() const {return m_hostName + m_exportPath;}
private:
  CNFSConnection();
  struct nfs_context *m_pNfsContext;//current nfs context
  std::string m_exportPath;//current connected export path
  std::string m_hostName;//current connected host
  std::string m_resolvedHostName;//current connected host - as ip
  uint64_t m_readChunkSize;//current read chunksize of connected server
  uint64_t m_writeChunkSize;//current write chunksize of connected server
  int m_OpenConnections;//number of open connections
  unsigned int m_IdleTimeout;//timeout for idle connection close and dyunload
  tFileKeepAliveMap m_KeepAliveTimeouts;//mapping filehandles to its idle timeout
  tOpenContextMap m_openContextMap;//unique map for tracking all open contexts
  uint64_t m_lastAccessedTime;//last access time for m_pNfsContext
  std::list<std::string> m_exportList;//list of exported paths of current connected servers
  P8PLATFORM::CMutex m_keepAliveLock;
  P8PLATFORM::CMutex m_openContextLock;

  void clearMembers();
  struct nfs_context *getContextFromMap(const std::string& exportname, bool forceCacheHit = false);
  int  getContextForExport(const std::string& exportname);//get context for given export and add to open contexts map - sets m_pNfsContext (my return a already mounted cached context)
  void destroyOpenContexts();
  void destroyContext(const std::string& exportName);
  void resolveHost(const std::string& hostname);//resolve hostname by dnslookup
  void keepAlive(std::string _exportPath, struct nfsfh  *_pFileHandle);
};
