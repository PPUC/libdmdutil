#include "DMDUtil/DMD.h"

#include "DMDUtil/Config.h"
#include "DMDUtil/ConsoleDMD.h"
#include "DMDUtil/LevelDMD.h"
#include "DMDUtil/RGB24DMD.h"

#if !(                                                                                                                \
    (defined(__APPLE__) && ((defined(TARGET_OS_IOS) && TARGET_OS_IOS) || (defined(TARGET_OS_TV) && TARGET_OS_TV))) || \
    defined(__ANDROID__))
#include "PixelcadeDMD.h"
#endif

#include <algorithm>
#include <chrono>
#include <cstring>

#include "AlphaNumeric.h"
#include "FrameUtil.h"
#include "Logger.h"
#include "ZeDMD.h"
#include "pupdmd.h"
#include "serum-decode.h"
#include "serum.h"

namespace DMDUtil
{

void PUPDMDCALLBACK PUPDMDLogCallback(const char* format, va_list args, const void* pUserData)
{
  char buffer[1024];
  vsnprintf(buffer, sizeof(buffer), format, args);

  Log(DMDUtil_LogLevel_INFO, "%s", buffer);
}

void ZEDMDCALLBACK ZeDMDLogCallback(const char* format, va_list args, const void* pUserData)
{
  char buffer[1024];
  vsnprintf(buffer, sizeof(buffer), format, args);

  Log(DMDUtil_LogLevel_INFO, "%s", buffer);
}

bool DMD::m_finding = false;

DMD::DMD()
{
  for (uint8_t i = 0; i < DMDUTIL_FRAME_BUFFER_SIZE; i++)
  {
    m_pUpdateBufferQueue[i] = new Update();
  }
  m_updateBufferQueuePosition.store(0, std::memory_order_release);
  m_stopFlag.store(false, std::memory_order_release);
  m_dmdFrameReady.store(false, std::memory_order_release);
  m_updateBuffered = std::make_shared<Update>();

  m_pAlphaNumeric = new AlphaNumeric();
  m_pSerum = nullptr;
  m_pZeDMD = nullptr;
  m_pPUPDMD = nullptr;
  m_pZeDMDThread = nullptr;
  m_pLevelDMDThread = nullptr;
  m_pRGB24DMDThread = nullptr;
  m_pConsoleDMDThread = nullptr;
  m_pDumpDMDTxtThread = nullptr;
  m_pDumpDMDRawThread = nullptr;
#if !(                                                                                                                \
    (defined(__APPLE__) && ((defined(TARGET_OS_IOS) && TARGET_OS_IOS) || (defined(TARGET_OS_TV) && TARGET_OS_TV))) || \
    defined(__ANDROID__))
  m_pPixelcadeDMD = nullptr;
  m_pPixelcadeDMDThread = nullptr;
#endif

  m_pDmdFrameThread = new std::thread(&DMD::DmdFrameThread, this);
  m_pPupDMDThread = new std::thread(&DMD::PupDMDThread, this);
  m_pSerumThread = new std::thread(&DMD::SerumThread, this);
  m_pDMDServerConnector = nullptr;
}

DMD::~DMD()
{
  std::unique_lock<std::shared_mutex> ul(m_dmdSharedMutex);
  m_stopFlag.store(true, std::memory_order_release);
  ul.unlock();
  m_dmdCV.notify_all();

  m_pDmdFrameThread->join();
  delete m_pDmdFrameThread;
  m_pDmdFrameThread = nullptr;

  if (m_pLevelDMDThread)
  {
    m_pLevelDMDThread->join();
    delete m_pLevelDMDThread;
    m_pLevelDMDThread = nullptr;
  }

  if (m_pRGB24DMDThread)
  {
    m_pRGB24DMDThread->join();
    delete m_pRGB24DMDThread;
    m_pRGB24DMDThread = nullptr;
  }

  if (m_pConsoleDMDThread)
  {
    m_pConsoleDMDThread->join();
    delete m_pConsoleDMDThread;
    m_pConsoleDMDThread = nullptr;
  }

  if (m_pZeDMDThread)
  {
    m_pZeDMDThread->join();
    delete m_pZeDMDThread;
    m_pZeDMDThread = nullptr;
  }

  if (m_pDumpDMDTxtThread)
  {
    m_pDumpDMDTxtThread->join();
    delete m_pDumpDMDTxtThread;
    m_pDumpDMDTxtThread = nullptr;
  }

  if (m_pDumpDMDRawThread)
  {
    m_pDumpDMDRawThread->join();
    delete m_pDumpDMDRawThread;
    m_pDumpDMDRawThread = nullptr;
  }

  if (m_pPupDMDThread)
  {
    m_pPupDMDThread->join();
    delete m_pPupDMDThread;
    m_pPupDMDThread = nullptr;
  }

  if (m_pSerumThread)
  {
    m_pSerumThread->join();
    delete m_pSerumThread;
    m_pSerumThread = nullptr;
  }

#if !(                                                                                                                \
    (defined(__APPLE__) && ((defined(TARGET_OS_IOS) && TARGET_OS_IOS) || (defined(TARGET_OS_TV) && TARGET_OS_TV))) || \
    defined(__ANDROID__))
  if (m_pPixelcadeDMDThread)
  {
    m_pPixelcadeDMDThread->join();
    delete m_pPixelcadeDMDThread;
    m_pPixelcadeDMDThread = nullptr;
  }
#endif
  delete m_pAlphaNumeric;
  delete m_pZeDMD;
  delete m_pPUPDMD;
#if !(                                                                                                                \
    (defined(__APPLE__) && ((defined(TARGET_OS_IOS) && TARGET_OS_IOS) || (defined(TARGET_OS_TV) && TARGET_OS_TV))) || \
    defined(__ANDROID__))
  if (m_pPixelcadeDMD) delete m_pPixelcadeDMD;
#endif

  for (LevelDMD* pLevelDMD : m_levelDMDs) delete pLevelDMD;
  for (RGB24DMD* pRGB24DMD : m_rgb24DMDs) delete pRGB24DMD;
  for (ConsoleDMD* pConsoleDMD : m_consoleDMDs) delete pConsoleDMD;

  if (m_pDMDServerConnector)
  {
    m_pDMDServerConnector->close();
    delete m_pDMDServerConnector;
    m_pDMDServerConnector = nullptr;
  }
}

bool DMD::ConnectDMDServer()
{
  if (!m_pDMDServerConnector)
  {
    Config* const pConfig = Config::GetInstance();
    sockpp::initialize();
    Log(DMDUtil_LogLevel_INFO, "Connecting DMDServer on %s:%d", pConfig->GetDMDServerAddr(),
        pConfig->GetDMDServerPort());
    m_pDMDServerConnector =
        new sockpp::tcp_connector({pConfig->GetDMDServerAddr(), (in_port_t)pConfig->GetDMDServerPort()});
    if (!m_pDMDServerConnector)
    {
      Log(DMDUtil_LogLevel_INFO, "DMDServer connection to %s:%d failed!", pConfig->GetDMDServerAddr(),
          pConfig->GetDMDServerPort());
    }
  }
  return (m_pDMDServerConnector);
}

bool DMD::IsFinding() { return m_finding; }

bool DMD::HasDisplay() const
{
#if !(                                                                                                                \
    (defined(__APPLE__) && ((defined(TARGET_OS_IOS) && TARGET_OS_IOS) || (defined(TARGET_OS_TV) && TARGET_OS_TV))) || \
    defined(__ANDROID__))
  return (m_pZeDMD != nullptr) || (m_pPixelcadeDMD != nullptr);
#else
  return (m_pZeDMD != nullptr);
#endif
}

bool DMD::HasHDDisplay() const { return (m_pZeDMD != nullptr && m_pZeDMD->GetWidth() == 256); }

void DMD::SetRomName(const char* name) { strcpy(m_romName, name ? name : ""); }

void DMD::SetAltColorPath(const char* path) { strcpy(m_altColorPath, path ? path : ""); }

void DMD::SetPUPVideosPath(const char* path) { strcpy(m_pupVideosPath, path ? path : ""); }

void DMD::DumpDMDTxt() { m_pDumpDMDTxtThread = new std::thread(&DMD::DumpDMDTxtThread, this); }

void DMD::DumpDMDRaw() { m_pDumpDMDRawThread = new std::thread(&DMD::DumpDMDRawThread, this); }

LevelDMD* DMD::CreateLevelDMD(uint16_t width, uint16_t height, bool sam)
{
  LevelDMD* const pLevelDMD = new LevelDMD(width, height, sam);
  m_levelDMDs.push_back(pLevelDMD);
  if (!m_pLevelDMDThread) m_pLevelDMDThread = new std::thread(&DMD::LevelDMDThread, this);
  return pLevelDMD;
}

