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

#include "RetroPlayerInput.h"
#include "games/libretro/libretro.h"
#include "guilib/GUIWindowManager.h"
#include "input/ButtonTranslator.h"
#include "input/KeyboardStat.h"
#include "utils/log.h"

#if defined(TARGET_WINDOWS)
#include "input/windows/WINJoystick.h"
#elif defined(HAS_SDL_JOYSTICK) || defined(HAS_EVENT_SERVER)
#include "input/SDLJoystick.h"
#endif

#ifndef JOY_POVFORWARD
#define JOY_POVFORWARD 0
#endif

#ifndef JOY_POVRIGHT
#define JOY_POVRIGHT 9000
#endif

#ifndef JOY_POVBACKWARD
#define JOY_POVBACKWARD 18000
#endif

#ifndef JOY_POVLEFT
#define JOY_POVLEFT 27000
#endif

#define JOY_POV_360  JOY_POVBACKWARD * 2
#define JOY_POV_NE   (JOY_POVFORWARD + JOY_POVRIGHT) / 2
#define JOY_POV_SE   (JOY_POVRIGHT + JOY_POVBACKWARD) / 2
#define JOY_POV_SW   (JOY_POVBACKWARD + JOY_POVLEFT) / 2
#define JOY_POV_NW   (JOY_POVLEFT + JOY_POV_360) / 2



unsigned char &CRetroPlayerInput::Hat::operator[](unsigned int i)
{
  switch (i)
  {
  case 0: return up;
  case 1: return right;
  case 2: return down;
  case 3: return left;
  default: return up;
  }
}

#define MAKE_DIRECTION(n, e, s, w) ((n) << 3 | (e) << 2 | (s) << 1 | (w))
const char *CRetroPlayerInput::Hat::GetDirection() const
{
  switch (MAKE_DIRECTION(up, right, down, left))
  {
  case MAKE_DIRECTION(1, 0, 0, 0): return "N";
  case MAKE_DIRECTION(1, 1, 0, 0): return "NE";
  case MAKE_DIRECTION(0, 1, 0, 0): return "E";
  case MAKE_DIRECTION(0, 1, 1, 0): return "SE";
  case MAKE_DIRECTION(0, 0, 1, 0): return "S";
  case MAKE_DIRECTION(0, 0, 1, 1): return "SW";
  case MAKE_DIRECTION(0, 0, 0, 1): return "W";
  case MAKE_DIRECTION(1, 0, 0, 1): return "NW";
  default: return "CENTERED";
  }
}


CRetroPlayerInput::CRetroPlayerInput() : m_bActive(false)
{
}

void CRetroPlayerInput::Begin()
{
  memset(m_joypadState, 0, sizeof(m_joypadState));
  memset(m_buttonState, 0, sizeof(m_buttonState));
  memset(m_hatState,    0, sizeof(m_hatState));
  m_bActive = true;
}

void CRetroPlayerInput::Finish()
{
  m_bActive = false;
}

int16_t CRetroPlayerInput::GetInput(unsigned port, unsigned device, unsigned index, unsigned id)
{
  CSingleLock lock(m_statesGuard);

  if (port == 0)
  {
    device &= RETRO_DEVICE_MASK;

    switch (device)
    {
    case RETRO_DEVICE_JOYPAD:
      if (id < sizeof(m_joypadState) / sizeof(m_joypadState[0]))
        return m_joypadState[id];
      else
        CLog::Log(LOGDEBUG, "RetroPlayerInput: GetInput() called with invalid ID: %d", id);
      break;
    default: // Only RETRO_DEVICE_JOYPAD is supported currently
      CLog::Log(LOGDEBUG, "RetroPlayerInput: GetInput() called with invalid device: %d", device);
      break;
    }
  }
  return 0;
}

