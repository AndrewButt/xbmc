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

#include "RetroPlayer.h"
#include "addons/AddonManager.h"
#include "cores/dvdplayer/DVDClock.h"
#include "cores/VideoRenderers/RenderManager.h"
#include "dialogs/GUIDialogBusy.h"
#include "dialogs/GUIDialogContextMenu.h"
#include "games/GameManager.h"
#include "games/tags/GameInfoTag.h"
#include "guilib/GUIWindowManager.h"
#include "guilib/Key.h"
#include "settings/AdvancedSettings.h"
#include "utils/log.h"
#include "utils/StringUtils.h"

#define PLAYSPEED_PAUSED    0
#define PLAYSPEED_NORMAL    1000

using namespace ADDON;

CRetroPlayer *CRetroPlayer::m_retroPlayer = NULL;

// Callback installation
CGameClient::DataReceiver CRetroPlayer::m_callbacks(OnVideoFrame,
                                                    OnAudioSample,
                                                    OnAudioSampleBatch,
                                                    OnInputState,
                                                    OnSetPixelFormat,
                                                    OnSetKeyboardCallback);

retro_keyboard_event_t CRetroPlayer::m_keyboardCallback = NULL;

CRetroPlayer::CRetroPlayer(IPlayerCallback& callback) :
    IPlayer(callback), CThread("RetroPlayer"),
    m_playSpeed(PLAYSPEED_NORMAL)
{
}

CRetroPlayer::~CRetroPlayer()
{
  CloseFile();
}

bool CRetroPlayer::OpenFile(const CFileItem& file, const CPlayerOptions& options)
{
  CLog::Log(LOGINFO, "RetroPlayer: Opening: %s", file.GetPath().c_str());
  m_bStop = false; // What trips you to true?

  if (IsRunning())
    CloseFile();

  // Get game info tag (from a mutable file item, if necessary)
  const GAME_INFO::CGameInfoTag *tag = file.GetGameInfoTag();
  CFileItem temp;
  if (!tag)
  {
    temp = file;
    if (temp.LoadGameTag())
      tag = temp.GetGameInfoTag();
  }

  // Dump discovered infomation to the debug log
  if (tag)
  {
    CLog::Log(LOGDEBUG, "RetroPlayer: ---------------------------------------");
    CLog::Log(LOGDEBUG, "RetroPlayer: Game tag loaded");
    CLog::Log(LOGDEBUG, "RetroPlayer: URL: %s", tag->GetURL().c_str());
    CLog::Log(LOGDEBUG, "RetroPlayer: Platform: %s", tag->GetPlatform().c_str());
    CLog::Log(LOGDEBUG, "RetroPlayer: Title: %s", tag->GetTitle().c_str());
    CLog::Log(LOGDEBUG, "RetroPlayer: Game Code: %s", tag->GetID().c_str());
    CLog::Log(LOGDEBUG, "RetroPlayer: Region: %s", tag->GetRegion().c_str());
    CLog::Log(LOGDEBUG, "RetroPlayer: Publisher: %s", tag->GetPublisher().c_str());
    CLog::Log(LOGDEBUG, "RetroPlayer: Format: %s", tag->GetFormat().c_str());
    CLog::Log(LOGDEBUG, "RetroPlayer: Cartridge Type: %s", tag->GetCartridgeType().c_str());
    CLog::Log(LOGDEBUG, "RetroPlayer: ---------------------------------------");
  }

  // Now we need to see how many game clients contend for this file
  CStdStringArray candidates;
  CGameManager::Get().GetGameClientIDs(file, candidates);
  if (candidates.empty())
  {
    CLog::Log(LOGERROR, "RetroPlayer: Error: no suitable game clients");
    return false;
  }
  else if (candidates.size() == 1)
  {
    AddonPtr addon;
    CAddonMgr::Get().GetAddon(candidates[0], addon, ADDON_GAMEDLL);
    m_gameClient = boost::dynamic_pointer_cast<CGameClient>(addon);
  }
  else
  {
    CLog::Log(LOGDEBUG, "RetroPlayer: Multiple clients found: %s", StringUtils::JoinString(candidates, ", ").c_str());
    std::vector<GameClientPtr> clients;
    CContextButtons choices;
    for (unsigned int i = 0; i < candidates.size(); i++)
    {
      AddonPtr addon;
      CAddonMgr::Get().GetAddon(candidates[i], addon, ADDON_GAMEDLL);
      GameClientPtr client = boost::dynamic_pointer_cast<CGameClient>(addon);
      if (client)
      {
        clients.push_back(client);
        choices.Add(i, client->Name());
      }
    }

    // Choice to go to the add-on manager
    choices.Add(choices.size(), 24025); // "Manage emulators..."

    int btnid = CGUIDialogContextMenu::ShowAndGetChoice(choices);
    if (btnid < 0 || btnid > (int)clients.size())
    {
      CLog::Log(LOGDEBUG, "RetroPlayer: User cancelled game client selection");
      return false;
    }
    else if (btnid == clients.size())
    {
      CLog::Log(LOGDEBUG, "RetroPlayer: User chose add-on manager from game client selection dialog");
      CStdStringArray params;
      params.push_back("addons://all/xbmc.gameclient");
      g_windowManager.ActivateWindow(WINDOW_ADDON_BROWSER, params);
      return false;
    }
    m_gameClient = clients[btnid];
    CLog::Log(LOGDEBUG, "RetroPlayer: Using %s", m_gameClient->ID().c_str());
  }

  // This really just screens against the dynamic pointer cast
  if (!m_gameClient)
  {
    CLog::Log(LOGERROR, "RetroPlayer: Error: no suitable game clients");
    return false;
  }

  // Load the DLL and retrieve system info from the game client
  if (!m_gameClient->Init())
  {
    CLog::Log(LOGERROR, "RetroPlayer: Failed to init game client %s", m_gameClient->ID().c_str());
    return false;
  }

  CLog::Log(LOGINFO, "RetroPlayer: Using game client %s at version %s", m_gameClient->GetClientName().c_str(),
    m_gameClient->GetClientVersion().c_str());

  // We need to store a pointer to ourself before sending the callbacks to the game client
  m_retroPlayer = this;
  if (!m_gameClient->OpenFile(file, m_callbacks))
  {
    CLog::Log(LOGERROR, "RetroPlayer: Error opening file");
    m_gameClient.reset();
    return false;
  }

  // Validate the reported framerate
  if (m_gameClient->GetFrameRate() < 5.0 || m_gameClient->GetFrameRate() > 100.0)
  {
    CLog::Log(LOGERROR, "RetroPlayer: Game client reported invalid framerate: %f", (float)m_gameClient->GetFrameRate());
    m_gameClient.reset();
    return false;
  }

  // Success. We use m_file.GetPath() to check if a file is playing in IsPlaying()
  m_file = file;
  m_PlayerOptions = options;

  g_renderManager.PreInit();
  Create();
  CLog::Log(LOGDEBUG, "RetroPlayer: File opened sucessfully");
  return true;
}