bool DMD::DestroyLevelDMD(LevelDMD* pLevelDMD)
{
  auto it = std::find(m_levelDMDs.begin(), m_levelDMDs.end(), pLevelDMD);
  if (it != m_levelDMDs.end())
  {
    m_levelDMDs.erase(it);
    delete pLevelDMD;

    if (m_levelDMDs.empty())
    {
      //@todo terminate LevelDMDThread
    }

    return true;
  }
  return false;
}

RGB24DMD* DMD::CreateRGB24DMD(uint16_t width, uint16_t height)
{
  RGB24DMD* const pRGB24DMD = new RGB24DMD(width, height);
  m_rgb24DMDs.push_back(pRGB24DMD);
  if (!m_pRGB24DMDThread) m_pRGB24DMDThread = new std::thread(&DMD::RGB24DMDThread, this);
  return pRGB24DMD;
}

bool DMD::DestroyRGB24DMD(RGB24DMD* pRGB24DMD)
{
  auto it = std::find(m_rgb24DMDs.begin(), m_rgb24DMDs.end(), pRGB24DMD);
  if (it != m_rgb24DMDs.end())
  {
    m_rgb24DMDs.erase(it);
    delete pRGB24DMD;

    if (m_rgb24DMDs.empty())
    {
      //@todo terminate RGB24DMDThread
    }

    return true;
  }
  return false;
}

ConsoleDMD* DMD::CreateConsoleDMD(bool overwrite, FILE* out)
{
  ConsoleDMD* const pConsoleDMD = new ConsoleDMD(overwrite, out);
  m_consoleDMDs.push_back(pConsoleDMD);
  if (!m_pConsoleDMDThread) m_pConsoleDMDThread = new std::thread(&DMD::ConsoleDMDThread, this);
  return pConsoleDMD;
}

bool DMD::DestroyConsoleDMD(ConsoleDMD* pConsoleDMD)
{
  auto it = std::find(m_consoleDMDs.begin(), m_consoleDMDs.end(), pConsoleDMD);
  if (it != m_consoleDMDs.end())
  {
    m_consoleDMDs.erase(it);
    delete pConsoleDMD;

    if (m_consoleDMDs.empty())
    {
      //@todo terminate ConsoleDMDThread
    }

    return true;
  }
  return false;
}

void DMD::UpdateData(const uint8_t* pData, int depth, uint16_t width, uint16_t height, uint8_t r, uint8_t g, uint8_t b,
                     Mode mode, bool buffered)
{
  auto dmdUpdate = std::make_shared<Update>();
  dmdUpdate->mode = mode;
  dmdUpdate->depth = depth;
  dmdUpdate->width = width;
  dmdUpdate->height = height;
  if (pData)
  {
    memcpy(dmdUpdate->data, pData, width * height * (mode == Mode::RGB16 ? 2 : (mode == Mode::RGB24 ? 3 : 1)));
    dmdUpdate->hasData = true;
  }
  else
  {
    dmdUpdate->hasData = false;
  }
  dmdUpdate->hasSegData = false;
  dmdUpdate->hasSegData2 = false;
  dmdUpdate->r = r;
  dmdUpdate->g = g;
  dmdUpdate->b = b;

  QueueUpdate(dmdUpdate, buffered);
}

void DMD::QueueUpdate(const std::shared_ptr<Update> dmdUpdate, bool buffered)
{
  std::thread(
      [this, dmdUpdate, buffered]()
      {
        std::unique_lock<std::shared_mutex> ul(m_dmdSharedMutex);
        uint8_t updateBufferQueuePosition = m_updateBufferQueuePosition.load(std::memory_order_acquire);
        if (++updateBufferQueuePosition >= DMDUTIL_FRAME_BUFFER_SIZE) updateBufferQueuePosition = 0;
        memcpy(m_pUpdateBufferQueue[updateBufferQueuePosition], dmdUpdate.get(), sizeof(Update));
        m_updateBufferQueuePosition.store(updateBufferQueuePosition, std::memory_order_release);
        m_dmdFrameReady.store(true, std::memory_order_release);

        // Log(DMDUtil_LogLevel_DEBUG, "Queued Frame: position=%d, mode=%d, depth=%d", updateBufferQueuePosition,
        // dmdUpdate->mode, dmdUpdate->depth);

        if (buffered)
        {
          memcpy(m_updateBuffered.get(), dmdUpdate.get(), sizeof(Update));
          m_hasUpdateBuffered = true;
        }

        ul.unlock();
        m_dmdCV.notify_all();

        if (m_pDMDServerConnector && !IsSerumMode(dmdUpdate->mode))
        {
          StreamHeader streamHeader;
          streamHeader.buffered = (uint8_t)buffered;
          streamHeader.disconnectOthers = (uint8_t)m_dmdServerDisconnectOthers;
          m_pDMDServerConnector->write_n(&streamHeader, sizeof(StreamHeader));
          PathsHeader pathsHeader;
          strcpy(pathsHeader.name, m_romName);
          strcpy(pathsHeader.altColorPath, m_altColorPath);
          strcpy(pathsHeader.pupVideosPath, m_pupVideosPath);
          m_pDMDServerConnector->write_n(&pathsHeader, sizeof(PathsHeader));
          m_pDMDServerConnector->write_n(&dmdUpdate, sizeof(Update));

          if (streamHeader.disconnectOthers != 0) m_dmdServerDisconnectOthers = false;
        }
      })
      .detach();
}

bool DMD::QueueBuffer()
{
  if (m_hasUpdateBuffered)
  {
    QueueUpdate(m_updateBuffered, false);
  }

  return m_hasUpdateBuffered;
}

void DMD::UpdateData(const uint8_t* pData, int depth, uint16_t width, uint16_t height, uint8_t r, uint8_t g, uint8_t b,
                     bool buffered)
{
  UpdateData(pData, depth, width, height, r, g, b, Mode::Data, buffered);
}

void DMD::UpdateRGB24Data(const uint8_t* pData, int depth, uint16_t width, uint16_t height, uint8_t r, uint8_t g,
                          uint8_t b, bool buffered)
{
  UpdateData(pData, depth, width, height, r, g, b, Mode::RGB24, buffered);
}

void DMD::UpdateRGB24Data(const uint8_t* pData, uint16_t width, uint16_t height, bool buffered)
{
  UpdateData(pData, 24, width, height, 0, 0, 0, Mode::RGB24, buffered);
}

void DMD::UpdateRGB16Data(const uint16_t* pData, uint16_t width, uint16_t height, bool buffered)
{
  auto dmdUpdate = std::make_shared<Update>();
  dmdUpdate->mode = Mode::RGB16;
  dmdUpdate->depth = 24;
  dmdUpdate->width = width;
  dmdUpdate->height = height;
  if (pData)
  {
    memcpy(dmdUpdate->segData, pData, width * height * sizeof(uint16_t));
    dmdUpdate->hasData = true;
  }
  else
  {
    dmdUpdate->hasData = false;
  }
  dmdUpdate->hasSegData = false;
  dmdUpdate->hasSegData2 = false;

  QueueUpdate(dmdUpdate, buffered);
}

void DMD::UpdateAlphaNumericData(AlphaNumericLayout layout, const uint16_t* pData1, const uint16_t* pData2, uint8_t r,
                                 uint8_t g, uint8_t b)
{
  auto dmdUpdate = std::make_shared<Update>();
  dmdUpdate->mode = Mode::AlphaNumeric;
  dmdUpdate->layout = layout;
  dmdUpdate->depth = 2;
  dmdUpdate->width = 128;
  dmdUpdate->height = 32;
  dmdUpdate->hasData = false;
  if (pData1)
  {
    memcpy(dmdUpdate->segData, pData1, 128 * sizeof(uint16_t));
    dmdUpdate->hasSegData = true;
  }
  else
  {
    dmdUpdate->hasSegData = false;
  }
  if (pData2)
  {
    memcpy(dmdUpdate->segData2, pData2, 128 * sizeof(uint16_t));
    dmdUpdate->hasSegData2 = true;
  }
  else
  {
    dmdUpdate->hasSegData2 = false;
  }
  dmdUpdate->r = r;
  dmdUpdate->g = g;
  dmdUpdate->b = b;

  QueueUpdate(dmdUpdate, false);
}