void CRetroPlayerInput::ProcessKeyDown(const CKey &key)
{
  // TODO: Use ID of current window, in case it defines joypad keys outside of
  // <FullscreenGame>. Maybe, if current window ID finds no action, then fallback
  // to WINDOW_FULLSCREEN_GAME.
  CAction action = CButtonTranslator::GetInstance().GetAction(WINDOW_FULLSCREEN_GAME, key);

  CSingleLock lock(m_statesGuard);

  int id = TranslateActionID(action.GetID());
  if (0 <= id && id < (int)(sizeof(m_joypadState) / sizeof(m_joypadState[0])))
  {
    CLog::Log(LOGDEBUG, "RetroPlayerInput: KeyDown, action=%s, ID=%d", action.GetName().c_str(), id);
    m_joypadState[id] = 1;
  }
  else
  {
    CLog::Log(LOGDEBUG, "RetroPlayerInput: Invalid KeyDown, action=%s, ID=%d", action.GetName().c_str(), action.GetID());
  }
}

void CRetroPlayerInput::ProcessKeyUp(const CKey &key)
{
  // TODO: Use ID of current window, in case it defines joypad keys outside of
  // <FullscreenGame>. Maybe, if current window ID finds no action, then fallback
  // to WINDOW_FULLSCREEN_GAME.
  CAction action = CButtonTranslator::GetInstance().GetAction(WINDOW_FULLSCREEN_GAME, key);

  CSingleLock lock(m_statesGuard);

  int id = TranslateActionID(action.GetID());
  if (0 <= id && id < (int)(sizeof(m_joypadState) / sizeof(m_joypadState[0])))
  {
    CLog::Log(LOGDEBUG, "RetroPlayerInput: KeyUp, action=%s, ID=%d", action.GetName().c_str(), id);
    m_joypadState[id] = 0;
  }
  else
  {
    CLog::Log(LOGDEBUG, "RetroPlayerInput: Invalid KeyUp, action=%s, ID=%d", action.GetName().c_str(), action.GetID());
  }
}