bool CRetroPlayer::CloseFile()
{
  CLog::Log(LOGDEBUG, "RetroPlayer: Closing file");
  // SetPlaySpeed(DVD_PLAYSPEED_NORMAL);

  // Set the abort request so that other threads can finish up
  m_bStop = true;
  m_file = CFileItem();

  // Set m_video.m_bStop to false before triggering the event
  m_video.StopThread(false);
  m_pauseEvent.Set();

  // Wait for the main thread to finish up. Since this main thread cleans up
  // all other resources and threads we are done after the StopThread call.
  StopThread(true);

  g_renderManager.UnInit();

  CLog::Log(LOGDEBUG, "RetroPlayer: File closed");
  return true;
}

void CRetroPlayer::Process()
{
  // Prepare the video thread
  double framerate = m_gameClient->GetFrameRate();
  if (framerate < 5 || framerate > 100)
  {
    CLog::Log(LOGNOTICE, "RetroPlayer: Game client reported %f fps, assuming 60 fps", framerate);
    framerate = 60;
  }

  // Start the audio thread
  const double samplerate = m_gameClient->GetSampleRate();
  // TODO: min/max sample rate?
  if (samplerate < 1 || samplerate > 384000)
  {
    // TODO: Put checks in CRetroPlayerAudio::SendAudioFrames() for inactivity
    CLog::Log(LOGNOTICE, "RetroPlayer: Game client reported sample rate of %f, continuing without sound", samplerate);
  }
  else
  {
    // Adjust video clock to give us an integer sample rate
    const int newSamplerate = (int)samplerate;

    if (newSamplerate != samplerate)
    {
      const double oldFramerate = framerate;
      framerate *= newSamplerate / samplerate;

      CLog::Log(LOGDEBUG, "RetroPlayer: Frame rate changed from %f to %f", oldFramerate, framerate);
      CLog::Log(LOGDEBUG, "RetroPlayer: Sample rate changed from %f to %d", samplerate, newSamplerate);
    }
    m_audio.GoForth(newSamplerate);
  }

  m_video.GoForth(framerate, m_PlayerOptions.fullscreen);
  m_input.Begin();

  const double frametime = 1000 * 1000 / framerate; // useconds
  double nextpts = CDVDClock::GetAbsoluteClock() + frametime;

  CLog::Log(LOGDEBUG, "RetroPlayer: Beginning loop de loop");
  while (!m_bStop)
  {
    if (m_playSpeed <= PLAYSPEED_PAUSED)
    {
      // No need to pause audio or video, the absence of frames will pause it
      m_pauseEvent.Wait();
      // Reset the clock
      nextpts = CDVDClock::GetAbsoluteClock() + frametime;
      continue;
    }

    // Run the game client for the next frame
    m_gameClient->RunFrame();

    // Slow down (increase nextpts) if we're playing catchup after stalling
    if (nextpts < CDVDClock::GetAbsoluteClock())
      nextpts = CDVDClock::GetAbsoluteClock();

    // Slow down to 0.5x (an extra frame) if the audio is delayed
    if (m_audio.GetDelay() * 1000 > g_advancedSettings.m_iGameAudioBuffer)
      nextpts += frametime * PLAYSPEED_NORMAL / m_playSpeed;

    CDVDClock::WaitAbsoluteClock(nextpts);
    nextpts += frametime * PLAYSPEED_NORMAL / m_playSpeed;
  }

  m_video.StopThread(true);
  m_audio.StopThread(true);
  m_input.Finish();
  m_bStop = true;
}

