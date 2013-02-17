/*
 *      Copyright (C) 2012 Garrett Brown
 *      Copyright (C) 2012 Team XBMC
 *      http://www.xbmc.org
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
 *  along with XBMC; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *  http://www.gnu.org/copyleft/gpl.html
 *
 */
#pragma once

#include "GameClient.h"
#include "games/tags/GameInfoTagLoader.h"
#include "threads/CriticalSection.h"

typedef std::vector<GAME_INFO::GamePlatform> GamePlatformArray;

/**
 * The main function of CGameManager is resolving file items into CGameClients.
 *
 * A manager is needed for resolving game clients as they are selected by the
 * file extensions they support. This is determined by loading the DLL and
 * querying it directly, so it is desirable to only do this once and cache the
 * information.
 */
class CGameManager
{
private:
  CGameManager() { }

public:
  static CGameManager &Get();

  /**
   * Create and maintain a cache of game client add-on information.
   */
  void RegisterAddons(const ADDON::VECADDONS &addons);
  void RegisterAddon(ADDON::GameClientPtr clientAddon);
  void UnregisterAddonByID(const CStdString &ID);

  /**
   * Resolve a file item to a list of game client IDs. If the file forces a
   * particular game client via file.SetProperty("gameclient", id), the result
   * will contain no more than one possible candidate. If the file's game info
   * tag provides a "platform", the available game clients will be filtered by
   * this platform (given the <platform> tag in their addon.xml). If file is a
   * zip file, the contents of that zip will be used to find suitable
   * candidates (which may yeild multiple if there are several different kinds
   * of ROMs inside).
   */
  void GetGameClientIDs(const CFileItem& file, CStdStringArray &candidates, int max = -1) const;

private:
  /**
   * Generate a GamePlatformArray from an array of strings. If a string isn't
   * recognized as a valid game platform, it is ommited from the output array.
   */
  static void TranslatePlatformArray(const CStdStringArray &strPlatforms, GamePlatformArray &vecPlatforms);
  
  /**
   * Pertinent information captured by the game client add-on. We use these
   * objects as a cache to avoid loading the DLL every time.
   */
  struct GameClientObject
  {
    CStdString        id;
    CStdStringArray   extensions;
    GamePlatformArray platforms;
  };

  std::vector<GameClientObject> m_gameClients;
  CCriticalSection m_critSection; // Guard m_gameClients
};
