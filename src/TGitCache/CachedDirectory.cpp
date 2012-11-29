// TortoiseGit - a Windows shell extension for easy version control

// External Cache Copyright (C) 2005-2008 - TortoiseSVN
// Copyright (C) 2008-2012 - TortoiseGit

// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software Foundation,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
//
#include "StdAfx.h"
#include ".\cacheddirectory.h"
//#include "SVNHelpers.h"
#include "GitStatusCache.h"
#include "GitStatus.h"
#include <set>

CCachedDirectory::CCachedDirectory(void)
{
	m_currentFullStatus = m_mostImportantFileStatus = git_wc_status_none;
	m_bRecursive = true;
}

CCachedDirectory::~CCachedDirectory(void)
{
}

CCachedDirectory::CCachedDirectory(const CTGitPath& directoryPath)
{
	ATLASSERT(directoryPath.IsDirectory() || !PathFileExists(directoryPath.GetWinPath()));

	m_directoryPath = directoryPath;
	m_directoryPath.GetGitPathString(); // make sure git path string is set

	m_currentFullStatus = m_mostImportantFileStatus = git_wc_status_none;
	m_bRecursive = true;
}

BOOL CCachedDirectory::SaveToDisk(FILE * pFile)
{
	AutoLocker lock(m_critSec);
#define WRITEVALUETOFILE(x) if (fwrite(&x, sizeof(x), 1, pFile)!=1) return false;

	unsigned int value = GIT_CACHE_VERSION;
	WRITEVALUETOFILE(value);	// 'version' of this save-format
	value = (int)m_entryCache.size();
	WRITEVALUETOFILE(value);	// size of the cache map
	// now iterate through the maps and save every entry.
	for (CacheEntryMap::iterator I = m_entryCache.begin(); I != m_entryCache.end(); ++I)
	{
		const CString& key = I->first;
		value = key.GetLength();
		WRITEVALUETOFILE(value);
		if (value)
		{
			if (fwrite((LPCTSTR)key, sizeof(TCHAR), value, pFile)!=value)
				return false;
			if (!I->second.SaveToDisk(pFile))
				return false;
		}
	}
	value = (int)m_childDirectories.size();
	WRITEVALUETOFILE(value);
	for (ChildDirStatus::iterator I = m_childDirectories.begin(); I != m_childDirectories.end(); ++I)
	{
		const CString& path = I->first.GetWinPathString();
		value = path.GetLength();
		WRITEVALUETOFILE(value);
		if (value)
		{
			if (fwrite((LPCTSTR)path, sizeof(TCHAR), value, pFile)!=value)
				return false;
			git_wc_status_kind status = I->second;
			WRITEVALUETOFILE(status);
		}
	}
//	WRITEVALUETOFILE(m_propsFileTime);
	value = m_directoryPath.GetWinPathString().GetLength();
	WRITEVALUETOFILE(value);
	if (value)
	{
		if (fwrite(m_directoryPath.GetWinPath(), sizeof(TCHAR), value, pFile)!=value)
			return false;
	}
	if (!m_ownStatus.SaveToDisk(pFile))
		return false;
	WRITEVALUETOFILE(m_currentFullStatus);
	WRITEVALUETOFILE(m_mostImportantFileStatus);
	return true;
}

