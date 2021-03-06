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

#include "addons/Addon.h"
#include "FileItem.h"
#include "GameClientDLL.h"

#include <deque>
#include <vector>
#include <utility>
#include <stdint.h>

#define GAMECLIENT_MAX_PLAYERS  8

namespace ADDON
{
  class CGameClient;
  typedef boost::shared_ptr<CGameClient> GameClientPtr;

  class CGameClient : public CAddon
  {
  public:
    /**
     * Loading a file in libretro cores is a complicated process. Game clients
     * support different extensions, some support loading from the VFS, and
     * some have the ability to load ROMs from within zips. Game clients have
     * a tendency to lie about their capabilities. Furthermore, different ROMs
     * can have different results, so it is desireable to try different
     * strategies upon failure.
     */
    class IRetroStrategy
    {
    public:
      IRetroStrategy() : m_useVfs(false) { }
      virtual ~IRetroStrategy() { }
      /**
       * Returns true if this strategy is a viable option. strPath is filled
       * with the file that should be loaded, either the original file or a
       * substitute file.
       */
      virtual bool CanLoad(const CGameClient &gc, const CFileItem& file) = 0;
      /**
       * Populates retro_game_info with results.
       */
      bool GetGameInfo(retro_game_info &info) const;

    protected:
      // Member variables populated with results from CanLoad()
      CStdString m_path;
      bool       m_useVfs;
    };

    /**
     * Load the file from the local hard disk.
     */
    class CStrategyUseHD : public IRetroStrategy
    {
    public:
      virtual bool CanLoad(const CGameClient &gc, const CFileItem& file);
    };

    /**
     * Use the VFS to load the file.
     */
    class CStrategyUseVFS : public IRetroStrategy
    {
    public:
      virtual bool CanLoad(const CGameClient &gc, const CFileItem& file);
    };

    /**
     * If the game client blocks extracting, we don't want to load a file from
     * within a zip. In this case, we try to use the container zip (parent
     * folder on the vfs).
     */
    class CStrategyUseParentZip : public IRetroStrategy
    {
    public:
      virtual bool CanLoad(const CGameClient &gc, const CFileItem& file);
    };

    /**
     * If a zip fails to load, try loading the ROM inside from the zip:// vfs.
     * Avoid recursion clashes with the above strategy.
     */
    class CStrategyEnterZip : public IRetroStrategy
    {
    public:
      virtual bool CanLoad(const CGameClient &gc, const CFileItem& file);
    };

    /**
     * Callback container. Data is passed in and out of the game client through
     * these callbacks.
     */
    struct DataReceiver
    {
      typedef void    (*VideoFrame_t)          (const void *data, unsigned width, unsigned height, size_t pitch);
      typedef void    (*AudioSample_t)         (int16_t left, int16_t right);
      typedef size_t  (*AudioSampleBatch_t)    (const int16_t *data, size_t frames);
      // Actually a "data sender", but who's looking
      typedef int16_t (*GetInputState_t)       (unsigned port, unsigned device, unsigned index, unsigned id);
      typedef void    (*SetPixelFormat_t)      (retro_pixel_format format); // retro_pixel_format defined in libretro.h
      typedef void    (*SetKeyboardCallback_t) (retro_keyboard_event_t callback); // retro_keyboard_event_t defined in libretro.h

      VideoFrame_t          VideoFrame;
      AudioSample_t         AudioSample;
      AudioSampleBatch_t    AudioSampleBatch;
      GetInputState_t       GetInputState;
      SetPixelFormat_t      SetPixelFormat;
      SetKeyboardCallback_t SetKeyboardCallback;

      DataReceiver(VideoFrame_t vf, AudioSample_t as, AudioSampleBatch_t asb, GetInputState_t is, SetPixelFormat_t spf, SetKeyboardCallback_t skc)
        : VideoFrame(vf), AudioSample(as), AudioSampleBatch(asb), GetInputState(is), SetPixelFormat(spf), SetKeyboardCallback(skc) { }
    };

    /**
     * Helper function: If strPath is a zip file, this will enumerate its
     * contents and return the first file inside with a valid extension. If
     * this returns false, effectivePath will be set to strPath.
     */
    static bool GetEffectiveRomPath(const CStdString &zipPath, const CStdStringArray &validExts, CStdString &effectivePath);

    CGameClient(const AddonProps &props);
    CGameClient(const cp_extension_t *props);
    virtual ~CGameClient() { DeInit(); }

    /**
     * Load the DLL and query basic parameters. After Init() is called, the
     * Get*() and CanOpen() functions may be called.
     */
    bool Init();

    /**
     * Cleanly shut down and unload the DLL.
     */
    void DeInit();

    /**
     * Returns true after Init() is called and until DeInit() is called.
     */
    bool IsInitialized() const { return m_dll.IsLoaded(); }

    /**
     * Precondition: Init() must be called first and return true.
     */
    const CStdString &GetClientName() const { return m_clientName; }

    /**
     * Precondition: Init() must be called first and return true.
     */
    const CStdString &GetClientVersion() const { return m_clientVersion; }