void DMD::FindDisplays()
{
  if (m_finding) return;

  Config* const pConfig = Config::GetInstance();
  if (pConfig->IsDmdServer())
  {
    ConnectDMDServer();
  }
  else
  {
    m_finding = true;

    std::thread(
        [this]()
        {
          Config* const pConfig = Config::GetInstance();

          ZeDMD* pZeDMD = nullptr;

          if (pConfig->IsZeDMD())
          {
            pZeDMD = new ZeDMD();
            pZeDMD->SetLogCallback(ZeDMDLogCallback, nullptr);

            bool open = false;

            if (pConfig->IsZeDMDWifiEnabled())
            {
              std::string wifiAddr = pConfig->GetZeDMDWifiAddr() ? pConfig->GetZeDMDWifiAddr() : "";
              uint16_t udpPortNumber = pConfig->GetZeDMDWifiPort() > 0 ? pConfig->GetZeDMDWifiPort() : 3333;

              if (wifiAddr.empty())
              {
                DMDUtil::Log(DMDUtil_LogLevel_ERROR, "ERROR: ZeDMD Wifi IP address is not configured.");
              }

              // Proceed only if the wifiAddr is valid.
              if (!wifiAddr.empty() && (open = pZeDMD->OpenWiFi(wifiAddr.c_str(), udpPortNumber)))
              {
                // Fix RGB and brightness
                std::stringstream logMessage;
                logMessage << "ZeDMD Wifi enabled, connected to " << wifiAddr << ":" << udpPortNumber << ".";
                DMDUtil::Log(DMDUtil_LogLevel_INFO, logMessage.str().c_str());
              }
            }
            else  // Serial communication
            {
              if (pConfig->GetZeDMDDevice() != nullptr && pConfig->GetZeDMDDevice()[0] != '\0')
                pZeDMD->SetDevice(pConfig->GetZeDMDDevice());

              bool open = false;
              if ((open = pZeDMD->Open()))
              {
                if (pConfig->GetZeDMDBrightness() != -1) pZeDMD->SetBrightness(pConfig->GetZeDMDBrightness());
                if (pConfig->IsZeDMDSaveSettings())
                {
                  if (pConfig->GetZeDMDRGBOrder() != -1) pZeDMD->SetRGBOrder(pConfig->GetZeDMDRGBOrder());
                  pZeDMD->SaveSettings();
                  if (pConfig->GetZeDMDRGBOrder() != -1)
                  {
                    // Setting the RGBOrder requires a reboot.
                    pZeDMD->Reset();
                    std::this_thread::sleep_for(std::chrono::seconds(8));
                    pZeDMD->Close();
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                    open = pZeDMD->Open();
                  }
                }
              }
            }
            if (open)
            {
              if (pConfig->IsZeDMDDebug()) pZeDMD->EnableDebug();
              pZeDMD->EnablePreDownscaling();
              pZeDMD->EnablePreUpscaling();
              m_pZeDMDThread = new std::thread(&DMD::ZeDMDThread, this);
            }
            else
            {
              delete pZeDMD;
              pZeDMD = nullptr;
            }
          }

          m_pZeDMD = pZeDMD;

#if !(                                                                                                                \
    (defined(__APPLE__) && ((defined(TARGET_OS_IOS) && TARGET_OS_IOS) || (defined(TARGET_OS_TV) && TARGET_OS_TV))) || \
    defined(__ANDROID__))
          PixelcadeDMD* pPixelcadeDMD = nullptr;

          if (pConfig->IsPixelcade())
          {
            pPixelcadeDMD = PixelcadeDMD::Connect(pConfig->GetPixelcadeDevice(), 128, 32);
            if (pPixelcadeDMD) m_pPixelcadeDMDThread = new std::thread(&DMD::PixelcadeDMDThread, this);
          }

          m_pPixelcadeDMD = pPixelcadeDMD;
#endif

          m_finding = false;
        })
        .detach();
  }
}

uint8_t DMD::GetNextBufferQueuePosition(uint8_t bufferPosition, const uint8_t updateBufferQueuePosition)
{
  if (++bufferPosition >= DMDUTIL_FRAME_BUFFER_SIZE) bufferPosition = 0;

  if (bufferPosition < updateBufferQueuePosition)
  {
    if ((updateBufferQueuePosition - bufferPosition) > DMDUTIL_MAX_FRAMES_BEHIND)
      return updateBufferQueuePosition - DMDUTIL_MIN_FRAMES_BEHIND;
  }
  else if (bufferPosition > updateBufferQueuePosition)
  {
    if ((DMDUTIL_FRAME_BUFFER_SIZE - bufferPosition + updateBufferQueuePosition) > DMDUTIL_MAX_FRAMES_BEHIND)
    {
      if (updateBufferQueuePosition >= DMDUTIL_MIN_FRAMES_BEHIND)
        return updateBufferQueuePosition - DMDUTIL_MIN_FRAMES_BEHIND;
      else
        return DMDUTIL_FRAME_BUFFER_SIZE - DMDUTIL_MIN_FRAMES_BEHIND + updateBufferQueuePosition;
    }
  }

  return bufferPosition;
}

void DMD::DmdFrameThread()
{
  char name[DMDUTIL_MAX_NAME_SIZE] = {0};

  m_dmdFrameReady.load(std::memory_order_acquire);
  m_stopFlag.load(std::memory_order_acquire);

  while (true)
  {
    std::shared_lock<std::shared_mutex> sl(m_dmdSharedMutex);
    m_dmdCV.wait(
        sl, [&]()
        { return m_dmdFrameReady.load(std::memory_order_relaxed) || m_stopFlag.load(std::memory_order_relaxed); });
    sl.unlock();

    if (strcmp(m_romName, name) != 0)
    {
      strcpy(name, m_romName);

      // In case of a new ROM, try to disconnect the other clients.
      if (m_pDMDServerConnector) m_dmdServerDisconnectOthers = true;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(2));

    std::unique_lock<std::shared_mutex> ul(m_dmdSharedMutex);
    m_dmdFrameReady.store(false, std::memory_order_release);
    ul.unlock();

    if (m_stopFlag)
    {
      return;
    }
  }
}