BOOL CCachedDirectory::LoadFromDisk(FILE * pFile)
{
	AutoLocker lock(m_critSec);
#define LOADVALUEFROMFILE(x) if (fread(&x, sizeof(x), 1, pFile)!=1) return false;
	try
	{
		unsigned int value = 0;
		LOADVALUEFROMFILE(value);
		if (value != GIT_CACHE_VERSION)
			return false;		// not the correct version
		int mapsize = 0;
		LOADVALUEFROMFILE(mapsize);
		for (int i=0; i<mapsize; ++i)
		{
			LOADVALUEFROMFILE(value);
			if (value > MAX_PATH)
				return false;
			if (value)
			{
				CString sKey;
				if (fread(sKey.GetBuffer(value+1), sizeof(TCHAR), value, pFile)!=value)
				{
					sKey.ReleaseBuffer(0);
					return false;
				}
				sKey.ReleaseBuffer(value);
				CStatusCacheEntry entry;
				if (!entry.LoadFromDisk(pFile))
					return false;
				// only read non empty keys (just needed for transition from old TGit clients)
				if (!sKey.IsEmpty())
					m_entryCache[sKey] = entry;
			}
		}
		LOADVALUEFROMFILE(mapsize);
		for (int i=0; i<mapsize; ++i)
		{
			LOADVALUEFROMFILE(value);
			if (value > MAX_PATH)
				return false;
			if (value)
			{
				CString sPath;
				if (fread(sPath.GetBuffer(value), sizeof(TCHAR), value, pFile)!=value)
				{
					sPath.ReleaseBuffer(0);
					return false;
				}
				sPath.ReleaseBuffer(value);
				git_wc_status_kind status;
				LOADVALUEFROMFILE(status);
				m_childDirectories[CTGitPath(sPath)] = status;
			}
		}
		LOADVALUEFROMFILE(value);
		if (value > MAX_PATH)
			return false;
		if (value)
		{
			CString sPath;
			if (fread(sPath.GetBuffer(value+1), sizeof(TCHAR), value, pFile)!=value)
			{
				sPath.ReleaseBuffer(0);
				return false;
			}
			sPath.ReleaseBuffer(value);
			// make sure paths do not end with backslash (just needed for transition from old TGit clients)
			if (sPath.GetLength() > 3 && sPath[sPath.GetLength() - 1] == _T('\\'))
				sPath.TrimRight(_T("\\"));
			m_directoryPath.SetFromWin(sPath);
			m_directoryPath.GetGitPathString(); // make sure git path string is set
		}
		if (!m_ownStatus.LoadFromDisk(pFile))
			return false;

		LOADVALUEFROMFILE(m_currentFullStatus);
		LOADVALUEFROMFILE(m_mostImportantFileStatus);
	}
	catch ( CAtlException )
	{
		return false;
	}
	return true;

}


CStatusCacheEntry CCachedDirectory::GetStatusFromCache(const CTGitPath& path, bool bRecursive)
{
	if(path.IsDirectory())
	{
		// We don't have directory status in our cache
		// Ask the directory if it knows its own status
		CCachedDirectory * dirEntry = CGitStatusCache::Instance().GetDirectoryCacheEntry(path);
		if( dirEntry)
		{
			if (dirEntry->IsOwnStatusValid())
				return dirEntry->GetOwnStatus(bRecursive);
			else
			{
				/* cache have outof date, need crawl again*/

				/*AutoLocker lock(dirEntry->m_critSec);
				ChildDirStatus::const_iterator it;
				for(it = dirEntry->m_childDirectories.begin(); it != dirEntry->m_childDirectories.end(); ++it)
				{
					CGitStatusCache::Instance().AddFolderForCrawling(it->first);
				}*/

				CGitStatusCache::Instance().AddFolderForCrawling(path);

				/*Return old status during crawling*/
				return dirEntry->GetOwnStatus(bRecursive);
			}
		}
		else
		{
			CGitStatusCache::Instance().AddFolderForCrawling(path);
		}
		return CStatusCacheEntry();
	}
	else
	{
		//All file ignored if under ignore directory
		if (m_ownStatus.GetEffectiveStatus() == git_wc_status_ignored)
			return CStatusCacheEntry(git_wc_status_ignored);
		if (m_ownStatus.GetEffectiveStatus() == git_wc_status_unversioned)
			return CStatusCacheEntry(git_wc_status_unversioned);

		// Look up a file in our own cache
		AutoLocker lock(m_critSec);
		CString strCacheKey = GetCacheKey(path);
		CacheEntryMap::iterator itMap = m_entryCache.find(strCacheKey);
		if(itMap != m_entryCache.end())
		{
			// We've hit the cache - check for timeout
			if(!itMap->second.HasExpired((long)GetTickCount()))
			{
				if(itMap->second.DoesFileTimeMatch(path.GetLastWriteTime()))
				{
					if ((itMap->second.GetEffectiveStatus()!=git_wc_status_missing)||(!PathFileExists(path.GetWinPath())))
					{
						// Note: the filetime matches after a modified has been committed too.
						// So in that case, we would return a wrong status (e.g. 'modified' instead
						// of 'normal') here.
						return itMap->second;
					}
				}
			}
		}

		CGitStatusCache::Instance().AddFolderForCrawling(path.GetContainingDirectory());
		return CStatusCacheEntry();
	}

}

