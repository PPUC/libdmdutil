#include "DMDUtil/DMD.h"

#include "DMDUtil/Config.h"
#include "DMDUtil/LevelDMD.h"
#include "DMDUtil/RGB24DMD.h"

#if !(                                                                                                                \
    (defined(__APPLE__) && ((defined(TARGET_OS_IOS) && TARGET_OS_IOS) || (defined(TARGET_OS_TV) && TARGET_OS_TV))) || \
    defined(__ANDROID__))
#include "PixelcadeDMD.h"
#endif

#include <algorithm>
#include <cstring>

#include "AlphaNumeric.h"
#include "FrameUtil.h"
#include "Logger.h"
#include "Serum.h"
#include "ZeDMD.h"

namespace DMDUtil
{

void ZEDMDCALLBACK ZeDMDLogCallback(const char* format, va_list args, const void* pUserData)
{
  char buffer[1024];
  vsnprintf(buffer, sizeof(buffer), format, args);

  Log("%s", buffer);
}

bool DMD::m_finding = false;

DMD::DMD()
{
  for (uint8_t i = 0; i < DMD_FRAME_BUFFER_SIZE; i++)
  {
    m_updateBuffer[i] = new DMDUpdate();
  }
  m_pAlphaNumeric = new AlphaNumeric();
  m_pSerum = nullptr;
  m_pZeDMD = nullptr;
  m_pZeDMDThread = nullptr;
#if !(                                                                                                                \
    (defined(__APPLE__) && ((defined(TARGET_OS_IOS) && TARGET_OS_IOS) || (defined(TARGET_OS_TV) && TARGET_OS_TV))) || \
    defined(__ANDROID__))
  m_pPixelcadeDMD = nullptr;
  m_pPixelcadeDMDThread = nullptr;
#endif

  FindDevices();

  // todo virtual dmdm thread

  m_pdmdFrameReadyResetThread = new std::thread(&DMD::DmdFrameReadyResetThread, this);
}