void DMD::ZeDMDThread()
{
  uint8_t bufferPosition = 0;
  uint16_t width = 0;
  uint16_t height = 0;
  uint16_t segData1[128] = {0};
  uint16_t segData2[128] = {0};
  uint8_t palette[PALETTE_SIZE] = {0};

  m_dmdFrameReady.load(std::memory_order_acquire);
  m_stopFlag.load(std::memory_order_acquire);

  while (true)
  {
    std::shared_lock<std::shared_mutex> sl(m_dmdSharedMutex);
    m_dmdCV.wait(
        sl, [&]()
        { return m_dmdFrameReady.load(std::memory_order_relaxed) || m_stopFlag.load(std::memory_order_relaxed); });
    sl.unlock();

    if (m_stopFlag.load(std::memory_order_acquire))
    {
      return;
    }

    const uint8_t updateBufferQueuePosition = m_updateBufferQueuePosition.load(std::memory_order_acquire);
    while (!m_stopFlag.load(std::memory_order_relaxed) && bufferPosition != updateBufferQueuePosition)
    {
      bufferPosition = GetNextBufferQueuePosition(bufferPosition, updateBufferQueuePosition);

      if (m_pSerum && !IsSerumMode(m_pUpdateBufferQueue[bufferPosition]->mode)) continue;

      // Note: libzedmd has its own update detection.

      if (m_pUpdateBufferQueue[bufferPosition]->hasData || m_pUpdateBufferQueue[bufferPosition]->hasSegData)
      {
        if (m_pUpdateBufferQueue[bufferPosition]->width != width ||
            m_pUpdateBufferQueue[bufferPosition]->height != height)
        {
          width = m_pUpdateBufferQueue[bufferPosition]->width;
          height = m_pUpdateBufferQueue[bufferPosition]->height;
          // Activate the correct scaling mode.
          m_pZeDMD->SetFrameSize(width, height);
        }

        bool update = false;
        if (m_pUpdateBufferQueue[bufferPosition]->depth != 24)
        {
          update = UpdatePalette(palette, m_pUpdateBufferQueue[bufferPosition]->depth,
                                 m_pUpdateBufferQueue[bufferPosition]->r, m_pUpdateBufferQueue[bufferPosition]->g,
                                 m_pUpdateBufferQueue[bufferPosition]->b);
        }
        if (m_pUpdateBufferQueue[bufferPosition]->mode == Mode::RGB24)
        {
          // ZeDMD HD supports 256 * 64 pixels.
          uint8_t rgb24Data[256 * 64 * 3];

          AdjustRGB24Depth(m_pUpdateBufferQueue[bufferPosition]->data, rgb24Data, width * height, palette,
                           m_pUpdateBufferQueue[bufferPosition]->depth);
          m_pZeDMD->DisablePreUpscaling();
          m_pZeDMD->RenderRgb24(rgb24Data);
          m_pZeDMD->EnablePreUpscaling();
        }
        else if (m_pUpdateBufferQueue[bufferPosition]->mode == Mode::RGB16)
        {
          m_pZeDMD->DisablePreUpscaling();
          m_pZeDMD->RenderRgb565(m_pUpdateBufferQueue[bufferPosition]->segData);
          m_pZeDMD->EnablePreUpscaling();
        }
        else if (m_pSerum && IsSerumMode(m_pUpdateBufferQueue[bufferPosition]->mode))
        {
          if ((m_pZeDMD->GetWidth() == 256 && m_pUpdateBufferQueue[bufferPosition]->mode == Mode::SerumV2_32_64) ||
              (m_pZeDMD->GetWidth() < 256 && m_pUpdateBufferQueue[bufferPosition]->mode == Mode::SerumV2_64_32))
            continue;

          if (IsSerumV2Mode(m_pUpdateBufferQueue[bufferPosition]->mode))
          {
            m_pZeDMD->RenderRgb565(m_pUpdateBufferQueue[bufferPosition]->segData);
          }
          else
          {
            // Note that uint16_t segData is used to transport the uint8_t palette data to keep the dmdserver protocol
            // stable.
            memcpy(palette, m_pUpdateBufferQueue[bufferPosition]->segData, PALETTE_SIZE);
            m_pZeDMD->RenderColoredGray6(m_pUpdateBufferQueue[bufferPosition]->data, palette, nullptr);
          }
        }
        else if (!m_pSerum)
        {
          if (m_pUpdateBufferQueue[bufferPosition]->mode == Mode::Data)
          {
            m_pZeDMD->SetPalette(palette, m_pUpdateBufferQueue[bufferPosition]->depth == 2 ? 4 : 16);

            switch (m_pUpdateBufferQueue[bufferPosition]->depth)
            {
              case 2:
                m_pZeDMD->RenderGray2(m_pUpdateBufferQueue[bufferPosition]->data);
                break;

              case 4:
                m_pZeDMD->RenderGray4(m_pUpdateBufferQueue[bufferPosition]->data);
                break;

              default:
                //@todo log error
                break;
            }
          }
          else if (m_pUpdateBufferQueue[bufferPosition]->mode == Mode::AlphaNumeric)
          {
            if (memcmp(segData1, m_pUpdateBufferQueue[bufferPosition]->segData, sizeof(segData1)) != 0)
            {
              memcpy(segData1, m_pUpdateBufferQueue[bufferPosition]->segData, sizeof(segData1));
              update = true;
            }

            if (m_pUpdateBufferQueue[bufferPosition]->hasSegData2 &&
                memcmp(segData2, m_pUpdateBufferQueue[bufferPosition]->segData2, sizeof(segData2)) != 0)
            {
              memcpy(segData2, m_pUpdateBufferQueue[bufferPosition]->segData2, sizeof(segData2));
              update = true;
            }

            if (update)
            {
              // ZeDMD HD supports 256 * 64 pixels.
              uint8_t renderBuffer[256 * 64];

              if (m_pUpdateBufferQueue[bufferPosition]->hasSegData2)
                m_pAlphaNumeric->Render(renderBuffer, m_pUpdateBufferQueue[bufferPosition]->layout, segData1, segData2);
              else
                m_pAlphaNumeric->Render(renderBuffer, m_pUpdateBufferQueue[bufferPosition]->layout, segData1);

              m_pZeDMD->SetPalette(palette, 4);
              m_pZeDMD->RenderGray2(renderBuffer);
            }
          }
        }
      }
    }
  }
}

void DMD::SerumThread()
{
  if (Config::GetInstance()->IsAltColor())
  {
    uint8_t bufferPosition = 0;
    uint32_t prevTriggerId = 0;
    char name[DMDUTIL_MAX_NAME_SIZE] = {0};
    uint32_t nextRotation = 0;
    Update* lastDmdUpdate = nullptr;

    m_dmdFrameReady.load(std::memory_order_acquire);
    m_stopFlag.load(std::memory_order_acquire);

    while (true)
    {
      std::shared_lock<std::shared_mutex> sl(m_dmdSharedMutex);
      m_dmdCV.wait(sl,
                   [&]()
                   {
                     return m_dmdFrameReady.load(std::memory_order_relaxed) ||
                            m_stopFlag.load(std::memory_order_relaxed) ||
                            (nextRotation > 0 && std::chrono::duration_cast<std::chrono::milliseconds>(
                                                     std::chrono::system_clock::now().time_since_epoch())
                                                         .count() > nextRotation);
                   });
      sl.unlock();

      if (m_stopFlag.load(std::memory_order_acquire))
      {
        if (m_pSerum)
        {
          Serum_Dispose();
        }

        return;
      }

      uint32_t now =
          std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
              .count();

      const uint8_t updateBufferQueuePosition = m_updateBufferQueuePosition.load(std::memory_order_acquire);
      while (!m_stopFlag.load(std::memory_order_relaxed) && bufferPosition != updateBufferQueuePosition)
      {
        if (++bufferPosition >= DMDUTIL_FRAME_BUFFER_SIZE) bufferPosition = 0;

        if (m_pUpdateBufferQueue[bufferPosition]->mode == Mode::Data)
        {
          if (strcmp(m_romName, name) != 0)
          {
            strcpy(name, m_romName);

            if (m_pSerum)
            {
              Serum_Dispose();
              m_pSerum = nullptr;
              lastDmdUpdate = nullptr;
            }

            if (m_altColorPath[0] == '\0') strcpy(m_altColorPath, Config::GetInstance()->GetAltColorPath());

            m_pSerum = (name[0] != '\0')
                           ? Serum_Load(m_altColorPath, m_romName, FLAG_REQUEST_32P_FRAMES | FLAG_REQUEST_64P_FRAMES)
                           : nullptr;
            if (m_pSerum)
            {
              Log(DMDUtil_LogLevel_INFO, "Loaded Serum v%d colorization for %s", m_pSerum->SerumVersion, m_romName);

              Serum_SetIgnoreUnknownFramesTimeout(Config::GetInstance()->GetIgnoreUnknownFramesTimeout());
              Serum_SetMaximumUnknownFramesToSkip(Config::GetInstance()->GetMaximumUnknownFramesToSkip());
            }
          }

          if (m_pSerum)
          {
            uint32_t result = Serum_Colorize(m_pUpdateBufferQueue[bufferPosition]->data);

            if (result != IDENTIFY_NO_FRAME)
            {
              // Log(DMDUtil_LogLevel_DEBUG, "Serum: frameID=%lu, rotation=%lu, flags=%lu", m_pSerum->frameID,
              // m_pSerum->rotationtimer, m_pSerum->flags);

              lastDmdUpdate = m_pUpdateBufferQueue[bufferPosition];
              QueueSerumFrames(lastDmdUpdate);

              if (result > 0 && ((result & 0xffff) < 2048))
                nextRotation = now + m_pSerum->rotationtimer;
              else
                nextRotation = 0;

              if (m_pSerum->triggerID < 0xffffffff & m_pSerum->triggerID != prevTriggerId)
              {
                HandleTrigger(m_pSerum->triggerID);
                prevTriggerId = m_pSerum->triggerID;
              }
            }
          }
        }
      }

      if (!m_stopFlag.load(std::memory_order_acquire) && m_pSerum && nextRotation > 0 && m_pSerum->rotationtimer > 0 &&
          lastDmdUpdate && now > nextRotation)
      {
        uint32_t result = Serum_Rotate();

        // Log(DMDUtil_LogLevel_DEBUG, "Serum: rotation=%lu, flags=%lu", m_pSerum->rotationtimer, result >> 16);

        QueueSerumFrames(lastDmdUpdate, result & 0x10000, result & 0x20000);

        if (result > 0 && ((result & 0xffff) < 2048))
        {
          nextRotation = now + m_pSerum->rotationtimer;
        }
        else
          nextRotation = 0;
      }
    }
  }
}