CStatusCacheEntry CCachedDirectory::GetStatusFromGit(const CTGitPath &path, CString sProjectRoot)
{
	CString subpaths = path.GetGitPathString();
	if(subpaths.GetLength() >= sProjectRoot.GetLength())
	{
		if(subpaths[sProjectRoot.GetLength()] == _T('/'))
			subpaths=subpaths.Right(subpaths.GetLength() - sProjectRoot.GetLength()-1);
		else
			subpaths=subpaths.Right(subpaths.GetLength() - sProjectRoot.GetLength());
	}

	GitStatus *pGitStatus = &CGitStatusCache::Instance().m_GitStatus;
	UNREFERENCED_PARAMETER(pGitStatus);

	CGitHash head;

	pGitStatus->GetHeadHash(sProjectRoot,head);

	bool isVersion =true;
	pGitStatus->IsUnderVersionControl(sProjectRoot, subpaths, path.IsDirectory(), &isVersion);
	if(!isVersion)
	{	//untracked file
		bool isIgnoreFileChanged=false;

		isIgnoreFileChanged = pGitStatus->IsGitReposChanged(sProjectRoot, subpaths, GIT_MODE_IGNORE);

		if( isIgnoreFileChanged)
		{
			pGitStatus->LoadIgnoreFile(sProjectRoot, subpaths);
		}

		if(path.IsDirectory())
		{

			CCachedDirectory * dirEntry = CGitStatusCache::Instance().GetDirectoryCacheEntry(path,
											false); /* we needn't watch untracked directory*/

			if(dirEntry)
			{
				AutoLocker lock(dirEntry->m_critSec);

				git_wc_status_kind dirstatus = dirEntry->GetCurrentFullStatus() ;
				if( dirstatus == git_wc_status_none || dirstatus >= git_wc_status_normal || isIgnoreFileChanged )
				{/* status have not initialized*/
					git_wc_status2_t status2;
					bool isignore = false;
					pGitStatus->IsIgnore(sProjectRoot,subpaths,&isignore);
					status2.text_status = status2.prop_status =
						(isignore? git_wc_status_ignored:git_wc_status_unversioned);

					dirEntry->m_ownStatus.SetKind(git_node_dir);
					dirEntry->m_ownStatus.SetStatus(&status2);

				}
				return dirEntry->m_ownStatus;
			}

		}
		else /* path is file */
		{
			AutoLocker lock(m_critSec);
			CString strCacheKey = GetCacheKey(path);

			if (strCacheKey.IsEmpty())
				return CStatusCacheEntry();

			CacheEntryMap::iterator itMap = m_entryCache.find(strCacheKey);
			if(itMap == m_entryCache.end() || isIgnoreFileChanged)
			{
				git_wc_status2_t status2;
				bool isignore = false;
				pGitStatus->IsIgnore(sProjectRoot,subpaths,&isignore);
				status2.text_status = status2.prop_status =
					(isignore? git_wc_status_ignored:git_wc_status_unversioned);
				AddEntry(path, &status2);
				return m_entryCache[strCacheKey];
			}
			else
			{
				return itMap->second;
			}
		}
		return CStatusCacheEntry();

	}
	else
	{
		EnumFiles((CTGitPath*)&path, TRUE);
		UpdateCurrentStatus();
		return CStatusCacheEntry(m_ownStatus);
	}

}

/// bFetch is true, fetch all status, call by crawl.
/// bFetch is false, get cache status, return quickly.