void CRetroPlayerInput::ProcessGamepad(const std::string &device, const unsigned char buttons[GAMEPAD_BUTTON_COUNT],
  int numHats, const unsigned long hats[4])
{
  int window = !g_windowManager.HasModalDialog() ? g_windowManager.GetActiveWindow() : g_windowManager.GetTopMostModalDialogID();

  CSingleLock lock(m_statesGuard);

  for (unsigned int bid = 0; bid < GAMEPAD_BUTTON_COUNT; bid++)
  {
    // We only care if a change in state is detected
    unsigned char bPressed = buttons[bid] & 0x80 ? 1 : 0;
    if (bPressed == m_buttonState[bid])
      continue;

    // We only process button presses in WINDOW_FULLSCREEN_VIDEO. We check for
    // this instead of WINDOW_FULLSCREEN_GAME because FULLSCREEN_GAME is a thin
    // alias for FULLSCREEN_VIDEO, used only for translating keyboard and gamepad
    // events (this way, our translaters don't have to query the active player 
    // core). Later on, when translating, *then* we'll use WINDOW_FULLSCREEN_GAME.
    if (bPressed && (window & WINDOW_ID_MASK) != WINDOW_FULLSCREEN_VIDEO)
      continue;

    // Record the new state
    m_buttonState[bid] = bPressed;

    int        actionID;
    CStdString actionName;
    bool       fullrange; // unused
    // Actual button ID is bid + 1
    if (!CButtonTranslator::GetInstance().TranslateJoystickString(WINDOW_FULLSCREEN_GAME,
        device.c_str(), bid + 1, JACTIVE_BUTTON, actionID, actionName, fullrange))
      continue;

    int id = TranslateActionID(actionID);
    if (0 <= id && id < (int)(sizeof(m_joypadState) / sizeof(m_joypadState[0])))
    {
      // Record the new joypad state
      m_joypadState[id] = bPressed;
      CLog::Log(LOGDEBUG, "RetroPlayerInput: Gamepad %s button %s, action=%s, ID=%d", device.c_str(),
        bPressed ? "press" : "unpress", actionName.c_str(), actionID);
    }
    else
    {
      CLog::Log(LOGDEBUG, "RetroPlayerInput: Gamepad %s invalid button %s, action=%s, ID=%d", device.c_str(),
        bPressed ? "press" : "unpress", actionName.c_str(), actionID);
    }
  }

  // numHats is capped at 4 in WINJoystick.h, but just in case...
  if (numHats < 0)
    numHats = 0;
  else if (numHats > 4)
    numHats = 4;

  for (unsigned int iHat = 0; iHat < (unsigned int)numHats; iHat++)
  {
    // Unpack unsigned long hats[iHat] into a Hat struct
    Hat hat;
    bool bCentered = ((hats[iHat] & 0xFFFF) == 0xFFFF);
    if (!bCentered)
    {
      // We disallow antagonistic buttons from being pressed together
      if ((JOY_POV_NW <= hats[iHat] && hats[iHat] <= JOY_POV_360) || hats[iHat] <= JOY_POV_NE)
        hat.up = 1;
      else if (JOY_POV_SE <= hats[iHat] && hats[iHat] <= JOY_POV_SW)
        hat.down = 1;

      if (JOY_POV_NE <= hats[iHat] && hats[iHat] <= JOY_POV_SE)
        hat.right = 1;
      else if (JOY_POV_SW <= hats[iHat] && hats[iHat] <= JOY_POV_NW)
        hat.left = 1;
    }
    
    // We only care if a change in state is detected
    if (hat == m_hatState[iHat])
      continue;
    CLog::Log(LOGDEBUG, "RetroPlayerInput: Gamepad %s, new hat %d direction is %s", device.c_str(), iHat, hat.GetDirection());

    // Using ordinal directions instead of cardinal directions lets us use a for loop
    for (unsigned int i = 0; i < 4; i++)
    {
      if (hat[i] == m_hatState[iHat][i])
        continue;

      // Don't record presses outside of fullscreen video (unpresses are ok)
      if (hat[i] && (window & WINDOW_ID_MASK) != WINDOW_FULLSCREEN_VIDEO)
        continue;

      // Record the new state
      m_hatState[iHat][i] = hat[i];

      // Compose button ID (SDL_HAT_UP is (1 << 0), SDL_HAT_RIGHT is (1 << 1), etc.)
      int        buttonID = (1 << i) << 16 | (iHat + 1); // Hat ID is iHat + 1
      int        actionID;
      CStdString actionName;
      bool       fullrange; // unused
      if (!CButtonTranslator::GetInstance().TranslateJoystickString(WINDOW_FULLSCREEN_GAME,
          device.c_str(), buttonID, JACTIVE_HAT, actionID, actionName, fullrange))
        continue;

      static const char *dir[] = {"UP", "RIGHT", "DOWN", "LEFT"};

      int id = TranslateActionID(actionID);
      if (0 <= id && id < (int)(sizeof(m_joypadState) / sizeof(m_joypadState[0])))
      {
        // Record the new joypad state
        m_joypadState[id] = hat[i];
        CLog::Log(LOGDEBUG, "RetroPlayerInput: Hat %s %s, action=%s, ID=%d", dir[i],
          hat[i] ? "press" : "unpress", actionName.c_str(), actionID);
      }
      else
      {
        CLog::Log(LOGDEBUG, "RetroPlayerInput: Invalid hat %s %s, action=%s, ID=%d", dir[i],
          hat[i] ? "press" : "unpress", actionName.c_str(), actionID);
      }
    }
  }
}

int CRetroPlayerInput::TranslateActionID(int id) const
{
  int m_device = RETRO_DEVICE_JOYPAD; // Until we keep track of multiple devices

  // TODO: Perform better bounds-checking.
  switch (m_device)
  {
  case RETRO_DEVICE_JOYPAD:
    return id - ACTION_JOYBAD_B;
  case RETRO_DEVICE_MOUSE:
    return id - ACTION_MOUSE_CONTROLLER_X;
  case RETRO_DEVICE_LIGHTGUN:
    return id - ACTION_LIGHTGUN_X;
  case RETRO_DEVICE_ANALOG:
    return id - ACTION_ANALOG_CONTROLLER_LEFT;
  case RETRO_DEVICE_KEYBOARD:
    //return id - ACTION_JOYBAD_B; // Keyboard is trickier
  default:
    break;
  }
  return -1; // Invalid device ID
}
