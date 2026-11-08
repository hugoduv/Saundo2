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

- For the board, the server converts files to **32 kHz mono MP3 at 64 kbit/s**
  and caches them. The original WAV API remains available at 16 kHz/16-bit.
- The board requests `/api/stream-mp3/<UID>`, decodes MP3 frames with
  [minimp3](https://github.com/lieff/minimp3) (CC0, revision
  `ea99364f61c14656440e8d77e9c233ccf3124633`) and converts PCM samples to
  12-bit DAC values. The MP3 stream is about a quarter of the previous
  256 kbit/s PCM payload, excluding protocol overhead.
- The circular buffer holds **16384 samples** (512 ms), with **12288 samples**
  (384 ms) prebuffered. It resides in RAM2; the linker reserves 24 KiB of
  main RAM for the stack. The MP3 decoder alone uses about 16 KiB of stack
  in the Debug build.
- DAC1 channel 1: 12-bit output triggered by **TIM6 TRGO at 32 kHz**.
  TIM6 uses a prescaler of 0 and a period of 2499. DMA1 channel 3 uses a
  circular buffer of 512 halfword samples.
- A network error stops playback. If the decoded buffer runs out, the board
  outputs silence, logs a diagnostic and resumes once it has rebuffered.
  Frequent rebuffering means the system cannot sustain playback; the log
  reports maximum decode and receive durations to help diagnose the cause.
  Streaming operation and stability still need to be verified on the board.
- While a tag's session is active, including during pause and after playback
  finishes, the board sends microphone audio to the local server for offline
  recognition of three commands:
  - **« pause »** pauses playback.
  - **« play »** resumes from the paused position.
  - **« recommence »** restarts the current track from the beginning.

  Removing the tag stops playback and microphone capture. There is no "stop"
  voice command. This experimental feature requires Windows and a French
  Windows speech recognizer on the server; it examines 1.5-second windows
  every 0.5 seconds. Verify it on the board, especially
  with sound playing through the speaker. Microphone audio crosses the local
  network unencrypted and is not saved by this software. Do not expose the
  server to an untrusted network.

  The current firmware sends 16 kHz unsigned 8-bit microphone samples to the
  server on HTTP port + 2 (8082 by default), halving the previous microphone
  payload. The server expands them to 16-bit PCM for recognition. The legacy
  16-bit protocol remains available on HTTP port + 1 (8081). Restart the
  server after updating; allow the new port on the trusted local network.
  If the microphone buffer overflows, old audio is discarded and reported;
  commands spoken during a loss may not be recognized.

The firmware build and host-side decoding tests do not establish that the
STM32 can decode fast enough while NFC and microphone capture are active.
Test continuous playback, card removal, pause, play, and restart on hardware.
The NFC removal timeout remains two seconds to avoid untested false removals.
MP3 reduces downstream bandwidth but does not by itself fix the previously
observed Wi-Fi SPI/DRDY timeouts.

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