CStatusCacheEntry CCachedDirectory::GetStatusForMember(const CTGitPath& path, bool bRecursive,  bool bFetch /* = true */)
{
	CString sProjectRoot;
	bool bIsVersionedPath;

	bool bRequestForSelf = false;
	if(path.IsEquivalentToWithoutCase(m_directoryPath))
	{
		bRequestForSelf = true;
		AutoLocker lock(m_critSec);
		// HasAdminDir might modify m_directoryPath, so we need to do it synchronized
		bIsVersionedPath = m_directoryPath.HasAdminDir(&sProjectRoot);
	}
	else
		bIsVersionedPath = path.HasAdminDir(&sProjectRoot);

	// In all most circumstances, we ask for the status of a member of this directory.
	ATLASSERT(m_directoryPath.IsEquivalentToWithoutCase(path.GetContainingDirectory()) || bRequestForSelf);

	//If is not version control path
	if( !bIsVersionedPath)
	{
		ATLTRACE(_T("%s is not underversion control\n"), path.GetWinPath());
		return CStatusCacheEntry();
	}

	// We've not got this item in the cache - let's add it
	// We never bother asking SVN for the status of just one file, always for its containing directory

	if (g_GitAdminDir.IsAdminDirPath(path.GetWinPathString()))
	{
		// We're being asked for the status of an .git directory
		// It's not worth asking for this
		return CStatusCacheEntry();
	}


	if(bFetch)
	{
		return GetStatusFromGit(path, sProjectRoot);
	}
	else
	{
		return GetStatusFromCache(path, bRecursive);
	}
}

CStatusCacheEntry CCachedDirectory::GetCacheStatusForMember(const CTGitPath& path)
{
	// no disk access!
	AutoLocker lock(m_critSec);
	CacheEntryMap::iterator itMap = m_entryCache.find(GetCacheKey(path));
	if(itMap != m_entryCache.end())
		return itMap->second;

	return CStatusCacheEntry();
}

