# libdmdutil

A cross platform library for performing DMD tasks.

This library is currently used by [Visual Pinball Standalone](https://github.com/vpinball/vpinball/tree/standalone) for processing [PinMAME](https://github.com/vpinball/pinmame/tree/master/src/libpinmame) and [FlexDMD](https://github.com/vbousquet/flexdmd) DMD frames. It supports colorizing using [Serum](https://github.com/zesinger/libserum), outputing to [ZeDMD](https://github.com/ppuc/zedmd) and [Pixelcade](https://pixelcade.org) devices, and providing intensity and RGB24 buffers that can be used to render in table and external DMDs for [Visual Pinball](https://github.com/vpinball/vpinball).

## Usage:

```
#include "DMDUtil/DMDUtil.h"
.
.
void setup()
{
   DMDUtil::Config* pConfig = DMDUtil::Config::GetInstance();
   pConfig->SetZeDMD(true);
   pConfig->SetZeDMDDevice("/dev/cu.usbserial-0001");
   pConfig->SetPixelcadeDMD(false);
}

void test()
{
   DMDUtil::DMD* pDmd = new DMDUtil::DMD();
   pDmd->FindDisplays();

   DMDUtil::RGB24DMD* pRGB24DMD = pDmd->CreateRGB24DMD(128, 32);

   uint8_t* pData = (uint8_t*)malloc(128 * 32 * 3);
   .
   .
   .
   pDmd->UpdateRGB24Data((const UINT8*)pData, 128, 32);

   uint8_t* pRGB24Data = pRGB24DMD->GetRGB24Data();

   if (pRGB24Data) {
      // Render pRGB24Data
   }

   pDmd->DestroyRGB24DMD(pRGB24DMD);
}
```

## dmdserver

`dmdserver` provides a server process on top of `libdmdutil`. It listens on port 6789 on localhost and accepts "raw" TCP connections.
It expects two packages to render a DMD frame. The first one is a DmdStream header followed by the "data".

The DmdStreamHeader is defined as a struct:
```
  struct DMDUtil::DMD::StreamHeader
  {
    char protocol[10] = "DMDStream"; // \0 terminated string 
    uint8_t version = 1;
    Mode mode = Mode::Data;  // int
    uint16_t width = 0;
    uint16_t height = 0;
    uint32_t length = 0;
  };

```

The data package is a simple byte stream. It's data format has to match `StreamHeader.mode`.
At the moment, three data modes are supported (If you don't use C++, you could use uint_32 / int instead):
1. `Mode::Data`, int 1
2. `Mode::RGB24`, int 2, also known as RGB888
3. `Mode::RGB16`, int 3, also known as RGB565

`Mode::RGB24` is a uint8_t data stream of `StreamHeader.length`.
Each pixel consist of three uint8_t bytes representening the colors R, G and B.
`StreamHeader.width` and `StreamHeader.height` have to be provided in the header.

`Mode::RGB216` is a uint16_t data stream of `StreamHeader.length`.
Each pixel consist of one uint16_t having its color encoded in 16bit according to the RGB565 standard.
`StreamHeader.width` and `StreamHeader.height` have to be provided in the header.

`Mode::Data` is a serialized stream of the `DMDUtil::DMD::Update` struct.
It will be sent from a client which uses libdmdutil itself (for example VPX Standalone or PPUC).
In case of `Mode::Data`, `StreamHeader.width`, `StreamHeader.height` and `StreamHeader.length` should be set to zero.
The data itself contains that information and a lot of additional data like instructions to map alphanummeric displays to DMDs or
colorizations of grayscale content.
So if you want to write a general purpose client to display images or text, you're adviced to use `Mode::RGB24` or `Mode::RGB216`!

Note:
At the moment, `StreamHeader.length` is a redundant information as it could be calculated from `StreamHeader.width` and
`StreamHeader.height` in combination with `StreamHeader.mode`.
But if data compression will be supported in future versions, it will become important.

### Examples

To send a RGB24 image of 4x2 pixels, you have to sent these two packages:

```
0x   0x   0x   0x   0x   0x   0x   0x   0x   0x00  // "DMDStream"
0x01                                               // Version 1
0x02                                               // Mode::RGB24
0x04 0x00                                          // width 4 (if your system is big endian, the byte order needs to be swapped)
0x02 0x00                                          // height 2 (if your system is big endian, the byte order needs to be swapped)
0x18 0x00 0x00 0x00                                // length 24 (if your system is big endian, the byte order needs to be swapped)
```

```
0xFF 0xFF 0xFF // row 1, pixel 1 R G B
0xFF 0xFF 0xFF // row 1, pixel 2 R G B
0xFF 0xFF 0xFF // row 1, pixel 3 R G B
0xFF 0xFF 0xFF // row 1, pixel 4 R G B
0xFF 0xFF 0xFF // row 2, pixel 1 R G B
0xFF 0xFF 0xFF // row 2, pixel 2 R G B
0xFF 0xFF 0xFF // row 2, pixel 3 R G B
0xFF 0xFF 0xFF // row 2, pixel 4 R G B
```

### Multiple Connections

`dmdserver` accepts muliple connections in parallel, but the last connection "wins".
That means that the data of the last client that connected get displayed. All previous connections from other cleints are "paused".
As soon as the last connection gets terminated by the client, the newest previous one becomes active again (if it is still active).
The "paused" connections aren't really paused. Their data is still accepted but dropped instead of dispalyed.

## Building:

#### Windows (x64)

```shell
platforms/win/x64/external.sh
cmake -G "Visual Studio 17 2022" -DPLATFORM=win -DARCH=x64 -B build
cmake --build build --config Release
```

#### Windows (x86)

```shell
platforms/win/x86/external.sh
cmake -G "Visual Studio 17 2022" -A Win32 -DPLATFORM=win -DARCH=x86 -B build
cmake --build build --config Release
```

#### Linux (x64)
```shell
platforms/linux/x64/external.sh
cmake -DPLATFORM=linux -DARCH=x64 -DCMAKE_BUILD_TYPE=Release -B build
cmake --build build
```

#### Linux (aarch64)
```shell
platforms/linux/aarch64/external.sh
cmake -DPLATFORM=linux -DARCH=aarch64 -DCMAKE_BUILD_TYPE=Release -B build
cmake --build build
```

#### MacOS (arm64)
```shell
platforms/macos/arm64/external.sh
cmake -DPLATFORM=macos -DARCH=arm64 -DCMAKE_BUILD_TYPE=Release -B build
cmake --build build
```

#### MacOS (x64)
```shell
platforms/macos/x64/external.sh
cmake -DPLATFORM=macos -DARCH=x64 -DCMAKE_BUILD_TYPE=Release -B build
cmake --build build
```

#### iOS (arm64)
```shell
platforms/ios/arm64/external.sh
cmake -DPLATFORM=ios -DARCH=arm64 -DCMAKE_BUILD_TYPE=Release -B build
cmake --build build
```

#### tvOS (arm64)
```shell
platforms/tvos/arm64/external.sh
cmake -DPLATFORM=tvos -DARCH=arm64 -DCMAKE_BUILD_TYPE=Release -B build
cmake --build build
```

#### Android (arm64-v8a)
```shell
platforms/android/arm64-v8a/external.sh
cmake -DPLATFORM=android -DARCH=arm64-v8a -DCMAKE_BUILD_TYPE=Release -B build
cmake --build build
```