DMD::~DMD()
{
  std::unique_lock<std::shared_mutex> ul(m_dmdSharedMutex);
  m_stopFlag = true;
  ul.unlock();
  m_dmdCV.notify_all();

  m_pdmdFrameReadyResetThread->join();
  delete m_pdmdFrameReadyResetThread;
  m_pdmdFrameReadyResetThread = nullptr;

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

  if (m_pZeDMDThread)
  {
    m_pZeDMDThread->join();
    delete m_pZeDMDThread;
    m_pZeDMDThread = nullptr;
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
  delete m_pSerum;
  delete m_pZeDMD;
#if !(                                                                                                                \
    (defined(__APPLE__) && ((defined(TARGET_OS_IOS) && TARGET_OS_IOS) || (defined(TARGET_OS_TV) && TARGET_OS_TV))) || \
    defined(__ANDROID__))
  delete m_pPixelcadeDMD;
#endif

  for (LevelDMD* pLevelDMD : m_levelDMDs) delete pLevelDMD;
  for (RGB24DMD* pRGB24DMD : m_rgb24DMDs) delete pRGB24DMD;
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

void DMD::UpdateData(const uint8_t* pData, int depth, uint16_t width, uint16_t height, uint8_t r, uint8_t g, uint8_t b,
                     DMDMode mode, const char* name)
{
  std::unique_lock<std::shared_mutex> ul(m_dmdSharedMutex);
  m_updateBuffer[m_updateBufferPosition]->mode = mode;
  m_updateBuffer[m_updateBufferPosition]->depth = depth;
  // @todo width, height and length have to be dynamic.
  m_updateBuffer[m_updateBufferPosition]->width = width;
  m_updateBuffer[m_updateBufferPosition]->height = height;
  if (m_updateBuffer[m_updateBufferPosition]->pData != nullptr)
  {
    free(m_updateBuffer[m_updateBufferPosition]->pData);
    m_updateBuffer[m_updateBufferPosition]->pData = nullptr;
  }
  if (m_updateBuffer[m_updateBufferPosition]->pData2 != nullptr)
  {
    free(m_updateBuffer[m_updateBufferPosition]->pData2);
    m_updateBuffer[m_updateBufferPosition]->pData2 = nullptr;
  }
  if (pData)
  {
    m_updateBuffer[m_updateBufferPosition]->pData = malloc(width * height * (mode == DMDMode::RGB24 ? 3 : 1));
    memcpy(m_updateBuffer[m_updateBufferPosition]->pData, pData, width * height * (mode == DMDMode::RGB24 ? 3 : 1));
  }
  m_updateBuffer[m_updateBufferPosition]->r = r;
  m_updateBuffer[m_updateBufferPosition]->g = g;
  m_updateBuffer[m_updateBufferPosition]->b = b;
  m_updateBuffer[m_updateBufferPosition]->name = name;

  m_dmdFrameReady = true;
  if (++m_updateBufferPosition >= DMD_FRAME_BUFFER_SIZE) m_updateBufferPosition = 0;
  ul.unlock();
  m_dmdCV.notify_all();
}

void DMD::UpdateData(const uint8_t* pData, int depth, uint16_t width, uint16_t height, uint8_t r, uint8_t g, uint8_t b,
                     const char* name)
{
  UpdateData(pData, depth, width, height, r, g, b, DMDMode::Data, name);
}

void DMD::UpdateRGB24Data(const uint8_t* pData, int depth, uint16_t width, uint16_t height, uint8_t r, uint8_t g,
                          uint8_t b)
{
  UpdateData(pData, depth, width, height, r, g, b, DMDMode::RGB24, nullptr);
}

void DMD::UpdateRGB24Data(const uint8_t* pData, uint16_t width, uint16_t height, uint8_t r, uint8_t g, uint8_t b)
{
  UpdateData(pData, 24, width, height, r, g, b, DMDMode::RGB24, nullptr);
}

void DMD::UpdateAlphaNumericData(AlphaNumericLayout layout, const uint16_t* pData1, const uint16_t* pData2, uint8_t r,
                                 uint8_t g, uint8_t b, const char* name)
{
  std::unique_lock<std::shared_mutex> ul(m_dmdSharedMutex);
  m_updateBuffer[m_updateBufferPosition]->mode = DMDMode::AlphaNumeric;
  m_updateBuffer[m_updateBufferPosition]->depth = 2;
  m_updateBuffer[m_updateBufferPosition]->width = 128;
  m_updateBuffer[m_updateBufferPosition]->height = 32;
  if (m_updateBuffer[m_updateBufferPosition]->pData != nullptr)
  {
    free(m_updateBuffer[m_updateBufferPosition]->pData);
    m_updateBuffer[m_updateBufferPosition]->pData = nullptr;
  }
  if (m_updateBuffer[m_updateBufferPosition]->pData2 != nullptr)
  {
    free(m_updateBuffer[m_updateBufferPosition]->pData2);
    m_updateBuffer[m_updateBufferPosition]->pData2 = nullptr;
  }
  m_updateBuffer[m_updateBufferPosition]->pData = malloc(128 * sizeof(uint16_t));
  memcpy(m_updateBuffer[m_updateBufferPosition]->pData, pData1, 128 * sizeof(uint16_t));
  if (pData2)
  {
    m_updateBuffer[m_updateBufferPosition]->pData2 = malloc(128 * sizeof(uint16_t));
    memcpy(m_updateBuffer[m_updateBufferPosition]->pData2, pData2, 128 * sizeof(uint16_t));
  }
  m_updateBuffer[m_updateBufferPosition]->r = r;
  m_updateBuffer[m_updateBufferPosition]->g = g;
  m_updateBuffer[m_updateBufferPosition]->b = b;
  m_updateBuffer[m_updateBufferPosition]->name = name;

  m_dmdFrameReady = true;
  if (++m_updateBufferPosition >= DMD_FRAME_BUFFER_SIZE) m_updateBufferPosition = 0;
  ul.unlock();
  m_dmdCV.notify_all();
}

void DMD::FindDevices()
{
  if (m_finding) return;

  m_finding = true;

  new std::thread(
      [this]()
      {
        Config* const pConfig = Config::GetInstance();

        ZeDMD* pZeDMD = nullptr;

        if (pConfig->IsZeDMD())
        {
          pZeDMD = new ZeDMD();
          pZeDMD->SetLogCallback(ZeDMDLogCallback, nullptr);

          if (pConfig->GetZeDMDDevice() != nullptr && pConfig->GetZeDMDDevice()[0] != '\0')
            pZeDMD->SetDevice(pConfig->GetZeDMDDevice());

          if (pZeDMD->Open())
          {
            if (pConfig->IsZeDMDDebug()) pZeDMD->EnableDebug();

            if (pConfig->GetZeDMDRGBOrder() != -1) pZeDMD->SetRGBOrder(pConfig->GetZeDMDRGBOrder());

            if (pConfig->GetZeDMDBrightness() != -1) pZeDMD->SetBrightness(pConfig->GetZeDMDBrightness());

            if (pConfig->IsZeDMDSaveSettings()) pZeDMD->SaveSettings();

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
          m_pPixelcadeDMDThread = new std::thread(&DMD::PixelcadeDMDThread, this);
        }

        m_pPixelcadeDMD = pPixelcadeDMD;
#endif

        m_finding = false;
      });
}

void DMD::DmdFrameReadyResetThread()
{
  const char* name = nullptr;

  while (true)
  {
    std::shared_lock<std::shared_mutex> sl(m_dmdSharedMutex);
    m_dmdCV.wait(sl, [&]() { return m_dmdFrameReady || m_stopFlag; });
    sl.unlock();

    if ((m_updateBuffer[m_updateBufferPosition]->name && !name) ||
        (!m_updateBuffer[m_updateBufferPosition]->name && name) ||
        (m_updateBuffer[m_updateBufferPosition]->name && name &&
         strcmp(m_updateBuffer[m_updateBufferPosition]->name, name) != 0))
    {
      if (m_pSerum)
      {
        delete (m_pSerum);
        m_pSerum = nullptr;
      }

      name = m_updateBuffer[m_updateBufferPosition]->name;
      m_pSerum =
          (Config::GetInstance()->IsAltColor() && name != nullptr && name[0] != '\0') ? Serum::Load(name) : nullptr;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(1));

    std::unique_lock<std::shared_mutex> ul(m_dmdSharedMutex);
    m_dmdFrameReady = false;
    ul.unlock();

    if (m_stopFlag)
    {
      return;
    }
  }
}

void DMD::ZeDMDThread()
{
  int bufferPosition = 0;
  uint16_t width = 0;
  uint16_t height = 0;
  uint16_t segData1[128];
  uint16_t segData2[128];
  uint8_t palette[192] = {0};
  // ZeDMD HD supports 256 * 64 pixels.
  uint8_t renderBuffer[256 * 64] = {0};

  while (true)
  {
    std::shared_lock<std::shared_mutex> sl(m_dmdSharedMutex);
    m_dmdCV.wait(sl, [&]() { return m_dmdFrameReady || m_stopFlag; });
    sl.unlock();
    if (m_stopFlag)
    {
      return;
    }

    while (!m_stopFlag && bufferPosition != m_updateBufferPosition)
    {
      // Note: libzedmd has its own update detection.

      if (m_updateBuffer[bufferPosition]->width != width || m_updateBuffer[bufferPosition]->height != height)
      {
        width = m_updateBuffer[bufferPosition]->width;
        height = m_updateBuffer[bufferPosition]->height;
        // Activate the correct scaling mode.
        m_pZeDMD->SetFrameSize(width, height);
      }

      if (m_updateBuffer[bufferPosition]->mode == DMDMode::RGB24)
      {
        m_pZeDMD->RenderRgb24((uint8_t*)m_updateBuffer[bufferPosition]->pData);
      }
      else
      {
        bool update = UpdatePalette(palette, m_updateBuffer[bufferPosition]->depth, m_updateBuffer[bufferPosition]->r,
                                    m_updateBuffer[bufferPosition]->g, m_updateBuffer[bufferPosition]->b);

        if (m_updateBuffer[bufferPosition]->mode == DMDMode::Data)
        {
          memcpy(renderBuffer, m_updateBuffer[bufferPosition]->pData, width * height);

          if (m_pSerum)
          {
            uint8_t rotations[24] = {0};
            uint32_t triggerID;
            uint32_t hashcode;
            int frameID;

            m_pSerum->SetStandardPalette(palette, m_updateBuffer[bufferPosition]->depth);

            if (m_pSerum->ColorizeWithMetadata(renderBuffer, width, height, palette, rotations, &triggerID, &hashcode,
                                               &frameID))
            {
              m_pZeDMD->RenderColoredGray6(renderBuffer, palette, rotations);

              // @todo: send DMD PUP Event with triggerID
            }
          }
          else
          {
            m_pZeDMD->SetPalette(palette, m_updateBuffer[bufferPosition]->depth == 2 ? 4 : 16);

            switch (m_updateBuffer[bufferPosition]->depth)
            {
              case 2:
                m_pZeDMD->RenderGray2(renderBuffer);
                break;

              case 4:
                m_pZeDMD->RenderGray4(renderBuffer);
                break;

              default:
                //@todo log error
                break;
            }
          }
        }
        else if (m_updateBuffer[bufferPosition]->mode == DMDMode::AlphaNumeric)
        {
          if (memcmp(segData1, m_updateBuffer[bufferPosition]->pData, 128 * sizeof(uint16_t)) != 0)
          {
            memcpy(segData1, m_updateBuffer[bufferPosition]->pData, 128 * sizeof(uint16_t));
            update = true;
          }

          if (m_updateBuffer[bufferPosition]->pData2 &&
              memcmp(segData2, m_updateBuffer[bufferPosition]->pData2, 128 * sizeof(uint16_t)) != 0)
          {
            memcpy(segData2, m_updateBuffer[bufferPosition]->pData2, 128 * sizeof(uint16_t));
            update = true;
          }

          if (update)
          {
            if (m_updateBuffer[bufferPosition]->pData2)
              m_pAlphaNumeric->Render(renderBuffer, m_updateBuffer[bufferPosition]->layout, (const uint16_t*)segData1,
                                      (const uint16_t*)segData2);
            else
              m_pAlphaNumeric->Render(renderBuffer, m_updateBuffer[bufferPosition]->layout, (const uint16_t*)segData1);

            m_pZeDMD->SetPalette(palette, 4);
            m_pZeDMD->RenderGray2(renderBuffer);
          }
        }
      }

      if (++bufferPosition >= DMD_FRAME_BUFFER_SIZE) bufferPosition = 0;
    }
  }
}

#if !(                                                                                                                \
    (defined(__APPLE__) && ((defined(TARGET_OS_IOS) && TARGET_OS_IOS) || (defined(TARGET_OS_TV) && TARGET_OS_TV))) || \
    defined(__ANDROID__))

void DMD::PixelcadeDMDThread()
{
  int bufferPosition = 0;
  uint16_t segData1[128];
  uint16_t segData2[128];
  uint8_t palette[192] = {0};
  uint16_t rgb565Data[128 * 32 * 3] = {0};
  uint8_t renderBuffer[128 * 32] = {0};

  while (true)
  {
    std::shared_lock<std::shared_mutex> sl(m_dmdSharedMutex);
    m_dmdCV.wait(sl, [&]() { return m_dmdFrameReady || m_stopFlag; });
    sl.unlock();
    if (m_stopFlag)
    {
      return;
    }

    while (!m_stopFlag && bufferPosition != m_updateBufferPosition)
    {
      // @todo scaling
      if (m_updateBuffer[bufferPosition]->width == 128 && m_updateBuffer[bufferPosition]->width == 32)
      {
        int length = m_updateBuffer[bufferPosition]->width * m_updateBuffer[bufferPosition]->height;
        bool update = false;

        if (m_updateBuffer[bufferPosition]->mode == DMDMode::RGB24)
        {
          for (int i = 0; i < length; i++)
          {
            int pos = i * 3;
            uint32_t r = ((uint8_t*)(m_updateBuffer[bufferPosition]->pData))[pos];
            uint32_t g = ((uint8_t*)(m_updateBuffer[bufferPosition]->pData))[pos + 1];
            uint32_t b = ((uint8_t*)(m_updateBuffer[bufferPosition]->pData))[pos + 2];

            rgb565Data[i] = (uint16_t)(((r & 0xF8u) << 8) | ((g & 0xFCu) << 3) | (b >> 3));
          }
          update = true;
        }
        else
        {
          update = UpdatePalette(palette, m_updateBuffer[bufferPosition]->depth, m_updateBuffer[bufferPosition]->r,
                                 m_updateBuffer[bufferPosition]->g, m_updateBuffer[bufferPosition]->b);

          if (m_updateBuffer[bufferPosition]->mode == DMDMode::Data)
          {
            // @todo At the momeent libserum only supports on instance. So don't apply colorization if a ZeDMD is
            // attached.
            if (m_pSerum && !m_pZeDMD)
            {
              update = m_pSerum->Convert((uint8_t*)m_updateBuffer[bufferPosition]->pData, renderBuffer, palette,
                                         m_updateBuffer[bufferPosition]->width, m_updateBuffer[bufferPosition]->height);
            }
            else
            {
              memcpy(renderBuffer, (uint8_t*)m_updateBuffer[bufferPosition]->pData,
                     m_updateBuffer[bufferPosition]->width * m_updateBuffer[bufferPosition]->height);
              update = true;
            }
          }
          else if (m_updateBuffer[bufferPosition]->mode == DMDMode::AlphaNumeric)
          {
            if (memcmp(segData1, m_updateBuffer[bufferPosition]->pData, 128 * sizeof(uint16_t)) != 0)
            {
              memcpy(segData1, m_updateBuffer[bufferPosition]->pData, 128 * sizeof(uint16_t));
              update = true;
            }

            if (m_updateBuffer[bufferPosition]->pData2 &&
                memcmp(segData2, m_updateBuffer[bufferPosition]->pData2, 128 * sizeof(uint16_t)) != 0)
            {
              memcpy(segData2, m_updateBuffer[bufferPosition]->pData2, 128 * sizeof(uint16_t));
              update = true;
            }

            if (update)
            {
              uint8_t* pData;

              if (m_updateBuffer[bufferPosition]->pData2)
                m_pAlphaNumeric->Render(renderBuffer, m_updateBuffer[bufferPosition]->layout, (const uint16_t*)segData1,
                                        (const uint16_t*)segData2);
              else
                m_pAlphaNumeric->Render(renderBuffer, m_updateBuffer[bufferPosition]->layout,
                                        (const uint16_t*)segData1);
            }
          }

          if (update)
          {
            for (int i = 0; i < length; i++)
            {
              int pos = renderBuffer[i] * 3;
              uint32_t r = palette[pos];
              uint32_t g = palette[pos + 1];
              uint32_t b = palette[pos + 2];

              rgb565Data[i] = (uint16_t)(((r & 0xF8u) << 8) | ((g & 0xFCu) << 3) | (b >> 3));
            }
          }
        }

        if (update) m_pPixelcadeDMD->Update(rgb565Data);
      }

      if (++bufferPosition >= DMD_FRAME_BUFFER_SIZE) bufferPosition = 0;
    }
  }
}
#endif

void DMD::LevelDMDThread()
{
  int bufferPosition = 0;
  uint8_t renderBuffer[256 * 64] = {0};

  while (true)
  {
    std::shared_lock<std::shared_mutex> sl(m_dmdSharedMutex);
    m_dmdCV.wait(sl, [&]() { return m_dmdFrameReady || m_stopFlag; });
    sl.unlock();
    if (m_stopFlag)
    {
      return;
    }

    while (!m_stopFlag && bufferPosition != m_updateBufferPosition)
    {
      if (!m_levelDMDs.empty() && m_updateBuffer[bufferPosition]->mode == DMDMode::Data && !m_pSerum)
      {
        int length = m_updateBuffer[bufferPosition]->width * m_updateBuffer[bufferPosition]->height;
        if (memcmp(renderBuffer, m_updateBuffer[bufferPosition]->pData, length) != 0)
        {
          memcpy(renderBuffer, m_updateBuffer[bufferPosition]->pData, length);
          for (LevelDMD* pLevelDMD : m_levelDMDs)
          {
            if (pLevelDMD->GetLength() == length * 3)
              pLevelDMD->Update(renderBuffer, m_updateBuffer[bufferPosition]->depth);
          }
        }
      }

      if (++bufferPosition >= DMD_FRAME_BUFFER_SIZE) bufferPosition = 0;
    }
  }
}

void DMD::RGB24DMDThread()
{
  int bufferPosition = 0;
  uint16_t segData1[128];
  uint16_t segData2[128];
  uint8_t palette[192] = {0};
  uint8_t renderBuffer[256 * 64] = {0};
  uint8_t rgb24Data[256 * 64 * 3] = {0};

  while (true)
  {
    std::shared_lock<std::shared_mutex> sl(m_dmdSharedMutex);
    m_dmdCV.wait(sl, [&]() { return m_dmdFrameReady || m_stopFlag; });
    sl.unlock();
    if (m_stopFlag)
    {
      return;
    }

    while (!m_stopFlag && bufferPosition != m_updateBufferPosition)
    {
      if (!m_rgb24DMDs.empty())
      {
        int length = m_updateBuffer[bufferPosition]->width * m_updateBuffer[bufferPosition]->height;
        bool update = false;

        if (m_updateBuffer[bufferPosition]->mode == DMDMode::RGB24)
        {
          if (memcmp(rgb24Data, m_updateBuffer[bufferPosition]->pData, length * 3) != 0)
          {
            memcpy(rgb24Data, m_updateBuffer[bufferPosition]->pData, length * 3);
            for (RGB24DMD* pRGB24DMD : m_rgb24DMDs)
            {
              if (pRGB24DMD->GetLength() == length * 3) pRGB24DMD->Update(rgb24Data);
            }
            // Reset renderBuffer in case the mode changes for the next frame to ensure that memcmp() will detect it.
            memset(renderBuffer, 0, sizeof(renderBuffer));
          }
        }
        else
        {
          // @todo At the momeent libserum only supports on instance. So don't apply colorization if any hardware DMD is
          // attached.
          if (m_updateBuffer[bufferPosition]->mode == DMDMode::Data && m_pSerum && !HasDisplay())
          {
            update = m_pSerum->Convert((uint8_t*)m_updateBuffer[bufferPosition]->pData, renderBuffer, palette,
                                       m_updateBuffer[bufferPosition]->width, m_updateBuffer[bufferPosition]->height);
          }
          else
          {
            update = UpdatePalette(palette, m_updateBuffer[bufferPosition]->depth, m_updateBuffer[bufferPosition]->r,
                                   m_updateBuffer[bufferPosition]->g, m_updateBuffer[bufferPosition]->b);

            if (m_updateBuffer[bufferPosition]->mode == DMDMode::Data)
            {
              if (memcmp(renderBuffer, m_updateBuffer[bufferPosition]->pData, length) != 0)
              {
                memcpy(renderBuffer, m_updateBuffer[bufferPosition]->pData, length);
                update = true;
              }
            }
            else if (m_updateBuffer[bufferPosition]->mode == DMDMode::AlphaNumeric)
            {
              if (memcmp(segData1, m_updateBuffer[bufferPosition]->pData, 128 * sizeof(uint16_t)) != 0)
              {
                memcpy(segData1, m_updateBuffer[bufferPosition]->pData, 128 * sizeof(uint16_t));
                update = true;
              }

              if (m_updateBuffer[bufferPosition]->pData2 &&
                  memcmp(segData2, m_updateBuffer[bufferPosition]->pData2, 128 * sizeof(uint16_t)) != 0)
              {
                memcpy(segData2, m_updateBuffer[bufferPosition]->pData2, 128 * sizeof(uint16_t));
                update = true;
              }

              if (update)
              {
                if (m_updateBuffer[bufferPosition]->pData2)
                  m_pAlphaNumeric->Render(renderBuffer, m_updateBuffer[bufferPosition]->layout,
                                          (const uint16_t*)segData1, (const uint16_t*)segData2);
                else
                  m_pAlphaNumeric->Render(renderBuffer, m_updateBuffer[bufferPosition]->layout,
                                          (const uint16_t*)segData1);
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
      }

      if (++bufferPosition >= DMD_FRAME_BUFFER_SIZE) bufferPosition = 0;
    }
  }
}

bool DMD::UpdatePalette(uint8_t* pPalette, uint8_t depth, uint8_t r, uint8_t g, uint8_t b)
{
  if (depth != 2 && depth != 4) return false;
  uint8_t palette[192];
  memcpy(palette, pPalette, 192);

  memset(pPalette, 0, 192);

  const uint8_t colors = (depth == 2) ? 4 : 16;
  uint8_t pos = 0;

  for (uint8_t i = 0; i < colors; i++)
  {
    float perc = FrameUtil::CalcBrightness((float)i / (float)(colors - 1));
    pPalette[pos++] = (uint8_t)(((float)r) * perc);
    pPalette[pos++] = (uint8_t)(((float)g) * perc);
    pPalette[pos++] = (uint8_t)(((float)b) * perc);
  }

  return (memcmp(pPalette, palette, 192) != 0);
}

}  // namespace DMDUtil
