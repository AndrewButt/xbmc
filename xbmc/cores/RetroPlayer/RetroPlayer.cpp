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
#include "games/GameManager.h"
#include "games/tags/GameInfoTag.h"
#include "guilib/GUIWindowManager.h"
#include "guilib/Key.h"
#include "utils/log.h"

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
    m_bAbortRequest(false),
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
  
  const GAME_INFO::CGameInfoTag *tag = file.GetGameInfoTag();
  CFileItem temp;
  if (!tag)
  {
    temp = file;
    if (temp.LoadGameTag())
      tag = temp.GetGameInfoTag();
  }

  if (tag)
  {
    CLog::Log(LOGDEBUG, "RetroPlayer: ---------------------------------------");
    CLog::Log(LOGDEBUG, "RetroPlayer: Game tag loaded");
    CLog::Log(LOGDEBUG, "RetroPlayer: URL: %s", tag->GetURL().c_str());
    CLog::Log(LOGDEBUG, "RetroPlayer: Platform: %s", tag->GetPlatform().c_str());
    CLog::Log(LOGDEBUG, "RetroPlayer: Name: %s", tag->GetName().c_str());
    CLog::Log(LOGDEBUG, "RetroPlayer: Game Code: %s", tag->GetID().c_str());
    CLog::Log(LOGDEBUG, "RetroPlayer: Region: %s", tag->GetRegion().c_str());
    CLog::Log(LOGDEBUG, "RetroPlayer: Publisher: %s", tag->GetPublisher().c_str());
    CLog::Log(LOGDEBUG, "RetroPlayer: Format: %s", tag->GetFormat().c_str());
    CLog::Log(LOGDEBUG, "RetroPlayer: Cartridge Type: %s", tag->GetCartridgeType().c_str());
    CLog::Log(LOGDEBUG, "RetroPlayer: ---------------------------------------");
  }

  m_bAbortRequest = false;
  m_file = file;
  m_PlayerOptions = options;

  m_gameClient = CGameManager::Get().GetGameClient(m_file.GetPath());
  if (!m_gameClient)
  {
    CLog::Log(LOGERROR, "RetroPlayer: Error: no suitable game clients");
    return false;
  }
  if (!m_gameClient->Init())
  {
    CLog::Log(LOGERROR, "RetroPlayer: Failed to init game client %s", m_gameClient->ID().c_str());
    return false;
  }
  CLog::Log(LOGINFO, "RetroPlayer: Using game client %s at version %s", m_gameClient->GetClientName().c_str(), m_gameClient->GetClientVersion().c_str());
  if (!m_gameClient->CanOpen(m_file.GetPath(), true))
  {
    CLog::Log(LOGERROR, "RetroPlayer: Error: Game client %s does not support file", m_gameClient->ID().c_str());
    CLog::Log(LOGERROR, "RetroPlayer: Valid extensions are: %s", m_gameClient->GetExtensions().c_str());
    m_gameClient.reset();
    return false;
  }
  m_retroPlayer = this;
  if (!m_gameClient->OpenFile(file, m_callbacks))
  {
    CLog::Log(LOGERROR, "RetroPlayer: Error opening file");
    m_gameClient.reset();
    return false;
  }
  if (m_gameClient->GetFrameRate() < 5.0 || m_gameClient->GetFrameRate() > 100.0)
  {
    CLog::Log(LOGERROR, "RetroPlayer: Game client reported invalid framerate: %f", (float)m_gameClient->GetFrameRate());
    m_gameClient.reset();
    return false;
  }

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
  m_bAbortRequest = true;
  m_pauseEvent.Set();

  // wait for the main thread to finish up
  // since this main thread cleans up all other resources and threads
  // we are done after the StopThread call
  StopThread(true);

  g_renderManager.UnInit();
  CLog::Log(LOGDEBUG, "RetroPlayer: File closed");
  return true;
}