/* static */
void CRetroPlayer::OnVideoFrame(const void *data, unsigned width, unsigned height, size_t pitch)
{
  // Verify all game client data. You don't know where that code's been.
  if (data && width && height && pitch)
    m_retroPlayer->m_video.SendVideoFrame(data, width, height, pitch);
}

/* static */
void CRetroPlayer::OnAudioSample(int16_t left, int16_t right)
{
  int16_t buf[2] = {left, right};
  // Too many small allocations
  //OnAudioSampleBatch(buf, 1);
}

/* static */
size_t CRetroPlayer::OnAudioSampleBatch(const int16_t *data, size_t frames)
{
  if (data && frames && m_retroPlayer->m_playSpeed == PLAYSPEED_NORMAL)
    m_retroPlayer->m_audio.SendAudioFrames(data, frames);
  return frames;
}

/* static */
int16_t CRetroPlayer::OnInputState(unsigned port, unsigned device, unsigned index, unsigned id)
{
  return m_retroPlayer->m_input.GetInput(port, device, index, id);
}

/* static */
void CRetroPlayer::OnSetPixelFormat(retro_pixel_format pixelFormat)
{
  m_retroPlayer->m_video.SetPixelFormat(pixelFormat);
}

/* static */
void CRetroPlayer::OnSetKeyboardCallback(retro_keyboard_event_t callback)
{
  m_keyboardCallback = callback;
}

void CRetroPlayer::Pause()
{
  if (m_playSpeed == PLAYSPEED_PAUSED)
  {
    m_playSpeed = PLAYSPEED_NORMAL;
    m_pauseEvent.Set();
  }
  else
    m_playSpeed = PLAYSPEED_PAUSED;
}

void CRetroPlayer::ToFFRW(int iSpeed /* = 0 */)
{
  bool unpause = (m_playSpeed == PLAYSPEED_PAUSED && iSpeed != PLAYSPEED_PAUSED);
  m_playSpeed = iSpeed * PLAYSPEED_NORMAL;
  if (unpause)
    m_pauseEvent.Set();
}

void CRetroPlayer::Seek(bool bPlus, bool bLargeStep)
{
  if (bPlus) // Cannot seek forward in time.
    return;

  if (!m_gameClient)
    return;

  int seek_seconds = bLargeStep ? 10 : 1;
  m_gameClient->RewindFrames(seek_seconds * m_gameClient->GetFrameRate());
}

void CRetroPlayer::SeekPercentage(float fPercent)
{
  if (!m_gameClient)
    return;

  int max_buffer = m_gameClient->RewindFramesAvailMax();
  if (!max_buffer) // Rewind not supported for game.
     return;

  int current_buffer = m_gameClient->RewindFramesAvail();
  int target_buffer = max_buffer * fPercent / 100.0f;
  int rewind_frames = current_buffer - target_buffer;
  if (rewind_frames > 0)
    m_gameClient->RewindFrames(rewind_frames);
}

float CRetroPlayer::GetPercentage()
{
  if (!m_gameClient)
    return 0.0f;

  int max_buffer = m_gameClient->RewindFramesAvailMax();
  if (!max_buffer)
     return 0.0f;

  int current_buffer = m_gameClient->RewindFramesAvail();
  return (current_buffer * 100.0f) / max_buffer;
}

void CRetroPlayer::SeekTime(int64_t iTime)
{
  if (!m_gameClient)
    return;

  int current_buffer = m_gameClient->RewindFramesAvail();
  if (!current_buffer) // Rewind not supported for game.
     return;

  int target_frame = 1000 * m_gameClient->GetFrameRate() / iTime;
  int rewind_frames = current_buffer - target_frame;
  if (rewind_frames > 0)
    m_gameClient->RewindFrames(rewind_frames);
}

int64_t CRetroPlayer::GetTime()
{
  if (!m_gameClient)
    return 0;

  int current_buffer = m_gameClient->RewindFramesAvail();
  return 1000 * current_buffer / m_gameClient->GetFrameRate(); // Millisecs
}

int64_t CRetroPlayer::GetTotalTime()
{
  if (!m_gameClient)
    return 0;

  int max_buffer = m_gameClient->RewindFramesAvailMax();
  return 1000 * max_buffer / m_gameClient->GetFrameRate(); // Millisecs
}