int CCachedDirectory::EnumFiles(CTGitPath *path , bool IsFull)
{
	CString sProjectRoot;
	if(path)
		path->HasAdminDir(&sProjectRoot);
	else
		m_directoryPath.HasAdminDir(&sProjectRoot);

	ATLTRACE(_T("EnumFiles %s\n"), path->GetWinPath());

	ATLASSERT( !m_directoryPath.IsEmpty() );

	CString sSubPath;

	CString s;
	if(path)
		s=path->GetWinPath();
	else
		s=m_directoryPath.GetDirectory().GetWinPathString();

	if (s.GetLength() > sProjectRoot.GetLength())
	{
		// skip initial slash if necessary
		if(s[sProjectRoot.GetLength()] == _T('\\'))
			sSubPath = s.Right(s.GetLength() - sProjectRoot.GetLength() -1);
		else
			sSubPath = s.Right(s.GetLength() - sProjectRoot.GetLength() );
	}

	// strip "\" at the end, otherwise cache lookups for drives do not work correctly
	sProjectRoot.TrimRight(_T("\\"));

	GitStatus *pStatus = &CGitStatusCache::Instance().m_GitStatus;
	UNREFERENCED_PARAMETER(pStatus);
	git_wc_status_kind status = git_wc_status_none;

	if(!path->IsDirectory())
	{
		bool assumeValid = false;
		bool skipWorktree = false;
		pStatus->GetFileStatus(sProjectRoot, sSubPath, &status, IsFull, false, true, GetStatusCallback, this, &assumeValid, &skipWorktree);
	}
	else
	{
		m_mostImportantFileStatus = git_wc_status_none;
		pStatus->EnumDirStatus(sProjectRoot, sSubPath, &status, IsFull, false, true, GetStatusCallback,this);

		// folders must not be displayed as added or deleted only as modified
		if (status == git_wc_status_deleted || status == git_wc_status_added)
			status = git_wc_status_modified;
		// this is necessary, even if the m_mostImportantFileStatus is set in GetStatusCallback,
		// however, if it's missing, deleted subdirectories won't mark parents as modified
		m_mostImportantFileStatus = GitStatus::GetMoreImportant(m_mostImportantFileStatus, status);

		if (sSubPath.IsEmpty())
		{
			// request a shell notification for working trees which have not been examined before
			if (m_currentFullStatus == git_wc_status_none)
			{
				m_currentFullStatus = git_wc_status_normal;
				CGitStatusCache::Instance().UpdateShell(sProjectRoot);
			}
			m_ownStatus = git_wc_status_normal;
		}
		// otherwise folders which contain at least one ignored item gets displayed as ignored
		// or: folders which were displayed as conflicted never recover from that state
		else if (m_ownStatus.GetEffectiveStatus() == git_wc_status_ignored || m_ownStatus.GetEffectiveStatus() == git_wc_status_conflicted)
			m_ownStatus = git_wc_status_normal;
	}

	return 0;
}
void
CCachedDirectory::AddEntry(const CTGitPath& path, const git_wc_status2_t* pGitStatus, DWORD validuntil /* = 0*/)
{
	AutoLocker lock(m_critSec);
	if(path.IsDirectory())
	{
		CCachedDirectory * childDir = CGitStatusCache::Instance().GetDirectoryCacheEntry(path);
		if (childDir)
		{
			if ((childDir->GetCurrentFullStatus() != git_wc_status_missing)||(pGitStatus==NULL)||(pGitStatus->text_status != git_wc_status_unversioned))
			{
				if(pGitStatus)
				{
					if(childDir->GetCurrentFullStatus() != GitStatus::GetMoreImportant(pGitStatus->prop_status, pGitStatus->text_status))
					{
						CGitStatusCache::Instance().UpdateShell(path);
						//ATLTRACE(_T("shell update for %s\n"), path.GetWinPath());
						childDir->m_ownStatus.SetKind(git_node_dir);
						childDir->m_ownStatus.SetStatus(pGitStatus);
					}
				}
			}
			childDir->m_ownStatus.SetKind(git_node_dir);


		}
	}
	else
	{
		CCachedDirectory * childDir = CGitStatusCache::Instance().GetDirectoryCacheEntry(path.GetContainingDirectory());
		bool bNotified = false;

		if(!childDir)
			return ;

		AutoLocker lock(childDir->m_critSec);
		CString cachekey = GetCacheKey(path);
		CacheEntryMap::iterator entry_it = childDir->m_entryCache.lower_bound(cachekey);
		if (entry_it != childDir->m_entryCache.end() && entry_it->first == cachekey)
		{
			if (pGitStatus)
			{
				if (entry_it->second.GetEffectiveStatus() > git_wc_status_none &&
					entry_it->second.GetEffectiveStatus() != GitStatus::GetMoreImportant(pGitStatus->prop_status, pGitStatus->text_status)
				)
				{
					bNotified =true;
				}
			}

		}
		else
		{
			entry_it = childDir->m_entryCache.insert(entry_it, std::make_pair(cachekey, CStatusCacheEntry()));
			bNotified = true;

		}
		entry_it->second = CStatusCacheEntry(pGitStatus, path.GetLastWriteTime(), path.IsReadOnly(), validuntil);
		// TEMP(?): git status doesn't not have "entry" that contains node type, so manually set as file
		entry_it->second.SetKind(git_node_file);

		if(bNotified)
		{
			CGitStatusCache::Instance().UpdateShell(path);
			//ATLTRACE(_T("shell update for %s\n"), path.GetWinPath());
		}

		//ATLTRACE(_T("Path Entry Add %s %s %s %d\n"), path.GetWinPath(), cachekey, m_directoryPath.GetWinPath(), pGitStatus->text_status);
	}

	CCachedDirectory * parent = CGitStatusCache::Instance().GetDirectoryCacheEntry(path.GetContainingDirectory());

	if(parent)
	{
		if ((parent->GetCurrentFullStatus() != git_wc_status_missing)||(pGitStatus==NULL)||(pGitStatus->text_status != git_wc_status_unversioned))
		{
			if(pGitStatus)
			{
				if(parent->GetCurrentFullStatus() < GitStatus::GetMoreImportant(pGitStatus->prop_status, pGitStatus->text_status))
				{
					CGitStatusCache::Instance().UpdateShell(parent->m_directoryPath);
					//ATLTRACE(_T("shell update for %s\n"), parent->m_directoryPath.GetWinPathString());
					parent->m_ownStatus.SetKind(git_node_dir);
					parent->m_ownStatus.SetStatus(pGitStatus);
				}
			}
		}
	}
}


CString
CCachedDirectory::GetCacheKey(const CTGitPath& path)
{
	// All we put into the cache as a key is just the end portion of the pathname
	// There's no point storing the path of the containing directory for every item
	return path.GetWinPathString().Mid(m_directoryPath.GetWinPathString().GetLength()).MakeLower().TrimLeft(_T("\\"));
}

