# Saundo2

Saundo2 associates NFC tags with music stored on a local server. The board
detects a tag, retrieves its associated track over Wi-Fi, and plays it through
its audio output. Removing the tag stops playback.

## Hardware and connections

- Board: **B-L475E-IOT01A2**, with an **STM32L475VGTx** microcontroller and an ISM43362 Wi-Fi module.
- NFC reader: **X-NUCLEO-NFC08A1** shield, with an ST25R3916B controller.
- NFC connection over SPI1: SCK **PA5**, MISO **PA6**, MOSI **PA7**, CS **PA2**, and IRQ **PC5**.
- Audio: connect **DAC1_OUT1 on PA4** to an audio amplifier input, with a
  common ground. Do not connect a speaker directly to PA4.

## Audio and streaming

- The server converts audio files to **16 kHz, 16-bit, mono PCM WAV** and
  caches the result.
- When a tag is detected, the board requests `/api/stream/<UID>` over HTTP.
  It progressively receives the WAV, converts it to 8-bit samples, and fills
  a **32,768-sample** circular buffer in RAM. Playback starts after approximately
  one second of prebuffering, or when the entire file has arrived for shorter sounds.
- DAC1 channel 1: 8-bit output triggered by **TIM6 TRGO at 16 kHz**.
  TIM6 uses a prescaler of 0 and a period of 4999. Transfers use **DMA1
  channel 3 in circular mode**, with a 1024-byte DAC buffer.
- A network error or empty buffer stops playback. Streaming operation and
  stability still need to be verified on the board.

## Server and configuration

The Node.js server runs on the PC, on the same local network as the board
(for example, a 2.4 GHz phone hotspot). It uses **FFmpeg**, listens over HTTP
on port **8080** by default, and reads UID-to-file mappings from
[`Server/data/tracks.json`](Server/data/tracks.json). Place music files in
`Server/audio/`, then start the server:

```powershell
cd Server
npm.cmd start
```

Configure the Wi-Fi network and PC IP address locally using
[`wifi_settings.example.h`](Core/Inc/wifi_settings.example.h). Do not share
the file containing the password. This HTTP server is intended for local
testing, not direct exposure to the internet.

For detailed setup and testing instructions, see
[`Core/WIFI-SETUP.md`](Core/WIFI-SETUP.md) and
[`Server/README.md`](Server/README.md).
