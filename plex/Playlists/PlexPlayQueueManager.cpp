#include "PlexPlayQueueManager.h"
#include "PlexPlayQueueServer.h"
#include "URL.h"
#include "PlayListPlayer.h"
#include "playlists/PlayList.h"
#include "ApplicationMessenger.h"
#include "FileItem.h"
#include "PlexApplication.h"
#include "Client/PlexServerManager.h"
#include "PlexPlayQueueLocal.h"
#include "settings/GUISettings.h"

using namespace PLAYLIST;

///////////////////////////////////////////////////////////////////////////////////////////////////
CPlexPlayQueueManager::CPlexPlayQueueManager()
{
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void CPlexPlayQueueManager::create(const CFileItem& container, const CStdString& uri,
                                   const CStdString& startItemKey, bool shuffle)
{
  IPlexPlayQueueBasePtr impl = getImpl(container);
  if (impl)
  {
    m_currentImpl = impl;
    m_currentImpl->create(container, uri, startItemKey, shuffle);
  }
}

///////////////////////////////////////////////////////////////////////////////////////////////////
int CPlexPlayQueueManager::getPlaylistFromType(ePlexMediaType type)
{
  switch (type)
  {
    case PLEX_MEDIA_TYPE_MUSIC:
      return PLAYLIST_MUSIC;
    case PLEX_MEDIA_TYPE_VIDEO:
      return PLAYLIST_VIDEO;
    default:
      return PLAYLIST_NONE;
  }
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void CPlexPlayQueueManager::playQueueUpdated(const ePlexMediaType& type, bool startPlaying)
{
  if (!m_currentImpl)
    return;

  CFileItemPtr playlistItem;
  int playlist = getPlaylistFromType(type);
  if (playlist != PLAYLIST_NONE)
  {
    CPlayList pl = g_playlistPlayer.GetPlaylist(type);
    if (pl.size() > 0)
      playlistItem = pl[0];
  }

  int pqID = m_currentImpl->getCurrentID();
  CFileItemList list;
  if (!m_currentImpl->getCurrent(list))
    return;

  if (g_playlistPlayer.GetCurrentPlaylist() == playlist && playlistItem &&
      playlistItem->GetProperty("playQueueID").asInteger() == pqID)
  {
    reconcilePlayQueueChanges(type, list);
  }
  else
  {
    int selectedOffset = list.GetProperty("playQueueSelectedItemOffset").asInteger(0);
    CApplicationMessenger::Get().MediaStop(true);
    g_playlistPlayer.SetCurrentPlaylist(playlist);
    g_playlistPlayer.ClearPlaylist(playlist);
    g_playlistPlayer.Add(playlist, list);
    g_playlistPlayer.SetCurrentSong(selectedOffset);

    if (startPlaying)
      g_playlistPlayer.Play();

    saveCurrentPlayQueue(m_currentImpl->server(), list);

    CLog::Log(LOGDEBUG,
              "CPlexPlayQueueManager::PlayQueueUpdated now playing PlayQueue of type %d",
              type);
  }
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void CPlexPlayQueueManager::saveCurrentPlayQueue(const CPlexServerPtr& server,
                                                 const CFileItemList& list)
{
  m_playQueueType = PlexUtils::GetMediaTypeFromItem(list);

  int playQueueID = list.GetProperty("playQueueID").asInteger();
  if (playQueueID > 0)
  {
    CStdString path;
    path.Format("%d", playQueueID);

    g_guiSettings.SetString("system.mostrecentplayqueue", server->BuildPlexURL(path).Get());
  }
}

///////////////////////////////////////////////////////////////////////////////////////////////////
CStdString CPlexPlayQueueManager::getURIFromItem(const CFileItem& item, const CStdString& uri)
{
  if (item.GetPath().empty())
    return "";

  CURL u(item.GetPath());

  if (u.GetProtocol() != "plexserver")
    return "";

  CStdString itemDirStr = "item";
  if (item.m_bIsFolder)
    itemDirStr = "directory";

  if (!item.HasProperty("librarySectionUUID"))
  {
    CLog::Log(LOGWARNING,
              "CPlexPlayQueueManager::getURIFromItem item %s doesn't have a section UUID",
              item.GetPath().c_str());
    return "";
  }

  CStdString realURI =
  uri.empty() ? (CStdString)item.GetProperty("unprocessed_key").asString() : uri;
  CURL::Encode(realURI);

  CStdString ret;
  ret.Format("library://%s/%s/%s", item.GetProperty("librarySectionUUID").asString(), itemDirStr,
             realURI);

  return ret;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void CPlexPlayQueueManager::reconcilePlayQueueChanges(int playlistType, const CFileItemList& list)
{
  bool firstCommon = false;
  int listCursor = 0;
  int playlistCursor = 0;

  CPlayList playlist(g_playlistPlayer.GetPlaylist(playlistType));

  while (true)
  {
    CStdString playlistPath;
    if (playlistCursor < playlist.size())
      playlistPath = playlist[playlistCursor]->GetProperty("unprocessed_key").asString();

    if (listCursor >= list.Size())
      break;

    CStdString listPath = list.Get(listCursor)->GetProperty("unprocessed_key").asString();

    if (!firstCommon && !playlistPath.empty())
    {
      if (playlistPath == listPath)
      {
        firstCommon = true;
        listCursor++;
      }
      else
      {
        // remove the item at in the playlist.
        g_playlistPlayer.Remove(playlistType, playlistCursor);
        CLog::Log(
        LOGDEBUG,
        "CPlexPlayQueueManager::reconcilePlayQueueChanges removing item %d from playlist %d",
        playlistCursor, playlistType);
      }
    }
    else if (playlistPath.empty())
    {
      CLog::Log(LOGDEBUG,
                "CPlexPlayQueueManager::reconcilePlayQueueChanges adding item %s to playlist %d",
                listPath.c_str(), playlistType);
      g_playlistPlayer.Add(playlistType, list.Get(listCursor));
      listCursor++;
    }
    else if (playlistPath == listPath)
    {
      listCursor++;
    }
    else
    {
      g_playlistPlayer.Remove(playlistType, playlistCursor);
    }

    playlistCursor++;
  }
}

///////////////////////////////////////////////////////////////////////////////////////////////////
bool CPlexPlayQueueManager::getCurrentPlayQueue(CFileItemList& list)
{
  if (m_currentImpl)
    return m_currentImpl->getCurrent(list);
  return false;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void CPlexPlayQueueManager::loadSavedPlayQueue()
{
  CURL playQueueURL(g_guiSettings.GetString("system.mostrecentplayqueue"));
  CPlexServerPtr server = g_plexApplication.serverManager->FindByUUID(playQueueURL.GetHostName());
  if (server && !m_currentImpl)
  {
    m_currentImpl = IPlexPlayQueueBasePtr(new CPlexPlayQueueServer(server));
    m_currentImpl->get(playQueueURL.GetFileName());
  }
}

///////////////////////////////////////////////////////////////////////////////////////////////////
IPlexPlayQueueBasePtr CPlexPlayQueueManager::getImpl(const CFileItem& container)
{
  CPlexServerPtr server = g_plexApplication.serverManager->FindFromItem(container);
  if (server)
  {
    CLog::Log(LOGDEBUG, "CPlexPlayQueueManager::getImpl identifier: %s server: %s",
              container.GetProperty("identifier").asString().c_str(),
              server->toString().c_str());

    if (container.GetProperty("identifier").asString() == "com.plexapp.plugins.library" &&
        CPlexPlayQueueServer::isSupported(server))
    {
      CLog::Log(LOGDEBUG, "CPlexPlayQueueManager::getImpl selecting PlexPlayQueueServer");
      return IPlexPlayQueueBasePtr(new CPlexPlayQueueServer(server));
    }

    CLog::Log(LOGDEBUG, "CPlexPlayQueueManager::getImpl selecting PlexPlayQueueLocal");
    return IPlexPlayQueueBasePtr(new CPlexPlayQueueLocal(server));
  }

  CLog::Log(LOGDEBUG, "CPlexPlayQueueManager::getImpl can't select implementation");
  return IPlexPlayQueueBasePtr();
}