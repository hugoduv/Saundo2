/* Host-side regression test for the actual incremental firmware decoder. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AUDIO_STREAM_H
#define WIFI_PROBE_H
#define WIFI_H
#define NFC_EVENTS_H
#define VOICE_STOP_H
#define NFC_UID_STR_LEN 21
#define __attribute__(x)
#define __DMB() ((void)0)
#define DAC_CHANNEL_1 0
#define DAC_ALIGN_12B_R 0
#define HAL_OK 0
#define WIFI_STATUS_OK 0
#define WIFI_TCP_PROTOCOL 0

typedef struct { int unused; } DAC_HandleTypeDef;
typedef struct { int unused; } TIM_HandleTypeDef;
DAC_HandleTypeDef hdac1;
TIM_HandleTypeDef htim6;
static uint8_t *mock_response;
static size_t mock_response_length, mock_response_offset;
static size_t mock_receive_limit = 173U;
static uint32_t mock_tick, mock_receives;
static char last_request[256];
int HAL_TIM_Base_Stop(TIM_HandleTypeDef *h) { (void)h; return 0; }
int HAL_TIM_Base_Start(TIM_HandleTypeDef *h) { (void)h; return 0; }
int HAL_DAC_Stop_DMA(DAC_HandleTypeDef *h, int c) { (void)h; (void)c; return 0; }
int HAL_DAC_SetValue(DAC_HandleTypeDef *h, int c, int a, int v)
{ (void)h; (void)c; (void)a; (void)v; return 0; }
int HAL_DAC_Start_DMA(DAC_HandleTypeDef *h, int c, uint32_t *p, int n, int a)
{ (void)h; (void)c; (void)p; (void)n; (void)a; return 0; }
uint32_t HAL_GetTick(void) { return mock_tick; }
int WIFI_CloseClientConnection(int s) { (void)s; return 0; }
int WIFI_OpenClientConnection(int s, int p, const char *n, const uint8_t *ip, int port, int t)
{ (void)s; (void)p; (void)n; (void)ip; (void)port; (void)t; return 0; }
int WIFI_SendData(int s, const uint8_t *p, uint16_t n, uint16_t *sent, int t)
{
    (void)s; (void)t;
    if (n < sizeof(last_request)) { memcpy(last_request, p, n); last_request[n] = '\0'; }
    *sent = n; return 0;
}
int WIFI_ReceiveData(int s, uint8_t *p, int n, uint16_t *r, int t)
{
    size_t count = mock_response_length - mock_response_offset;
    (void)s; (void)t;
    ++mock_receives;
    if (count > mock_receive_limit) count = mock_receive_limit;
    if (count > (size_t)n) count = (size_t)n;
    if (count) memcpy(p, mock_response + mock_response_offset, count);
    mock_response_offset += count;
    *r = (uint16_t)count;
    return 0;
}
int wifi_probe_is_ready(void) { return 1; }
const uint8_t *wifi_probe_server_ip(void) { static const uint8_t ip[4] = {0}; return ip; }
uint16_t wifi_probe_server_port(void) { return 8080; }
int wifi_probe_recover(void) { return 1; }
void voice_stop_wifi_reset(void) {}

#include "../Core/Src/audio_stream.c"

static void check(int condition, const char *message)
{
    if (!condition) { fprintf(stderr, "%s\n", message); exit(1); }
}

static unsigned int simulated_half;
static uint32_t simulated_dma_tick;
static uint32_t simulated_mic_pending;
static int simulate_microphone;

static void advance_time(uint32_t duration)
{
    for (uint32_t i = 0; i < duration; ++i) {
        ++mock_tick;
        if (simulate_microphone) simulated_mic_pending += 16U;
        if (playing && ++simulated_dma_tick == 8U) {
            simulated_dma_tick = 0;
            if (simulated_half == 0U) audio_stream_dma_half(&hdac1);
            else audio_stream_dma_complete(&hdac1);
            simulated_half ^= 1U;
        }
    }
}

static void simulated_audio_step(void)
{
    uint32_t before_write = state == STREAM_OPEN ? 0U : ring_write;
    uint32_t before_receives = mock_receives;
    audio_stream_process();
    advance_time(((ring_write - before_write) / MP3_SAMPLES) * 8U +
                 (mock_receives - before_receives) * 19U);
}

static void simulated_audio_service(void)
{
    uint32_t started = mock_tick;
    do {
        simulated_audio_step();
    } while (audio_stream_needs_refill() && mock_tick - started < 200U);
}

static void simulated_scheduled_audio(void)
{
    uint32_t started = mock_tick;
    if (simulated_mic_pending >= 1200U && !audio_stream_refill_urgent()) return;
    do {
        simulated_audio_step();
    } while (audio_stream_needs_refill() && simulated_mic_pending < 1200U &&
             mock_tick - started < 200U);
}

int main(int argc, char **argv)
{
    FILE *input;
    long length;
    uint8_t *bytes;
    char header[128];
    size_t i;
    uint32_t frames = 0, non_silent = 0;
    int result;
    check(argc == 2, "expected MP3 filename");
    input = fopen(argv[1], "rb");
    check(input != NULL, "cannot open MP3");
    check(fseek(input, 0, SEEK_END) == 0, "seek failed");
    length = ftell(input);
    check(length >= 288 && fseek(input, 0, SEEK_SET) == 0, "invalid input size");
    bytes = malloc((size_t)length);
    check(bytes != NULL && fread(bytes, 1, (size_t)length, input) == (size_t)length, "read failed");
    fclose(input);

    reset_stream();
    snprintf(header, sizeof(header), "HTTP/1.1 200 OK\r\nContent-Length: %ld\r\n\r\n", length);
    for (i = 0; i < strlen(header); ++i) {
        network[0] = (uint8_t)header[i];
        check(consume_network(1), "fragmented HTTP header rejected");
    }
    for (i = 0; i < (size_t)length; ++i) {
        network[0] = bytes[i];
        check(consume_network(1), "fragmented MP3 rejected");
        result = decode_frame();
        check(result >= 0, "frame decode failed");
        if (result == 1) {
            for (uint32_t j = ring_read; j < ring_write; ++j) {
                uint16_t sample = ring[j & (RING_SIZE - 1U)];
                check(sample <= 4095, "invalid DAC sample");
                if (sample < 2000 || sample > 2096) ++non_silent;
            }
            ring_read = ring_write;
            ++frames;
        }
    }
    check(mp3_used == 0 && body_received == content_length, "incomplete frame or body");
    check(frames >= 28 && non_silent > 1000, "missing decoded audio");
    network[0] = 0;
    check(!consume_network(1), "extra body bytes accepted");
    reset_stream();
    mp3_used = 4;
    memset(mp3_buffer, 0, 4);
    check(decode_frame() == -1, "invalid frame accepted");
    reset_stream();
    memcpy(mp3_buffer, bytes, 4);
    mp3_used = 4;
    check(decode_frame() == 0, "truncated frame decoded");
    reset_stream();
    strcpy(stream_etag, "\"test\"");
    content_length = 3000U;
    body_received = resume_offset = 1000U;
    resuming = 1U;
    strcpy(http_header, "HTTP/1.1 206 Partial Content\r\nContent-Length: 2000\r\nContent-Range: bytes 1000-2999/3000\r\nETag: \"test\"\r\n\r\n");
    check(parse_http_header(), "valid HTTP resume rejected");
    strcpy(http_header, "HTTP/1.1 206 Partial Content\r\nContent-Length: 2000\r\nContent-Range: bytes 1000-2999/3000\r\nETag: \"changed\"\r\n\r\n");
    check(!parse_http_header(), "changed representation accepted on resume");
    strcpy(http_header, "HTTP/1.1 200 OK\r\nContent-Length: 3000\r\nETag: \"test\"\r\n\r\n");
    check(!parse_http_header(), "full response accepted as partial resume");
    strcpy(http_header, "HTTP/1.1 206 Partial Content\r\nContent-Length: 2000\r\nContent-Range: bytes 999-2999/3000\r\nETag: \"test\"\r\n\r\n");
    check(!parse_http_header(), "incorrect resume offset accepted");
    reset_stream();
    {
        size_t prefix = 100U;
        size_t resumed_header;
        char partial[256];
        check((size_t)length > prefix, "MP3 too short for recovery test");
        content_length = (uint32_t)length;
        body_received = (uint32_t)prefix;
        http_complete = 1U;
        memcpy(mp3_buffer, bytes, prefix);
        mp3_used = (uint32_t)prefix;
        strcpy(stream_etag, "\"test\"");
        strcpy(pending_uid, "12345678");
        state = STREAM_RECEIVE;
        playing = 0U;
        audio_stream_wifi_fault();
        resumed_header = (size_t)snprintf(partial, sizeof(partial),
            "HTTP/1.1 206 Partial Content\r\nContent-Length: %lu\r\nContent-Range: bytes 100-%lu/%lu\r\nETag: \"test\"\r\n\r\n",
            (unsigned long)length - 100UL, (unsigned long)length - 1UL, (unsigned long)length);
        mock_response_length = resumed_header + (size_t)length - prefix;
        mock_response = malloc(mock_response_length);
        check(mock_response != NULL, "resume allocation failed");
        memcpy(mock_response, partial, resumed_header);
        memcpy(mock_response + resumed_header, bytes + prefix, (size_t)length - prefix);
        mock_response_offset = 0;
        audio_stream_process();
        check(strstr(last_request, "Range: bytes=100-") != NULL &&
              strstr(last_request, "If-Range: \"test\"") != NULL, "missing resume headers");
        for (unsigned int j = 0; j < 100000 && state != STREAM_FINISHED && state != STREAM_FAILED; ++j) {
            audio_stream_process();
            if (playing) {
                if (j % 2U) audio_stream_dma_half(&hdac1);
                else audio_stream_dma_complete(&hdac1);
            }
        }
        check(state == STREAM_FINISHED && body_received == (uint32_t)length,
              "resumed stream did not finish");
        free(mock_response);
        mock_response = NULL;
        mock_response_length = mock_response_offset = 0U;
    }
    reset_stream();
    for (uint32_t j = 0; j < START_SAMPLES; ++j) ring[j] = (uint16_t)(1000U + j % 500U);
    ring_write = 100U;
    check(fill_half(dma_buffer) == 0 && buffering && underruns == 1U,
          "underrun did not begin buffering");
    check(ring_read == 0 && dma_buffer[0] == 2048U, "underrun consumed partial samples");
    ring_write = START_SAMPLES - 1U;
    check(fill_half(dma_buffer) == 0 && buffering && underruns == 1U && ring_read == 0,
          "buffering resumed too early or counted twice");
    ring_write = START_SAMPLES;
    check(fill_half(dma_buffer) == 0 && !buffering && ring_read == DMA_SIZE / 2U,
          "buffering did not resume at threshold");
    check(dma_buffer[0] == 1000U && dma_buffer[255] == 1255U,
          "rebuffering did not preserve samples");
    reset_stream();
    ring[0] = 1234U;
    ring_write = 1U;
    source_finished = 1U;
    buffering = 1U;
    check(fill_half(dma_buffer) == 1 && ring_read == 1U &&
          dma_buffer[0] == 1234U && dma_buffer[1] == 2048U,
          "short final buffer did not drain");
    reset_stream();
    state = STREAM_RECEIVE;
    playing = 1U;
    strcpy(pending_uid, "12345678");
    audio_stream_pause();
    check(paused && playing && state == STREAM_RECEIVE, "pause lost stream state");
    audio_stream_play();
    check(!paused && playing && state == STREAM_RECEIVE, "resume lost stream state");
    audio_stream_restart();
    check(!paused && !playing && state == STREAM_OPEN, "restart did not reopen stream");
    audio_stream_tag_removed();
    check(state == STREAM_IDLE && pending_uid[0] == '\0', "tag removal did not stop");
    {
        size_t header_length = strlen(header);
        unsigned int half = 0, iterations, forced_stall = 0;
        mock_response_length = header_length + (size_t)length;
        mock_response = malloc(mock_response_length);
        check(mock_response != NULL, "mock allocation failed");
        memcpy(mock_response, header, header_length);
        memcpy(mock_response + header_length, bytes, (size_t)length);
        mock_response_offset = 0;
        audio_stream_tag_placed("12345678");
        for (iterations = 0; iterations < 100000 && state != STREAM_FINISHED && state != STREAM_FAILED; ++iterations) {
            audio_stream_process();
            if (playing && !forced_stall) {
                for (unsigned int j = 0; j < 80U; ++j) {
                    if (half == 0U) audio_stream_dma_half(&hdac1);
                    else audio_stream_dma_complete(&hdac1);
                    half ^= 1U;
                }
                check(buffering && underruns == 1U && playing,
                      "simulated producer stall stopped playback");
                forced_stall = 1U;
            }
            if (playing && iterations % 2U == 0U) {
                if (half == 0U) audio_stream_dma_half(&hdac1);
                else audio_stream_dma_complete(&hdac1);
                half ^= 1U;
            }
        }
        check(state == STREAM_FINISHED, "stream did not finish normally");
        check(forced_stall && underruns >= 1U && !buffering,
              "stream did not recover from simulated stall");
        check(mock_response_offset == mock_response_length, "stream ended before all bytes arrived");
        mock_response_offset = 0;
        mock_receive_limit = 256U;
        mock_tick = mock_receives = simulated_half = simulated_dma_tick = 0U;
        audio_stream_tag_placed("12345678");
        for (iterations = 0; iterations < 10000 && !underruns &&
             state != STREAM_FINISHED && state != STREAM_FAILED; ++iterations) {
            advance_time(45U);
            simulated_audio_step();
            advance_time(40U);
        }
        check(underruns > 0U, "single audio service did not reproduce modeled starvation");
        mock_response_offset = 0;
        mock_tick = mock_receives = simulated_half = simulated_dma_tick = 0U;
        audio_stream_tag_placed("12345678");
        for (iterations = 0; iterations < 10000 && state != STREAM_FINISHED && state != STREAM_FAILED; ++iterations) {
            advance_time(45U); /* simulated NFC work */
            simulated_audio_service();
            advance_time(40U); /* simulated voice work */
            simulated_audio_service();
            if (underruns) {
                fprintf(stderr, "timed underrun at %lu ms, received %lu/%lu bytes\n",
                        (unsigned long)mock_tick, (unsigned long)body_received,
                        (unsigned long)content_length);
                break;
            }
        }
        check(state == STREAM_FINISHED, "timed playback did not finish");
        check(underruns == 0U, "timed playback underrun with modeled competing work");
        mock_response_offset = 0;
        mock_receive_limit = 512U;
        mock_tick = mock_receives = simulated_half = simulated_dma_tick = 0U;
        simulated_mic_pending = 0U;
        simulate_microphone = 1;
        audio_stream_tag_placed("12345678");
        for (iterations = 0; iterations < 10000 && state != STREAM_FINISHED && state != STREAM_FAILED; ++iterations) {
            advance_time(10U);
            simulated_scheduled_audio();
            for (unsigned int j = 0; j < 4U; ++j) {
                if (simulated_mic_pending >= 1200U) {
                    simulated_mic_pending -= 1200U;
                    advance_time(25U);
                }
                simulated_scheduled_audio();
                if (simulated_mic_pending < 1200U) break;
            }
            check(simulated_mic_pending < 16384U, "simulated microphone overflow");
            check(underruns == 0U, "simulated simultaneous playback underrun");
        }
        simulate_microphone = 0;
        check(state == STREAM_FINISHED, "simultaneous playback did not finish");
        check(simulated_mic_pending < 2400U, "microphone backlog did not converge");
        free(mock_response);
    }
    free(bytes);
    printf("Decoded %lu frames and %lu non-silent samples\n",
           (unsigned long)frames, (unsigned long)non_silent);
    return 0;
}