CString
CCachedDirectory::GetFullPathString(const CString& cacheKey)
{
	return m_directoryPath.GetWinPathString() + _T("\\") + cacheKey;
}

BOOL CCachedDirectory::GetStatusCallback(const CString & path, git_wc_status_kind status,bool isDir, void *, bool assumeValid, bool skipWorktree)
{
	git_wc_status2_t _status;
	git_wc_status2_t *status2 = &_status;

	status2->prop_status = status2->text_status = status;
	status2->assumeValid = assumeValid;
	status2->skipWorktree = skipWorktree;

	CTGitPath gitPath(path);

	CCachedDirectory *pThis = CGitStatusCache::Instance().GetDirectoryCacheEntry(gitPath.GetContainingDirectory());

	if(pThis == NULL)
		return FALSE;

//	if(status->entry)
	{
		if (isDir)
		{	/*gitpath is directory*/
			//if ( !gitPath.IsEquivalentToWithoutCase(pThis->m_directoryPath) )
			{
				if (!gitPath.Exists())
				{
					ATLTRACE(_T("Miss dir %s \n"), gitPath.GetWinPath());
					pThis->m_mostImportantFileStatus = GitStatus::GetMoreImportant(pThis->m_mostImportantFileStatus, git_wc_status_deleted);
				}

				if ( status <  git_wc_status_normal)
				{
					if( ::PathFileExists(path+_T("\\.git")))
					{ // this is submodule
						ATLTRACE(_T("skip submodule %s\n"), path);
						return FALSE;
					}
				}
				if (pThis->m_bRecursive)
				{
					// Add any versioned directory, which is not our 'self' entry, to the list for having its status updated
//OutputDebugStringA("AddFolderCrawl: ");OutputDebugStringW(svnPath.GetWinPathString());OutputDebugStringA("\r\n");
					if(status >= git_wc_status_normal)
						CGitStatusCache::Instance().AddFolderForCrawling(gitPath);
				}

				// Make sure we know about this child directory
				// This initial status value is likely to be overwritten from below at some point
				git_wc_status_kind s = GitStatus::GetMoreImportant(status2->text_status, status2->prop_status);

				// folders must not be displayed as added or deleted only as modified
				if (s == git_wc_status_deleted || s == git_wc_status_added)
					s = git_wc_status_modified;

				CCachedDirectory * cdir = CGitStatusCache::Instance().GetDirectoryCacheEntryNoCreate(gitPath);
				if (cdir)
				{
					// This child directory is already in our cache!
					// So ask this dir about its recursive status
					git_wc_status_kind st = GitStatus::GetMoreImportant(s, cdir->GetCurrentFullStatus());
					AutoLocker lock(pThis->m_critSec);
					pThis->m_childDirectories[gitPath] = st;
					ATLTRACE(_T("call 1 Update dir %s %d\n"), gitPath.GetWinPath(), st);
				}
				else
				{
					AutoLocker lock(pThis->m_critSec);
					// the child directory is not in the cache. Create a new entry for it in the cache which is
					// initially 'unversioned'. But we added that directory to the crawling list above, which
					// means the cache will be updated soon.
					CGitStatusCache::Instance().GetDirectoryCacheEntry(gitPath);

					pThis->m_childDirectories[gitPath] = s;
					ATLTRACE(_T("call 2 Update dir %s %d\n"), gitPath.GetWinPath(), s);
				}
			}
		}
		else /* gitpath is file*/
		{
			// Keep track of the most important status of all the files in this directory
			// Don't include subdirectories in this figure, because they need to provide their
			// own 'most important' value
			if (status2->text_status == git_wc_status_deleted || status2->text_status == git_wc_status_added)
			{
				// if just a file in a folder is deleted or added report back that the folder is modified and not deleted or added
				pThis->m_mostImportantFileStatus = GitStatus::GetMoreImportant(pThis->m_mostImportantFileStatus, git_wc_status_modified);
			}
			else
			{
				pThis->m_mostImportantFileStatus = GitStatus::GetMoreImportant(pThis->m_mostImportantFileStatus, status2->text_status);
				pThis->m_mostImportantFileStatus = GitStatus::GetMoreImportant(pThis->m_mostImportantFileStatus, status2->prop_status);
				if ((status2->text_status == git_wc_status_unversioned)
					&&(CGitStatusCache::Instance().IsUnversionedAsModified()))
				{
					// treat unversioned files as modified
					if (pThis->m_mostImportantFileStatus != git_wc_status_added)
						pThis->m_mostImportantFileStatus = GitStatus::GetMoreImportant(pThis->m_mostImportantFileStatus, git_wc_status_modified);
				}
			}
		}
	}

	pThis->AddEntry(gitPath, status2);

	return FALSE;
}