    const CStdStringArray &GetPlatforms() const { return m_platforms; }

    /**
     * Returns the suggested extensions, as provided by the DLL.
     * Precondition: Init() must be called first and return true.
     * \return A string delimited by pipes i.e. "bin|rom|iso". This string can
     *         be empty if the client DLL hasn't implemented it.
     */
    const CStdStringArray &GetExtensions() const { return m_validExtensions; }

    /**
     * If the game client was a bad boy and provided no extensions, this will
     * optimistically return true.
     */
    bool IsExtensionValid(const CStdString &ext) const;

    /**
     * The game client allows files to be loaded with no local path.
     */
    bool AllowsVFS() const { return m_bAllowVFS; }

    /**
     * If false, and ROM is in a zip, ROM file must be loaded from within the
     * zip instead of extracted to a temporary cache. In XBMC's case, loading
     * from the VFS is like extraction because the relative paths to the
     * emulator are not available.
     */
    bool BlockZipExtraction() const { return m_bRequireZip; }

    bool OpenFile(const CFileItem &file, const DataReceiver &callbacks);
    void CloseFile();

    /**
     * Find the region of a currently running game. The return value will be
     * RETRO_REGION_NTSC, RETRO_REGION_PAL or -1 for invalid.
     */
    int GetRegion() { return m_region; }

    /**
     * Each port (or player, if you will) must be associated with a device. The
     * default device is RETRO_DEVICE_JOYPAD. For a list of valid devices, see
     * libretro.h.
     *
     * Do not exceed the number of devices that the game client supports. A
     * quick analysis of SNES9x Next v2 showed that a third port will overflow
     * a buffer. Currently, there is no way to determine the number of ports a
     * client will support, so stick with 1.
     *
     * Precondition: OpenFile() must return true.
     */
    void SetDevice(unsigned int port, unsigned int device);

    /**
     * Allow the game to run and produce a video frame. Precondition:
     * OpenFile() returned true.
     */
    void RunFrame();

    /**
     * Rewind gameplay 'frames' frames.
     * As there is a fixed size buffer backing
     * save state deltas, it might not be possible to rewind as many
     * frames as desired. The function returns number of frames actually rewound.
     */
    int RewindFrames(int frames);

    /**
     * Returns how many frames it is possible to rewind
     * with a call to RewindFrames(). */
    int RewindFramesAvail() const { return m_rewindBuffer.size(); }

    /**
     * Returns the maximum amount of frames that can ever
     * be rewound. */
    int RewindFramesAvailMax() const { return m_rewindMaxFrames; }

    /**
     * Reset the game, if running.
     */
    void Reset();

    double GetFrameRate() const { return m_frameRate; }
    double GetSampleRate() const { return m_sampleRate; }

  private:
    void Initialize();

    /**
     * Parse a pipe-separated extensions list, returned from the game client,
     * into an array. The extensions list contains both upper and lower case
     * extensions; only lower-case extensions are stored in m_validExtensions.
     */
    void SetExtensions(const CStdString &strExtensionList);

    static bool EnvironmentCallback(unsigned cmd, void *data);
    static DataReceiver::SetPixelFormat_t _SetPixelFormat; // called by EnvironmentCallback()
    static DataReceiver::SetKeyboardCallback_t _SetKeyboardCallback; // called by EnvironmentCallback()

    GameClientDLL   m_dll;
    CStdStringArray m_platforms;
    bool            m_bIsInited; // Keep track of whether m_dll.retro_init() has been called
    bool            m_bIsPlaying; // This is true between retro_load_game() and retro_unload_game()
    CStdString      m_clientName;
    CStdString      m_clientVersion;
    CStdStringArray m_validExtensions;
    bool            m_bAllowVFS; // Allow files with no local path
    bool            m_bRequireZip; // Don't use VFS for zip files, pass zip path directly
    double          m_frameRate; // Video framerate
    double          m_sampleRate; // Audio frequency
    int             m_region; // Region of the loaded game

    CCriticalSection m_critSection;
    bool m_rewindSupported;
    size_t m_rewindMaxFrames;
    size_t m_serializeSize;
    std::vector<uint32_t> m_lastSaveState;

    /* Rewinding is implemented by applying XOR deltas on the specific parts
     * of the save state buffer which has changed.
     * In practice, this is very fast and simple (linear scan)
     * and allows deltas to be compressed down to 1-3% of original save state size
     * depending on the system. The algorithm runs on 32 bits at a time for speed.
     * The state buffer has a fixed number of frames.
     *
     * Use std::deque here to achieve amortized O(1) on pop/push to front and back.
     */
    typedef std::pair<size_t, uint32_t> DeltaPair;
    typedef std::vector<DeltaPair> DeltaPairVector;
    std::deque<DeltaPairVector> m_rewindBuffer;

    /* Run after retro_run() to append a new state delta to the rewind buffer. */
    void AppendStateDelta();

    /**
     * This callback exists to give XBMC a chance to poll for input. XBMC already
     * takes care of this, so the callback isn't needed.
     */
    static void NoopPoop() { }
  };
}