void DMD::QueueSerumFrames(Update* dmdUpdate, bool render32, bool render64)
{
  if (!render32 && !render64) return;

  auto serumUpdate = std::make_shared<Update>();
  serumUpdate->hasData = true;
  serumUpdate->hasSegData = false;
  serumUpdate->hasSegData2 = false;

  if (m_pSerum->SerumVersion == SERUM_V1 && render32)
  {
    serumUpdate->mode = Mode::SerumV1;
    serumUpdate->depth = 6;
    serumUpdate->width = dmdUpdate->width;
    serumUpdate->height = dmdUpdate->height;
    memcpy(serumUpdate->data, m_pSerum->frame, dmdUpdate->width * dmdUpdate->height);
    memcpy(serumUpdate->segData, m_pSerum->palette, PALETTE_SIZE);

    QueueUpdate(serumUpdate, false);
  }
  else if (m_pSerum->SerumVersion == SERUM_V2)
  {
    if (m_pSerum->width32 > 0 && m_pSerum->width64 == 0)
    {
      if (render32)
      {
        serumUpdate->mode = Mode::SerumV2_32;
        serumUpdate->depth = 24;
        serumUpdate->width = m_pSerum->width32;
        serumUpdate->height = 32;
        memcpy(serumUpdate->segData, m_pSerum->frame32, m_pSerum->width32 * 32 * sizeof(uint16_t));

        QueueUpdate(serumUpdate, false);
      }
    }
    else if (m_pSerum->width32 == 0 && m_pSerum->width64 > 0)
    {
      if (render64)
      {
        serumUpdate->mode = Mode::SerumV2_64;
        serumUpdate->depth = 24;
        serumUpdate->width = m_pSerum->width64;
        serumUpdate->height = 64;
        memcpy(serumUpdate->segData, m_pSerum->frame64, m_pSerum->width64 * 64 * sizeof(uint16_t));

        QueueUpdate(serumUpdate, false);
      }
    }
    else if (m_pSerum->width32 > 0 && m_pSerum->width64 > 0)
    {
      if (render32)
      {
        serumUpdate->mode = Mode::SerumV2_32_64;
        serumUpdate->depth = 24;
        serumUpdate->width = m_pSerum->width32;
        serumUpdate->height = 32;
        memcpy(serumUpdate->segData, m_pSerum->frame32, m_pSerum->width32 * 32 * sizeof(uint16_t));

        QueueUpdate(serumUpdate, false);
      }

      if (render64)
      {
        // We can't reuse the shared pointer from above.
        auto serumUpdateHD = std::make_shared<Update>();
        serumUpdateHD->hasData = true;
        serumUpdateHD->hasSegData = false;
        serumUpdateHD->hasSegData2 = false;
        serumUpdateHD->mode = Mode::SerumV2_64_32;
        serumUpdateHD->width = m_pSerum->width64;
        serumUpdateHD->height = 64;
        memcpy(serumUpdateHD->segData, m_pSerum->frame64, m_pSerum->width64 * 64 * sizeof(uint16_t));

        QueueUpdate(serumUpdateHD, false);
      }
    }
  }
}

#if !(                                                                                                                \
    (defined(__APPLE__) && ((defined(TARGET_OS_IOS) && TARGET_OS_IOS) || (defined(TARGET_OS_TV) && TARGET_OS_TV))) || \
    defined(__ANDROID__))

void DMD::PixelcadeDMDThread()
{
  uint8_t bufferPosition = 0;
  uint16_t segData1[128] = {0};
  uint16_t segData2[128] = {0};
  uint8_t palette[PALETTE_SIZE] = {0};
  uint16_t rgb565Data[128 * 32] = {0};

  m_dmdFrameReady.load(std::memory_order_acquire);
  m_stopFlag.load(std::memory_order_acquire);

  while (true)
  {
    std::shared_lock<std::shared_mutex> sl(m_dmdSharedMutex);
    m_dmdCV.wait(
        sl, [&]()
        { return m_dmdFrameReady.load(std::memory_order_relaxed) || m_stopFlag.load(std::memory_order_relaxed); });
    sl.unlock();
    if (m_stopFlag.load(std::memory_order_acquire))
    {
      return;
    }

    const uint8_t updateBufferQueuePosition = m_updateBufferQueuePosition.load(std::memory_order_acquire);
    while (!m_stopFlag.load(std::memory_order_relaxed) && bufferPosition != updateBufferQueuePosition)
    {
      bufferPosition = GetNextBufferQueuePosition(bufferPosition, updateBufferQueuePosition);

      if (m_pSerum && !IsSerumMode(m_pUpdateBufferQueue[bufferPosition]->mode)) continue;

      if (m_pUpdateBufferQueue[bufferPosition]->hasData || m_pUpdateBufferQueue[bufferPosition]->hasSegData)
      {
        uint16_t width = m_pUpdateBufferQueue[bufferPosition]->width;
        uint16_t height = m_pUpdateBufferQueue[bufferPosition]->height;
        int length = width * height;

        bool update = false;
        if (m_pUpdateBufferQueue[bufferPosition]->depth != 24)
        {
          update = UpdatePalette(palette, m_pUpdateBufferQueue[bufferPosition]->depth,
                                 m_pUpdateBufferQueue[bufferPosition]->r, m_pUpdateBufferQueue[bufferPosition]->g,
                                 m_pUpdateBufferQueue[bufferPosition]->b);
        }

        if (m_pUpdateBufferQueue[bufferPosition]->mode == Mode::RGB24)
        {
          uint8_t rgb24Data[256 * 64 * 3];
          AdjustRGB24Depth(m_pUpdateBufferQueue[bufferPosition]->data, rgb24Data, length, palette,
                           m_pUpdateBufferQueue[bufferPosition]->depth);

          uint8_t scaledBuffer[128 * 32 * 3];
          if (width == 128 && height == 32)
            memcpy(scaledBuffer, rgb24Data, 128 * 32 * 3);
          else if (width == 128 && height == 16)
            FrameUtil::Helper::Center(scaledBuffer, 128, 32, rgb24Data, 128, 16, 24);
          else if (height == 64)
            FrameUtil::Helper::ScaleDown(scaledBuffer, 128, 32, rgb24Data, width, 64, 24);
          else
            continue;

          for (int i = 0; i < 128 * 32; i++)
          {
            int pos = i * 3;
            uint32_t r = scaledBuffer[pos];
            uint32_t g = scaledBuffer[pos + 1];
            uint32_t b = scaledBuffer[pos + 2];

            rgb565Data[i] = (uint16_t)(((r & 0xF8u) << 8) | ((g & 0xFCu) << 3) | (b >> 3));
          }
          update = true;
        }
        else if (m_pUpdateBufferQueue[bufferPosition]->mode == Mode::RGB16)
        {
          if (width == 128 && height == 32)
            memcpy(rgb565Data, m_pUpdateBufferQueue[bufferPosition]->segData, 128 * 32 * 2);
          else if (width == 128 && height == 16)
            FrameUtil::Helper::Center((uint8_t*)rgb565Data, 128, 32,
                                      (uint8_t*)m_pUpdateBufferQueue[bufferPosition]->segData, 128, 16, 16);
          else if (height == 64)
            FrameUtil::Helper::ScaleDown((uint8_t*)rgb565Data, 128, 32,
                                         (uint8_t*)m_pUpdateBufferQueue[bufferPosition]->segData, width, 64, 16);
          else
            continue;

          update = true;
        }
        else if (IsSerumV2Mode(m_pUpdateBufferQueue[bufferPosition]->mode))
        {
          if (m_pUpdateBufferQueue[bufferPosition]->mode == Mode::SerumV2_32 ||
              m_pUpdateBufferQueue[bufferPosition]->mode == Mode::SerumV2_32_64)
            memcpy(rgb565Data, m_pUpdateBufferQueue[bufferPosition]->segData, 128 * 32 * 2);
          else if (m_pUpdateBufferQueue[bufferPosition]->mode == Mode::SerumV2_64)
            FrameUtil::Helper::ScaleDown((uint8_t*)rgb565Data, 128, 32,
                                         (uint8_t*)m_pUpdateBufferQueue[bufferPosition]->segData, width, 64, 16);
          else
            continue;

          update = true;
        }
        else
        {
          uint8_t renderBuffer[256 * 64];

          if (m_pUpdateBufferQueue[bufferPosition]->mode == Mode::SerumV1)
          {
            memcpy(palette, m_pUpdateBufferQueue[bufferPosition]->segData, PALETTE_SIZE);
            memcpy(renderBuffer, m_pUpdateBufferQueue[bufferPosition]->data, length);
            update = true;
          }
          else if (!m_pSerum && m_pUpdateBufferQueue[bufferPosition]->mode == Mode::Data)
          {
            memcpy(renderBuffer, m_pUpdateBufferQueue[bufferPosition]->data, length);
            update = true;
          }
          else if (m_pUpdateBufferQueue[bufferPosition]->mode == Mode::AlphaNumeric)
          {
            if (memcmp(segData1, m_pUpdateBufferQueue[bufferPosition]->segData, sizeof(segData1)) != 0)
            {
              memcpy(segData1, m_pUpdateBufferQueue[bufferPosition]->segData, sizeof(segData1));
              update = true;
            }

            if (m_pUpdateBufferQueue[bufferPosition]->hasSegData2 &&
                memcmp(segData2, m_pUpdateBufferQueue[bufferPosition]->segData2, sizeof(segData2)) != 0)
            {
              memcpy(segData2, m_pUpdateBufferQueue[bufferPosition]->segData2, sizeof(segData2));
              update = true;
            }

            if (update)
            {
              if (m_pUpdateBufferQueue[bufferPosition]->hasSegData2)
                m_pAlphaNumeric->Render(renderBuffer, m_pUpdateBufferQueue[bufferPosition]->layout, segData1, segData2);
              else
                m_pAlphaNumeric->Render(renderBuffer, m_pUpdateBufferQueue[bufferPosition]->layout, segData1);
            }
          }

          if (update)
          {
            uint8_t scaledBuffer[128 * 32];
            if (width == 128 && height == 32)
              memcpy(scaledBuffer, renderBuffer, 128 * 32);
            else if (width == 128 && height == 16)
              FrameUtil::Helper::CenterIndexed(scaledBuffer, 128, 32, renderBuffer, 128, 16);
            else if (width == 192 && height == 64)
              FrameUtil::Helper::ScaleDownIndexed(scaledBuffer, 128, 32, renderBuffer, 192, 64);
            else if (width == 256 && height == 64)
              FrameUtil::Helper::ScaleDownIndexed(scaledBuffer, 128, 32, renderBuffer, 256, 64);
            else
              continue;

            for (int i = 0; i < 128 * 32; i++)
            {
              int pos = scaledBuffer[i] * 3;
              uint32_t r = palette[pos];
              uint32_t g = palette[pos + 1];
              uint32_t b = palette[pos + 2];

              rgb565Data[i] = (uint16_t)(((r & 0xF8u) << 8) | ((g & 0xFCu) << 3) | (b >> 3));
            }
          }
        }

        if (update) m_pPixelcadeDMD->Update(rgb565Data);
      }
    }
  }
}
#endif