bool
CCachedDirectory::IsOwnStatusValid() const
{
	return m_ownStatus.HasBeenSet() &&
		   !m_ownStatus.HasExpired(GetTickCount());
}

void CCachedDirectory::Invalidate()
{
	m_ownStatus.Invalidate();
}

git_wc_status_kind CCachedDirectory::CalculateRecursiveStatus()
{
	// Combine our OWN folder status with the most important of our *FILES'* status.
	git_wc_status_kind retVal = GitStatus::GetMoreImportant(m_mostImportantFileStatus, m_ownStatus.GetEffectiveStatus());

	// Now combine all our child-directorie's status
	AutoLocker lock(m_critSec);
	ChildDirStatus::const_iterator it;
	for(it = m_childDirectories.begin(); it != m_childDirectories.end(); ++it)
	{
		retVal = GitStatus::GetMoreImportant(retVal, it->second);
	}

	return retVal;
}

// Update our composite status and deal with things if it's changed
void CCachedDirectory::UpdateCurrentStatus()
{
	git_wc_status_kind newStatus = CalculateRecursiveStatus();
	ATLTRACE(_T("UpdateCurrentStatus %s new:%d old: %d\n"),
		m_directoryPath.GetWinPath(),
		newStatus, m_currentFullStatus);

	if ( this->m_ownStatus.GetEffectiveStatus() < git_wc_status_normal )
	{
		if (::PathFileExists(this->m_directoryPath.GetWinPathString()+_T("\\.git")))
		{
			//project root must be normal status at least.
			ATLTRACE(_T("force update project root directory as normal status\n"));
			this->m_ownStatus.ForceStatus(git_wc_status_normal);
		}
	}

	if ((newStatus != m_currentFullStatus) && m_ownStatus.IsVersioned())
	{
		if ((m_currentFullStatus != git_wc_status_none)&&(m_ownStatus.GetEffectiveStatus() != git_wc_status_missing))
		{
			// Our status has changed - tell the shell
			ATLTRACE(_T("Dir %s, status change from %d to %d, send shell notification\n"), m_directoryPath.GetWinPath(), m_currentFullStatus, newStatus);
			CGitStatusCache::Instance().UpdateShell(m_directoryPath);
		}
		if (m_ownStatus.GetEffectiveStatus() != git_wc_status_missing)
			m_currentFullStatus = newStatus;
		else
			m_currentFullStatus = git_wc_status_missing;
	}
	// And tell our parent, if we've got one...
	// we tell our parent *always* about our status, even if it hasn't
	// changed. This is to make sure that the parent has really our current
	// status - the parent can decide itself if our status has changed
	// or not.
	CTGitPath parentPath = m_directoryPath.GetContainingDirectory();
	if(!parentPath.IsEmpty())
	{
		// We have a parent
		// just version controled directory need to cache.
		CString root1, root2;
		if(parentPath.HasAdminDir(&root1) && m_directoryPath.HasAdminDir(&root2) && root1 == root2)
		{
			CCachedDirectory * cachedDir = CGitStatusCache::Instance().GetDirectoryCacheEntry(parentPath);
			if (cachedDir)
				cachedDir->UpdateChildDirectoryStatus(m_directoryPath, m_currentFullStatus);
		}
	}
}


// Receive a notification from a child that its status has changed
void CCachedDirectory::UpdateChildDirectoryStatus(const CTGitPath& childDir, git_wc_status_kind childStatus)
{
	git_wc_status_kind currentStatus = git_wc_status_none;
	{
		AutoLocker lock(m_critSec);
		currentStatus = m_childDirectories[childDir];
	}
	if ((currentStatus != childStatus)||(!IsOwnStatusValid()))
	{
		{
			AutoLocker lock(m_critSec);
			m_childDirectories[childDir] = childStatus;
		}
		UpdateCurrentStatus();
	}
}

