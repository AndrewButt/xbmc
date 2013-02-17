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

#include "GameManager.h"
#include "addons/AddonManager.h"
#include "threads/SingleLock.h"
#include "utils/log.h"
#include "utils/StringUtils.h"
#include "utils/URIUtils.h"
#include "utils/Variant.h"

using namespace ADDON;
using namespace GAME_INFO;


/* TEMPORARY */
// Remove this struct when libretro has an API call to query the number of
// controller ports a game supports. If this code is still here in six months,
// Garrett will be very unhappy. I found a buffer overflow in SNES9x when
// trying to set controller ports 3-8, so this API call needs to happen.
/*
struct PortMapping
{
  GamePlatform platform;
  int          ports;
};

static const PortMapping ports[] =
{
  { PLATFORM_GAMEBOY,              1 },
  { PLATFORM_GAMEBOY_COLOR,        1 },
  { PLATFORM_GAMEBOY_ADVANCE,      1 },
  { PLATFORM_NEO_GEO_POCKET_COLOR, 1 },
  { PLATFORM_SEGA_MASTER_SYSTEM,   2 },
  { PLATFORM_SNES,                 2 },
};
*/

/* static */
CGameManager &CGameManager::Get()
{
  static CGameManager gameManagerInstance;
  return gameManagerInstance;
}

void CGameManager::RegisterAddons(const VECADDONS &addons)
{
  for (VECADDONS::const_iterator it = addons.begin(); it != addons.end(); it++)
    RegisterAddon(boost::dynamic_pointer_cast<CGameClient>(*it));
}

void CGameManager::RegisterAddon(GameClientPtr clientAddon)
{
  // Sanity check
  if (!clientAddon)
    return;

  CSingleLock lock(m_critSection);

  // If we are already tracking the add-on, erase it so we can refresh the data
  for (std::vector<GameClientObject>::const_iterator it = m_gameClients.begin(); it != m_gameClients.end(); it++)
  {
    if (clientAddon->ID() == it->id)
    {
      CLog::Log(LOGERROR, "CGameManager: Refreshing information for add-on %s!", clientAddon->ID());
      m_gameClients.erase(it);
      break;
    }
  }

  // Load the DLL
  if (!clientAddon->Init())
  {
    CLog::Log(LOGERROR, "CGameManager: failed to load the DLL for add-on %s", clientAddon->ID());
    return;
  }

  GameClientObject clientObject;
  clientObject.id = clientAddon->ID();
  clientObject.extensions = clientAddon->GetExtensions();
  TranslatePlatformArray(clientAddon->GetPlatforms(), clientObject.platforms);

  // Currently we only store these three fields, so we're all done here
  m_gameClients.push_back(clientObject);

  // Unload the DLL
  clientAddon->DeInit();
}

void CGameManager::UnregisterAddonByID(const CStdString &ID)
{
  CSingleLock lock(m_critSection);

  for (std::vector<GameClientObject>::iterator it = m_gameClients.begin(); it != m_gameClients.end(); it++)
    if (ID == it->id)
      { m_gameClients.erase(it); return; }
  CLog::Log(LOGERROR, "CGameManager: can't unregister %s - not registered!", ID.c_str());
}

void CGameManager::GetGameClientIDs(const CFileItem& file, CStdStringArray &candidates, int max /* = -1 */) const
{
  candidates.clear();

  // Whether we should force a specific game client
  if (!file.GetProperty("gameclient").empty())
  {
    CStdString id = file.GetProperty("gameclient").asString();
    for (std::vector<GameClientObject>::const_iterator it = m_gameClients.begin(); it != m_gameClients.end(); it++)
    {
      if (it->id == id)
        { candidates.push_back(id); break; }
    }
    // If the game client isn't installed, it's not a valid candidate
    return;
  }

  // Look for a "platform" hint in the file item
  GamePlatform platformHint = PLATFORM_UNKNOWN;
  if (file.GetGameInfoTag())
    platformHint = CGameInfoTagLoader::GetPlatformByName(file.GetGameInfoTag()->GetPlatform()).id;
  if (platformHint == PLATFORM_AMBIGUOUS)
    platformHint = PLATFORM_UNKNOWN; // Treat ambiguous as unknown

  // Get the file extension
  CStdString strExtension(URIUtils::GetExtension(file.GetPath()));
  strExtension.ToLower();

  CSingleLock lock(m_critSection);

  for (std::vector<GameClientObject>::const_iterator it = m_gameClients.begin(); it != m_gameClients.end(); it++)
  {
    // Skip the game client if it doesn't support the platform. This check is
    // only done if both the game client lists at least one valid platform, and
    // the file extension is unambiguously matched to a platform.
    if ((platformHint != PLATFORM_UNKNOWN) && (!it->platforms.empty()) &&
        (std::find(it->platforms.begin(), it->platforms.end(), platformHint) == it->platforms.end()))
      continue;

    // If the game client lists supported extensions, filter by those as well
    if (!it->extensions.empty())
    {
      // If the file is a zip, rake the contents for valid game files
      if (strExtension.Equals(".zip"))
      {
        CStdString path2;
        if (!CGameClient::GetEffectiveRomPath(file.GetPath(), it->extensions, path2))\
          continue;
      }
      else if (std::find(it->extensions.begin(), it->extensions.end(), strExtension) == it->extensions.end())
      {
        continue;
      }
    }

    candidates.push_back(it->id);
    if (max != -1 && (int)candidates.size() >= max)
      break;
  }
}

/* static */
void CGameManager::TranslatePlatformArray(const CStdStringArray &strPlatforms, GamePlatformArray &vecPlatforms)
{
  vecPlatforms.clear();
  for (CStdStringArray::const_iterator it = strPlatforms.begin(); it != strPlatforms.end(); it++)
  {
    GamePlatform id = CGameInfoTagLoader::GetPlatformByName(*it).id;
    if (id != PLATFORM_UNKNOWN)
      vecPlatforms.push_back(id);
  }
}