void CRetroPlayer::Process()
{
  m_video.EnableFullscreen(m_PlayerOptions.fullscreen);

  // Start the video thread
  double framerate = m_gameClient->GetFrameRate();
  if (framerate < 5 || framerate > 100)
  {
    CLog::Log(LOGNOTICE, "RetroPlayer: Game client reported %f fps, assuming 60 fps", framerate);
    framerate = 60;
  }

  // Start the audio thread
  double samplerate = m_gameClient->GetSampleRate();
  if (samplerate < 1 /* ??? */ || samplerate > 384000)
  {
    // TODO: Put checks in CRetroPlayerAudio::SendAudioFrames() for inactivity
    CLog::Log(LOGNOTICE, "RetroPlayer: Game client reported sample rate of %f, continuing without sound", samplerate);
  }
  else
  {
    // Adjust video clock to give us an integer sample rate
    int newSamplerate = static_cast<int>(samplerate);

    if (newSamplerate != samplerate)
    {
      double oldFramerate = framerate;
      framerate *= newSamplerate / samplerate;

      CLog::Log(LOGDEBUG, "RetroPlayer: Frame rate changed from %f to %f", oldFramerate, framerate);
      CLog::Log(LOGDEBUG, "RetroPlayer: Sample rate changed from %f to %d", samplerate, newSamplerate);
    }
    m_audio.GoForth(newSamplerate);
  }

  m_video.GoForth(framerate);
  m_input.Begin();

  const double frametime = 1000 * 1000 / framerate; // useconds
  double nextpts = CDVDClock::GetAbsoluteClock() + frametime;

  CLog::Log(LOGDEBUG, "RetroPlayer: Beginning loop de loop");
  while (!m_bAbortRequest)
  {
    if (m_playSpeed <= PLAYSPEED_PAUSED)
    {
      m_video.Pause();
      m_audio.Pause();
      m_pauseEvent.Wait();
      // Reset the clock
      nextpts = CDVDClock::GetAbsoluteClock() + frametime;
      m_video.UnPause();
      m_audio.UnPause();
      continue;
    }

    // Run the game client for the next frame
    m_gameClient->RunFrame();

    m_video.Tickle();
    // Audio tickling occurs in CRetroPlayerAudio::SendAudioFrames()

    CDVDClock::WaitAbsoluteClock(nextpts);
    nextpts += frametime * PLAYSPEED_NORMAL / m_playSpeed;
  }

  m_video.StopThread();
  m_audio.StopThread();
  m_input.Finish();
}

void CRetroPlayer::OnExit()
{
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
  OnAudioSampleBatch(buf, 1);
}

/* static */
size_t CRetroPlayer::OnAudioSampleBatch(const int16_t *data, size_t frames)
{
  if (data && frames && m_retroPlayer->m_playSpeed == PLAYSPEED_NORMAL)
  {
    int16_t *copy = new int16_t[frames * 2];
    if (copy)
    {
      memcpy(copy, data, frames * 2 * sizeof(int16_t));
      m_retroPlayer->m_audio.SendAudioFrames(copy, frames);
    }
  }
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

  int seek_seconds = bLargeStep ? 10 : 1;
  m_gameClient->RewindFrames(seek_seconds * m_gameClient->GetFrameRate());
}

void CRetroPlayer::SeekPercentage(float fPercent)
{
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
  int max_buffer = m_gameClient->RewindFramesAvailMax();
  if (!max_buffer)
     return 0.0f;

  int current_buffer = m_gameClient->RewindFramesAvail();
  return (current_buffer * 100.0f) / max_buffer;
}

void CRetroPlayer::SeekTime(int64_t iTime)
{
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
  int current_buffer = m_gameClient->RewindFramesAvail();
  return 1000 * current_buffer / m_gameClient->GetFrameRate(); // Millisecs
}

int64_t CRetroPlayer::GetTotalTime()
{
  int max_buffer = m_gameClient->RewindFramesAvailMax();
  return 1000 * max_buffer / m_gameClient->GetFrameRate(); // Millisecs
}