CStatusCacheEntry CCachedDirectory::GetOwnStatus(bool bRecursive)
{
	// Don't return recursive status if we're unversioned ourselves.
	if(bRecursive && m_ownStatus.GetEffectiveStatus() > git_wc_status_unversioned && m_ownStatus.GetEffectiveStatus() != git_wc_status_ignored)
	{
		CStatusCacheEntry recursiveStatus(m_ownStatus);
		UpdateCurrentStatus();
		if (m_currentFullStatus == git_wc_status_deleted || m_currentFullStatus == git_wc_status_added)
			m_currentFullStatus = git_wc_status_modified;
		recursiveStatus.ForceStatus(m_currentFullStatus);
		return recursiveStatus;
	}
	else
	{
		return m_ownStatus;
	}
}

void CCachedDirectory::RefreshStatus(bool bRecursive)
{
	// Make sure that our own status is up-to-date
	GetStatusForMember(m_directoryPath,bRecursive);

	AutoLocker lock(m_critSec);
	// We also need to check if all our file members have the right date on them
	CacheEntryMap::iterator itMembers;
	std::set<CTGitPath> refreshedpaths;
	DWORD now = GetTickCount();
	if (m_entryCache.empty())
		return;
	for (itMembers = m_entryCache.begin(); itMembers != m_entryCache.end(); ++itMembers)
	{
		if (itMembers->first)
		{
			CTGitPath filePath(m_directoryPath);
			filePath.AppendPathString(itMembers->first);
			std::set<CTGitPath>::iterator refr_it;
			if ((!filePath.IsEquivalentToWithoutCase(m_directoryPath))&&
				(((refr_it = refreshedpaths.lower_bound(filePath)) == refreshedpaths.end()) || !filePath.IsEquivalentToWithoutCase(*refr_it)))
			{
				if ((itMembers->second.HasExpired(now))||(!itMembers->second.DoesFileTimeMatch(filePath.GetLastWriteTime())))
				{
					lock.Unlock();
					// We need to request this item as well
					GetStatusForMember(filePath,bRecursive);
					// GetStatusForMember now has recreated the m_entryCache map.
					// So start the loop again, but add this path to the refreshed paths set
					// to make sure we don't refresh this path again. This is to make sure
					// that we don't end up in an endless loop.
					lock.Lock();
					refreshedpaths.insert(refr_it, filePath);
					itMembers = m_entryCache.begin();
					if (m_entryCache.empty())
						return;
					continue;
				}
				else if ((bRecursive)&&(itMembers->second.IsDirectory()))
				{
					// crawl all sub folders too! Otherwise a change deep inside the
					// tree which has changed won't get propagated up the tree.
					CGitStatusCache::Instance().AddFolderForCrawling(filePath);
				}
			}
		}
	}
}

void CCachedDirectory::RefreshMostImportant()
{
	AutoLocker lock(m_critSec);
	CacheEntryMap::iterator itMembers;
	git_wc_status_kind newStatus = m_ownStatus.GetEffectiveStatus();
	for (itMembers = m_entryCache.begin(); itMembers != m_entryCache.end(); ++itMembers)
	{
		newStatus = GitStatus::GetMoreImportant(newStatus, itMembers->second.GetEffectiveStatus());
		if (((itMembers->second.GetEffectiveStatus() == git_wc_status_unversioned)||(itMembers->second.GetEffectiveStatus() == git_wc_status_none))
			&&(CGitStatusCache::Instance().IsUnversionedAsModified()))
		{
			// treat unversioned files as modified
			if (newStatus != git_wc_status_added)
				newStatus = GitStatus::GetMoreImportant(newStatus, git_wc_status_modified);
		}
	}
	if (newStatus != m_mostImportantFileStatus)
	{
		ATLTRACE(_T("status change of path %s\n"), m_directoryPath.GetWinPath());
		CGitStatusCache::Instance().UpdateShell(m_directoryPath);
	}
	m_mostImportantFileStatus = newStatus;
}