void DMD::LevelDMDThread()
{
  uint8_t bufferPosition = 0;
  uint8_t renderBuffer[256 * 64] = {0};

  m_dmdFrameReady.load(std::memory_order_acquire);
  m_stopFlag.load(std::memory_order_acquire);

  while (true)
  {
    std::shared_lock<std::shared_mutex> sl(m_dmdSharedMutex);
    m_dmdCV.wait(
        sl, [&]()
        { return m_dmdFrameReady.load(std::memory_order_relaxed) || m_stopFlag.load(std::memory_order_relaxed); });
    sl.unlock();
    if (m_stopFlag.load(std::memory_order_acquire))
    {
      return;
    }

    const uint8_t updateBufferQueuePosition = m_updateBufferQueuePosition.load(std::memory_order_acquire);
    while (!m_stopFlag.load(std::memory_order_relaxed) && bufferPosition != updateBufferQueuePosition)
    {
      bufferPosition = GetNextBufferQueuePosition(bufferPosition, updateBufferQueuePosition);

      if (!m_levelDMDs.empty() && m_pUpdateBufferQueue[bufferPosition]->mode == Mode::Data &&
          m_pUpdateBufferQueue[bufferPosition]->hasData)
      {
        int length = m_pUpdateBufferQueue[bufferPosition]->width * m_pUpdateBufferQueue[bufferPosition]->height;
        if (memcmp(renderBuffer, m_pUpdateBufferQueue[bufferPosition]->data, length) != 0)
        {
          memcpy(renderBuffer, m_pUpdateBufferQueue[bufferPosition]->data, length);
          for (LevelDMD* pLevelDMD : m_levelDMDs)
          {
            if (pLevelDMD->GetLength() == length)
              pLevelDMD->Update(renderBuffer, m_pUpdateBufferQueue[bufferPosition]->depth);
          }
        }
      }
    }
  }
}

void DMD::RGB24DMDThread()
{
  uint8_t bufferPosition = 0;
  uint16_t segData1[128] = {0};
  uint16_t segData2[128] = {0};
  uint8_t palette[PALETTE_SIZE] = {0};
  uint8_t renderBuffer[256 * 64] = {0};
  uint8_t rgb24Data[256 * 64 * 3] = {0};

  m_dmdFrameReady.load(std::memory_order_acquire);
  m_stopFlag.load(std::memory_order_acquire);

  while (true)
  {
    std::shared_lock<std::shared_mutex> sl(m_dmdSharedMutex);
    m_dmdCV.wait(
        sl, [&]()
        { return m_dmdFrameReady.load(std::memory_order_relaxed) || m_stopFlag.load(std::memory_order_relaxed); });
    sl.unlock();
    if (m_stopFlag.load(std::memory_order_acquire))
    {
      return;
    }

    const uint8_t updateBufferQueuePosition = m_updateBufferQueuePosition.load(std::memory_order_acquire);
    while (!m_stopFlag.load(std::memory_order_relaxed) && bufferPosition != updateBufferQueuePosition)
    {
      bufferPosition = GetNextBufferQueuePosition(bufferPosition, updateBufferQueuePosition);

      if (m_pSerum && !IsSerumMode(m_pUpdateBufferQueue[bufferPosition]->mode)) continue;

      if (!m_rgb24DMDs.empty() &&
          (m_pUpdateBufferQueue[bufferPosition]->hasData || m_pUpdateBufferQueue[bufferPosition]->hasSegData))
      {
        int length = m_pUpdateBufferQueue[bufferPosition]->width * m_pUpdateBufferQueue[bufferPosition]->height;
        bool update = false;

        if (m_pUpdateBufferQueue[bufferPosition]->mode == Mode::RGB24)
        {
          if (memcmp(rgb24Data, m_pUpdateBufferQueue[bufferPosition]->data, length * 3) != 0)
          {
            if (m_pUpdateBufferQueue[bufferPosition]->depth != 24)
            {
              UpdatePalette(palette, m_pUpdateBufferQueue[bufferPosition]->depth,
                            m_pUpdateBufferQueue[bufferPosition]->r, m_pUpdateBufferQueue[bufferPosition]->g,
                            m_pUpdateBufferQueue[bufferPosition]->b);
            }

            AdjustRGB24Depth(m_pUpdateBufferQueue[bufferPosition]->data, rgb24Data, length, palette,
                             m_pUpdateBufferQueue[bufferPosition]->depth);

            for (RGB24DMD* pRGB24DMD : m_rgb24DMDs)
            {
              if (pRGB24DMD->GetLength() == length * 3) pRGB24DMD->Update(rgb24Data);
            }
            // Reset renderBuffer in case the mode changes for the next frame to ensure that memcmp() will detect it.
            memset(renderBuffer, 0, sizeof(renderBuffer));
          }
        }
        else if (m_pUpdateBufferQueue[bufferPosition]->mode != Mode::RGB16 &&
                 !IsSerumV2Mode(m_pUpdateBufferQueue[bufferPosition]->mode))
        {
          if (m_pUpdateBufferQueue[bufferPosition]->mode == Mode::SerumV1)
          {
            memcpy(palette, m_pUpdateBufferQueue[bufferPosition]->segData, PALETTE_SIZE);
            memcpy(renderBuffer, m_pUpdateBufferQueue[bufferPosition]->data, length);
            update = true;
          }
          else
          {
            update = UpdatePalette(palette, m_pUpdateBufferQueue[bufferPosition]->depth,
                                   m_pUpdateBufferQueue[bufferPosition]->r, m_pUpdateBufferQueue[bufferPosition]->g,
                                   m_pUpdateBufferQueue[bufferPosition]->b);

            if (!m_pSerum && m_pUpdateBufferQueue[bufferPosition]->mode == Mode::Data)
            {
              if (memcmp(renderBuffer, m_pUpdateBufferQueue[bufferPosition]->data, length) != 0)
              {
                memcpy(renderBuffer, m_pUpdateBufferQueue[bufferPosition]->data, length);
                update = true;
              }
            }
            else if (m_pUpdateBufferQueue[bufferPosition]->mode == Mode::AlphaNumeric)
            {
              if (memcmp(segData1, m_pUpdateBufferQueue[bufferPosition]->segData, sizeof(segData1)) != 0)
              {
                memcpy(segData1, m_pUpdateBufferQueue[bufferPosition]->segData, sizeof(segData1));
                update = true;
              }

              if (m_pUpdateBufferQueue[bufferPosition]->hasSegData2 &&
                  memcmp(segData2, m_pUpdateBufferQueue[bufferPosition]->segData2, sizeof(segData2)) != 0)
              {
                memcpy(segData2, m_pUpdateBufferQueue[bufferPosition]->segData2, sizeof(segData2));
                update = true;
              }

              if (update)
              {
                if (m_pUpdateBufferQueue[bufferPosition]->hasSegData2)
                  m_pAlphaNumeric->Render(renderBuffer, m_pUpdateBufferQueue[bufferPosition]->layout, segData1,
                                          segData2);
                else
                  m_pAlphaNumeric->Render(renderBuffer, m_pUpdateBufferQueue[bufferPosition]->layout, segData1);
              }
            }
          }

          if (update)
          {
            for (int i = 0; i < length; i++)
            {
              int palettePos = renderBuffer[i] * 3;
              int pos = i * 3;
              rgb24Data[pos] = palette[palettePos];
              rgb24Data[pos + 1] = palette[palettePos + 1];
              rgb24Data[pos + 2] = palette[palettePos + 2];
            }

            for (RGB24DMD* pRGB24DMD : m_rgb24DMDs)
            {
              if (pRGB24DMD->GetLength() == length * 3) pRGB24DMD->Update(rgb24Data);
            }
          }
        }
        else
        {
          for (int i = 0; i < length; i++)
          {
            int pos = i * 3;
            rgb24Data[pos] = ((m_pUpdateBufferQueue[bufferPosition]->segData[i] >> 8) & 0xF8) |
                             ((m_pUpdateBufferQueue[bufferPosition]->segData[i] >> 13) & 0x07);
            rgb24Data[pos + 1] = ((m_pUpdateBufferQueue[bufferPosition]->segData[i] >> 3) & 0xFC) |
                                 ((m_pUpdateBufferQueue[bufferPosition]->segData[i] >> 9) & 0x03);
            rgb24Data[pos + 2] = ((m_pUpdateBufferQueue[bufferPosition]->segData[i] << 3) & 0xF8) |
                                 ((m_pUpdateBufferQueue[bufferPosition]->segData[i] >> 2) & 0x07);
          }

          for (RGB24DMD* pRGB24DMD : m_rgb24DMDs)
          {
            if (pRGB24DMD->GetLength() == length * 3) pRGB24DMD->Update(rgb24Data);
          }
        }
      }
    }
  }
}

void DMD::ConsoleDMDThread()
{
  uint8_t bufferPosition = 0;
  uint8_t renderBuffer[256 * 64] = {0};

  m_dmdFrameReady.load(std::memory_order_acquire);
  m_stopFlag.load(std::memory_order_acquire);

  while (true)
  {
    std::shared_lock<std::shared_mutex> sl(m_dmdSharedMutex);
    m_dmdCV.wait(
        sl, [&]()
        { return m_dmdFrameReady.load(std::memory_order_relaxed) || m_stopFlag.load(std::memory_order_relaxed); });
    sl.unlock();
    if (m_stopFlag.load(std::memory_order_acquire))
    {
      return;
    }

    const uint8_t updateBufferQueuePosition = m_updateBufferQueuePosition.load(std::memory_order_acquire);
    while (!m_stopFlag.load(std::memory_order_relaxed) && bufferPosition != updateBufferQueuePosition)
    {
      bufferPosition = GetNextBufferQueuePosition(bufferPosition, updateBufferQueuePosition);

      if (!m_consoleDMDs.empty() && m_pUpdateBufferQueue[bufferPosition]->mode == Mode::Data &&
          m_pUpdateBufferQueue[bufferPosition]->hasData)
      {
        int length = m_pUpdateBufferQueue[bufferPosition]->width * m_pUpdateBufferQueue[bufferPosition]->height;
        if (memcmp(renderBuffer, m_pUpdateBufferQueue[bufferPosition]->data, length) != 0)
        {
          memcpy(renderBuffer, m_pUpdateBufferQueue[bufferPosition]->data, length);
          for (ConsoleDMD* pConsoleDMD : m_consoleDMDs)
          {
            pConsoleDMD->Render(renderBuffer, m_pUpdateBufferQueue[bufferPosition]->width,
                                m_pUpdateBufferQueue[bufferPosition]->height,
                                m_pUpdateBufferQueue[bufferPosition]->depth);
          }
        }
      }
    }
  }
}

bool DMD::UpdatePalette(uint8_t* pPalette, uint8_t depth, uint8_t r, uint8_t g, uint8_t b)
{
  if (depth != 2 && depth != 4) return false;
  uint8_t palette[192];

  const uint8_t colors = (depth == 2) ? 4 : 16;
  memcpy(palette, pPalette, colors * 3);
  uint8_t pos = 0;

  for (uint8_t i = 0; i < colors; i++)
  {
    float perc = FrameUtil::Helper::CalcBrightness((float)i / (float)(colors - 1));
    pPalette[pos++] = (uint8_t)((float)r * perc);
    pPalette[pos++] = (uint8_t)((float)g * perc);
    pPalette[pos++] = (uint8_t)((float)b * perc);
  }

  return (memcmp(pPalette, palette, colors * 3) != 0);
}

void DMD::AdjustRGB24Depth(uint8_t* pData, uint8_t* pDstData, int length, uint8_t* palette, uint8_t depth)
{
  if (depth != 24)
  {
    for (int i = 0; i < length; i++)
    {
      int pos = i * 3;
      uint32_t r = pData[pos];
      uint32_t g = pData[pos + 1];
      uint32_t b = pData[pos + 2];

      int v = (int)(0.2126f * (float)r + 0.7152f * (float)g + 0.0722f * (float)b);
      if (v > 255) v = 255;

      uint8_t level;
      if (depth == 2)
        level = (uint8_t)(v >> 6);
      else
        level = (uint8_t)(v >> 4);

      int pos2 = level * 3;
      pDstData[pos] = palette[pos2];
      pDstData[pos + 1] = palette[pos2 + 1];
      pDstData[pos + 2] = palette[pos2 + 2];
    }
  }
  else
  {
    memcpy(pDstData, pData, length * 3);
  }
}

void DMD::DumpDMDTxtThread()
{
  char name[DMDUTIL_MAX_NAME_SIZE] = {0};
  uint8_t bufferPosition = 0;
  uint8_t renderBuffer[3][256 * 64] = {0};
  uint32_t passed[3] = {0};
  std::chrono::steady_clock::time_point start;
  FILE* f = nullptr;

  m_dmdFrameReady.load(std::memory_order_acquire);
  m_stopFlag.load(std::memory_order_acquire);

  while (true)
  {
    std::shared_lock<std::shared_mutex> sl(m_dmdSharedMutex);
    m_dmdCV.wait(
        sl, [&]()
        { return m_dmdFrameReady.load(std::memory_order_relaxed) || m_stopFlag.load(std::memory_order_relaxed); });
    sl.unlock();
    if (m_stopFlag.load(std::memory_order_acquire))
    {
      if (f)
      {
        fclose(f);
        f = nullptr;
      }
      return;
    }

    const uint8_t updateBufferQueuePosition = m_updateBufferQueuePosition.load(std::memory_order_acquire);
    while (!m_stopFlag.load(std::memory_order_relaxed) && bufferPosition != updateBufferQueuePosition)
    {
      // Don't use GetNextBufferPosition() here, we need all frames!
      if (++bufferPosition >= DMDUTIL_FRAME_BUFFER_SIZE) bufferPosition = 0;

      if (m_pUpdateBufferQueue[bufferPosition]->depth <= 4 &&
          m_pUpdateBufferQueue[bufferPosition]->mode == Mode::Data && m_pUpdateBufferQueue[bufferPosition]->hasData)
      {
        bool update = false;
        if (strcmp(m_romName, name) != 0)
        {
          // New game ROM.
          start = std::chrono::steady_clock::now();
          if (f)
          {
            fclose(f);
            f = nullptr;
          }
          strcpy(name, m_romName);

          if (name[0] != '\0')
          {
            char filename[128];
            snprintf(filename, DMDUTIL_MAX_NAME_SIZE + 5, "%s.txt", name);
            f = fopen(filename, "a");
            update = true;
            memset(renderBuffer, 0, 2 * 256 * 64);
            passed[0] = passed[1] = 0;
          }
        }

        if (name[0] != '\0')
        {
          int length = m_pUpdateBufferQueue[bufferPosition]->width * m_pUpdateBufferQueue[bufferPosition]->height;
          if (update || (memcmp(renderBuffer[1], m_pUpdateBufferQueue[bufferPosition]->data, length) != 0))
          {
            passed[2] = (uint32_t)(std::chrono::duration_cast<std::chrono::milliseconds>(
                                       std::chrono::steady_clock::now() - start)
                                       .count());
            memcpy(renderBuffer[2], m_pUpdateBufferQueue[bufferPosition]->data, length);

            if (m_pUpdateBufferQueue[bufferPosition]->depth == 2 &&
                (passed[2] - passed[1]) < DMDUTIL_MAX_TRANSITIONAL_FRAME_DURATION)
            {
              int i = 0;
              while (i < length &&
                     ((renderBuffer[0][i] == 2) ||
                      ((renderBuffer[0][i] == 3) || (renderBuffer[2][i] > 1)) == (renderBuffer[1][i] > 0)))
              {
                i++;
              }
              if (i == length)
              {
                // renderBuffer[1] is a transitional frame, delete it.
                memcpy(renderBuffer[1], renderBuffer[2], length);
                passed[1] += passed[2];
                continue;
              }
            }

            if (f)
            {
              fprintf(f, "0x%08x\n", passed[0]);
              for (int y = 0; y < m_pUpdateBufferQueue[bufferPosition]->height; y++)
              {
                for (int x = 0; x < m_pUpdateBufferQueue[bufferPosition]->width; x++)
                {
                  fprintf(f, "%x", renderBuffer[0][y * m_pUpdateBufferQueue[bufferPosition]->width + x]);
                }
                fprintf(f, "\n");
              }
              fprintf(f, "\n");
            }
            memcpy(renderBuffer[0], renderBuffer[1], length);
            passed[0] = passed[1];
            memcpy(renderBuffer[1], renderBuffer[2], length);
            passed[1] = passed[2];
          }
        }
      }
    }
  }
}

void DMD::DumpDMDRawThread()
{
  char name[DMDUTIL_MAX_NAME_SIZE] = {0};
  uint8_t bufferPosition = 0;
  std::chrono::steady_clock::time_point start;
  FILE* f = nullptr;

  m_dmdFrameReady.load(std::memory_order_acquire);
  m_stopFlag.load(std::memory_order_acquire);

  while (true)
  {
    std::shared_lock<std::shared_mutex> sl(m_dmdSharedMutex);
    m_dmdCV.wait(
        sl, [&]()
        { return m_dmdFrameReady.load(std::memory_order_relaxed) || m_stopFlag.load(std::memory_order_relaxed); });
    sl.unlock();
    if (m_stopFlag.load(std::memory_order_acquire))
    {
      if (f)
      {
        fclose(f);
        f = nullptr;
      }
      return;
    }

    const uint8_t updateBufferQueuePosition = m_updateBufferQueuePosition.load(std::memory_order_acquire);
    while (!m_stopFlag.load(std::memory_order_relaxed) && bufferPosition != updateBufferQueuePosition)
    {
      // Don't use GetNextBufferPosition() here, we need all frames!
      if (++bufferPosition >= DMDUTIL_FRAME_BUFFER_SIZE) bufferPosition = 0;

      if (m_pUpdateBufferQueue[bufferPosition]->hasData || m_pUpdateBufferQueue[bufferPosition]->hasSegData)
      {
        if (strcmp(m_romName, name) != 0)
        {
          // New game ROM.
          start = std::chrono::steady_clock::now();
          if (f)
          {
            fclose(f);
            f = nullptr;
          }
          strcpy(name, m_romName);

          if (name[0] != '\0')
          {
            char filename[128];
            snprintf(filename, DMDUTIL_MAX_NAME_SIZE + 5, "%s.raw", name);
            f = fopen(filename, "ab");
          }
        }

        if (name[0] != '\0')
        {
          if (f)
          {
            auto current =
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
            fwrite(&current, 1, 4, f);

            uint32_t size = sizeof(m_pUpdateBufferQueue[bufferPosition]);
            fwrite(&size, 1, 4, f);

            fwrite(m_pUpdateBufferQueue[bufferPosition], 1, size, f);
          }
        }
      }
    }
  }
}

void DMD::PupDMDThread()
{
  uint8_t bufferPosition = 0;
  uint8_t renderBuffer[256 * 64] = {0};
  uint8_t palette[192] = {0};
  char name[DMDUTIL_MAX_NAME_SIZE] = {0};

  m_dmdFrameReady.load(std::memory_order_acquire);
  m_stopFlag.load(std::memory_order_acquire);

  while (true)
  {
    std::shared_lock<std::shared_mutex> sl(m_dmdSharedMutex);
    m_dmdCV.wait(
        sl, [&]()
        { return m_dmdFrameReady.load(std::memory_order_relaxed) || m_stopFlag.load(std::memory_order_relaxed); });
    sl.unlock();
    if (m_stopFlag.load(std::memory_order_acquire))
    {
      return;
    }

    const uint8_t updateBufferQueuePosition = m_updateBufferQueuePosition.load(std::memory_order_acquire);
    while (!m_stopFlag.load(std::memory_order_relaxed) && bufferPosition != updateBufferQueuePosition)
    {
      // Don't use GetNextBufferPosition() here, we need all frames!
      if (++bufferPosition >= DMDUTIL_FRAME_BUFFER_SIZE) bufferPosition = 0;

      if (strcmp(m_romName, name) != 0)
      {
        strcpy(name, m_romName);

        if (Config::GetInstance()->IsPUPCapture())
        {
          if (m_pPUPDMD)
          {
            delete (m_pPUPDMD);
            m_pPUPDMD = nullptr;
          }

          if (name[0] != '\0')
          {
            if (m_pupVideosPath[0] == '\0') strcpy(m_pupVideosPath, Config::GetInstance()->GetPUPVideosPath());
            m_pPUPDMD = new PUPDMD::DMD();
            m_pPUPDMD->SetLogCallback(PUPDMDLogCallback, nullptr);

            if (!m_pPUPDMD->Load(m_pupVideosPath, m_romName, m_pUpdateBufferQueue[bufferPosition]->depth))
            {
              delete (m_pPUPDMD);
              m_pPUPDMD = nullptr;
            }
          }
        }
      }

      if (m_pPUPDMD && m_pUpdateBufferQueue[bufferPosition]->hasData &&
          m_pUpdateBufferQueue[bufferPosition]->mode == Mode::Data && m_pUpdateBufferQueue[bufferPosition]->depth != 24)
      {
        uint16_t width = m_pUpdateBufferQueue[bufferPosition]->width;
        uint16_t height = m_pUpdateBufferQueue[bufferPosition]->height;
        int length = width * height;

        if (memcmp(renderBuffer, m_pUpdateBufferQueue[bufferPosition]->data, length) != 0)
        {
          memcpy(renderBuffer, m_pUpdateBufferQueue[bufferPosition]->data, length);
          uint8_t depth = m_pUpdateBufferQueue[bufferPosition]->depth;

          uint8_t scaledBuffer[128 * 32];
          if (width == 128 && height == 32)
            memcpy(scaledBuffer, renderBuffer, 128 * 32);
          else if (width == 128 && height == 16)
            FrameUtil::Helper::CenterIndexed(scaledBuffer, 128, 32, renderBuffer, 128, 16);
          else if (width == 192 && height == 64)
            FrameUtil::Helper::ScaleDownPUP(scaledBuffer, 128, 32, renderBuffer, 192, 64);
          else
            return;

          uint16_t triggerID = 0;
          if (Config::GetInstance()->IsPUPExactColorMatch())
          {
            triggerID = m_pPUPDMD->MatchIndexed(scaledBuffer, width, height);
          }
          else
          {
            // apply a standard orange palette
            UpdatePalette(palette, depth, 255, 69, 0);

            uint8_t* pFrame = (uint8_t*)malloc(length * 3);
            for (uint16_t i = 0; i < length; i++)
            {
              uint16_t pos = scaledBuffer[i] * 3;
              memcpy(&pFrame[i * 3], &palette[pos], 3);
            }
            triggerID = m_pPUPDMD->Match(pFrame, width, height, false);
            free(pFrame);
          }

          if (triggerID > 0) HandleTrigger(triggerID);
        }
      }
    }
  }
}

void DMD::HandleTrigger(uint16_t id)
{
  static Config* pConfig = Config::GetInstance();

  Log(DMDUtil_LogLevel_DEBUG, "HandleTrigger: id=D%d", id);

  DMDUtil_PUPTriggerCallbackContext callbackContext = pConfig->GetPUPTriggerCallbackContext();
  if (callbackContext.callback != nullptr)
  {
    (*callbackContext.callback)(id, callbackContext.pUserData);
  }
}

}  // namespace DMDUtil
