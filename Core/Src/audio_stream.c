#include "audio_stream.h"
#include "wifi_probe.h"
#include "wifi.h"
#include "nfc_events.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* minimp3 ea99364f61c14656440e8d77e9c233ccf3124633, CC0.
 * Optimization is required even in Debug: the decoder has a large stack frame. */
#define MINIMP3_ONLY_MP3
#define MINIMP3_IMPLEMENTATION
#pragma GCC push_options
#pragma GCC optimize ("O2")
#include "minimp3.h"
#pragma GCC pop_options

#define STREAM_SOCKET 0U
#define NETWORK_BLOCK 1024U
#define MP3_BUFFER_SIZE 4096U
#define MP3_SAMPLES 1152U
#define RING_SIZE 16384U
#define START_SAMPLES 12288U
#define URGENT_SAMPLES 4096U
#define DMA_SIZE 512U
#define DECODE_BATCH 8U
#define MP3_RECEIVE_LOW_WATER 1024U
#define FADE_SAMPLES 128U
#define HTTP_HEADER_SIZE 1024U
#define FIRST_RESPONSE_TIMEOUT 60000U
#define STREAM_TIMEOUT 3000U

extern DAC_HandleTypeDef hdac1;
extern TIM_HandleTypeDef htim6;

typedef enum {
    STREAM_IDLE, STREAM_OPEN, STREAM_RECEIVE, STREAM_DRAIN, STREAM_FINISHED, STREAM_FAILED
} stream_state_t;
typedef enum {
    PLAY_OK, PLAY_FINISHED, PLAY_DMA_ERROR
} play_result_t;

static stream_state_t state = STREAM_IDLE;
static uint32_t stream_session;
static char pending_uid[NFC_UID_STR_LEN];
static uint8_t socket_open;
static uint8_t paused;
static uint8_t network[NETWORK_BLOCK];
static uint16_t ring[RING_SIZE] __attribute__((section(".audio_ram2"), aligned(4)));
static uint16_t dma_buffer[DMA_SIZE] __attribute__((aligned(4)));
static volatile uint32_t ring_read;
static volatile uint32_t ring_write;
static volatile uint8_t source_finished;
static volatile uint8_t playing;
static volatile uint8_t half_final[2];
static volatile play_result_t play_result;
static volatile uint8_t buffering;
static volatile uint32_t underruns;
static uint32_t reported_underruns;
static uint32_t max_decode_ms;
static uint32_t max_receive_ms;
static uint32_t max_nfc_ms;
static uint32_t max_voice_ms;
static uint32_t max_service_gap_ms;
static uint32_t last_service_tick;
static uint8_t service_tick_valid;

static char http_header[HTTP_HEADER_SIZE];
static uint32_t http_used;
static uint32_t content_length;
static uint32_t body_received;
static uint8_t http_complete;
static uint32_t last_data_tick;
static uint8_t mp3_buffer[MP3_BUFFER_SIZE];
static uint32_t mp3_used;
static mp3dec_t decoder;
static mp3d_sample_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
static uint32_t sample_position;

static void stop_dac(void)
{
    playing = 0U;
    (void)HAL_TIM_Base_Stop(&htim6);
    (void)HAL_DAC_Stop_DMA(&hdac1, DAC_CHANNEL_1);
    (void)HAL_DAC_SetValue(&hdac1, DAC_CHANNEL_1, DAC_ALIGN_12B_R, 2048U);
}

static void close_socket(void)
{
    if (socket_open)
    {
        socket_open = 0U;
        if (WIFI_CloseClientConnection(STREAM_SOCKET) != WIFI_STATUS_OK)
            printf("[AUDIO] Erreur de fermeture du socket.\r\n");
    }
}

static void fail_stream(const char *reason)
{
    stop_dac();
    printf("[AUDIO] %s\r\n", reason);
    close_socket();
    state = STREAM_FAILED;
}

/* The producer publishes ring_write only after writing samples; DMA is the sole consumer. */
static uint8_t fill_half(uint16_t *out)
{
    uint32_t read = ring_read;
    uint32_t write = ring_write;
    uint32_t available = write - read;
    uint32_t count = available < DMA_SIZE / 2U ? available : DMA_SIZE / 2U;
    uint32_t i;
    __DMB();
    if (!source_finished &&
        ((buffering && available < START_SAMPLES) || count < DMA_SIZE / 2U))
    {
        if (!buffering)
        {
            buffering = 1U;
            ++underruns;
        }
        for (i = 0U; i < DMA_SIZE / 2U; ++i) out[i] = 2048U;
        return 0U;
    }
    buffering = 0U;
    for (i = 0U; i < count; ++i)
        out[i] = ring[(read + i) & (RING_SIZE - 1U)];
    for (; i < DMA_SIZE / 2U; ++i)
        out[i] = 2048U;
    __DMB();
    ring_read = read + count;
    if (source_finished && ring_read == ring_write) return 1U;
    return 0U;
}

static void dma_callback(DAC_HandleTypeDef *dac, uint32_t half)
{
    uint8_t result;
    if (dac != &hdac1 || !playing) return;
    if (half_final[half])
    {
        stop_dac();
        play_result = PLAY_FINISHED;
        return;
    }
    result = fill_half(&dma_buffer[half * (DMA_SIZE / 2U)]);
    half_final[half] = result;
}

void audio_stream_dma_half(DAC_HandleTypeDef *dac) { dma_callback(dac, 0U); }
void audio_stream_dma_complete(DAC_HandleTypeDef *dac) { dma_callback(dac, 1U); }

void audio_stream_dma_error(DAC_HandleTypeDef *dac)
{
    if (dac == &hdac1 && playing)
    {
        stop_dac();
        play_result = PLAY_DMA_ERROR;
    }
}

static int start_dac(void)
{
    uint8_t first = fill_half(dma_buffer);
    uint8_t second = fill_half(&dma_buffer[DMA_SIZE / 2U]);
    half_final[0] = first;
    half_final[1] = second;
    play_result = PLAY_OK;
    (void)HAL_DAC_SetValue(&hdac1, DAC_CHANNEL_1, DAC_ALIGN_12B_R, 2048U);
    playing = 1U;
    if (HAL_DAC_Start_DMA(&hdac1, DAC_CHANNEL_1, (uint32_t *)dma_buffer,
                          DMA_SIZE, DAC_ALIGN_12B_R) != HAL_OK)
    {
        playing = 0U;
        return 0;
    }
    if (HAL_TIM_Base_Start(&htim6) != HAL_OK)
    {
        stop_dac();
        return 0;
    }
    return 1;
}

static int header_name_equal(const char *line, const char *name)
{
    while (*name)
    {
        char a = *line++;
        char b = *name++;
        if (a >= 'A' && a <= 'Z') a = (char)(a + ('a' - 'A'));
        if (a != b) return 0;
    }
    return *line == ':';
}

static int parse_http_header(void)
{
    char *line;
    char *end;
    unsigned long length;
    uint8_t found = 0U;
    if (strncmp(http_header, "HTTP/1.1 200 ", 13U) != 0 &&
        strncmp(http_header, "HTTP/1.0 200 ", 13U) != 0) return 0;
    line = strstr(http_header, "\r\n");
    while (line && line[2] != '\r')
    {
        line += 2;
        if (header_name_equal(line, "content-length"))
        {
            char *value = line + sizeof("content-length");
            while (*value == ' ' || *value == '\t') ++value;
            if (*value < '0' || *value > '9' || found) return 0;
            length = strtoul(value, &end, 10);
            if (end == value || *end != '\r' || length < 288UL ||
                length > 0x7FFFFFFFUL) return 0;
            content_length = (uint32_t)length;
            found = 1U;
        }
        line = strstr(line, "\r\n");
    }
    return found;
}

static int consume_network(uint16_t count)
{
    for (uint32_t i = 0U; i < count; ++i)
    {
        if (!http_complete)
        {
            if (http_used >= HTTP_HEADER_SIZE - 1U) return 0;
            http_header[http_used++] = (char)network[i];
            http_header[http_used] = '\0';
            if (http_used >= 4U &&
                memcmp(&http_header[http_used - 4U], "\r\n\r\n", 4U) == 0)
            {
                if (!parse_http_header()) return 0;
                http_complete = 1U;
            }
        }
        else
        {
            if (body_received >= content_length || mp3_used == MP3_BUFFER_SIZE) return 0;
            mp3_buffer[mp3_used++] = network[i];
            ++body_received;
        }
    }
    return 1;
}

/* The dedicated endpoint produces headerless MPEG-1 Layer III, mono, 32 kHz,
 * 64 kbit/s CBR. Checking the header lets us wait for a complete split frame. */
static int decode_frame(void)
{
    mp3dec_frame_info_t info;
    uint32_t frame_size;
    uint32_t write;
    int samples;
    uint32_t started;
    if (mp3_used < 4U) return 0;
    if (mp3_buffer[0] != 0xffU || (mp3_buffer[1] & 0xfeU) != 0xfaU ||
        (mp3_buffer[2] & 0xfcU) != 0x58U || (mp3_buffer[3] & 0xc0U) != 0xc0U)
        return -1;
    frame_size = 288U + ((mp3_buffer[2] >> 1U) & 1U);
    if (mp3_used < frame_size) return 0;
    if (RING_SIZE - (ring_write - ring_read) < MP3_SAMPLES) return 0;
    started = HAL_GetTick();
    samples = mp3dec_decode_frame(&decoder, mp3_buffer, (int)frame_size, pcm, &info);
    if (HAL_GetTick() - started > max_decode_ms) max_decode_ms = HAL_GetTick() - started;
    if (samples != (int)MP3_SAMPLES || info.frame_bytes != (int)frame_size ||
        info.frame_offset != 0 || info.channels != 1 || info.hz != 32000 ||
        info.layer != 3 || info.bitrate_kbps != 64) return -1;
    write = ring_write;
    for (int i = 0; i < samples; ++i)
    {
        int32_t sample = pcm[i];
        if (sample_position < FADE_SAMPLES)
            sample = sample * (int32_t)sample_position / (int32_t)FADE_SAMPLES;
        ring[(write + (uint32_t)i) & (RING_SIZE - 1U)] =
            (uint16_t)((sample + 32768) >> 4);
        ++sample_position;
    }
    __DMB();
    ring_write = write + (uint32_t)samples;
    mp3_used -= frame_size;
    memmove(mp3_buffer, mp3_buffer + frame_size, mp3_used);
    return 1;
}

static void reset_stream(void)
{
    ring_read = ring_write = 0U;
    source_finished = 0U;
    buffering = 0U;
    underruns = reported_underruns = 0U;
    max_decode_ms = max_receive_ms = 0U;
    max_nfc_ms = max_voice_ms = max_service_gap_ms = 0U;
    service_tick_valid = 0U;
    play_result = PLAY_OK;
    half_final[0] = half_final[1] = 0U;
    http_used = content_length = body_received = 0U;
    http_complete = 0U;
    mp3_used = sample_position = 0U;
    memset(&decoder, 0, sizeof(decoder));
    mp3dec_init(&decoder);
}

void audio_stream_tag_placed(const char *uid)
{
    stop_dac();
    paused = 0U;
    ++stream_session;
    play_result = PLAY_OK;
    if (strlen(uid) >= sizeof(pending_uid))
    {
        printf("[AUDIO] UID trop long.\r\n");
        pending_uid[0] = '\0';
        state = STREAM_FAILED;
        return;
    }
    strcpy(pending_uid, uid);
    state = STREAM_OPEN;
}

void audio_stream_tag_removed(void)
{
    stop_dac();
    paused = 0U;
    pending_uid[0] = '\0';
    state = STREAM_IDLE;
}

void audio_stream_pause(void)
{
    if (paused || state == STREAM_IDLE || state == STREAM_FAILED || state == STREAM_FINISHED) return;
    if (playing && HAL_TIM_Base_Stop(&htim6) != HAL_OK)
    {
        fail_stream("Pause du DAC impossible.");
        return;
    }
    paused = 1U;
    printf("[AUDIO] Lecture en pause.\r\n");
}

void audio_stream_play(void)
{
    if (!paused) return;
    if (playing && HAL_TIM_Base_Start(&htim6) != HAL_OK)
    {
        fail_stream("Reprise du DAC impossible.");
        return;
    }
    last_data_tick = HAL_GetTick();
    paused = 0U;
    printf("[AUDIO] Lecture reprise.\r\n");
}

void audio_stream_restart(void)
{
    if (state == STREAM_IDLE || state == STREAM_FAILED || pending_uid[0] == '\0') return;
    stop_dac();
    paused = 0U;
    play_result = PLAY_OK;
    state = STREAM_OPEN;
    printf("[AUDIO] Lecture depuis le debut.\r\n");
}

int audio_stream_active(void) { return state != STREAM_IDLE && state != STREAM_FAILED; }
uint32_t audio_stream_session(void) { return stream_session; }

int audio_stream_needs_refill(void)
{
    return state == STREAM_RECEIVE && !paused &&
           ring_write - ring_read < START_SAMPLES;
}

int audio_stream_refill_urgent(void)
{
    return state == STREAM_RECEIVE && !paused &&
           ring_write - ring_read < URGENT_SAMPLES;
}

void audio_stream_note_nfc_time(uint32_t duration)
{
    if (duration > max_nfc_ms) max_nfc_ms = duration;
}

void audio_stream_note_voice_time(uint32_t duration)
{
    if (duration > max_voice_ms) max_voice_ms = duration;
}

void audio_stream_process(void)
{
    uint16_t received = 0U, sent = 0U;
    char request[128];
    int length, decoded;

    if (state == STREAM_IDLE || state == STREAM_FAILED)
    {
        close_socket();
        return;
    }
    if (playing && !paused && service_tick_valid)
    {
        uint32_t gap = HAL_GetTick() - last_service_tick;
        if (gap > max_service_gap_ms) max_service_gap_ms = gap;
    }
    last_service_tick = HAL_GetTick();
    service_tick_valid = 1U;
    if (play_result == PLAY_DMA_ERROR)
    {
        fail_stream("Erreur DMA DAC.");
        return;
    }
    if (underruns != reported_underruns)
    {
        reported_underruns = underruns;
        printf("[AUDIO] Buffer vide : recharge (%lu), MP3=%lu octets, decode=%lu ms, reception=%lu ms, NFC=%lu ms, voix=%lu ms, service=%lu ms (max).\r\n",
               (unsigned long)reported_underruns, (unsigned long)mp3_used,
               (unsigned long)max_decode_ms, (unsigned long)max_receive_ms,
               (unsigned long)max_nfc_ms, (unsigned long)max_voice_ms,
               (unsigned long)max_service_gap_ms);
    }
    if (play_result == PLAY_FINISHED)
    {
        close_socket();
        printf("[AUDIO] Lecture terminee.\r\n");
        play_result = PLAY_OK;
        state = STREAM_FINISHED;
        return;
    }
    if (paused || state == STREAM_FINISHED) return;
    if (state == STREAM_OPEN)
    {
        close_socket();
        reset_stream();
        if (!wifi_probe_is_ready())
        {
            fail_stream("Wi-Fi ou serveur non disponible ; redemarrer la carte.");
            return;
        }
        if (WIFI_OpenClientConnection(STREAM_SOCKET, WIFI_TCP_PROTOCOL, "SAUNDO2",
                                      wifi_probe_server_ip(), wifi_probe_server_port(), 0) != WIFI_STATUS_OK)
        {
            fail_stream("Connexion au serveur impossible.");
            return;
        }
        socket_open = 1U;
        length = snprintf(request, sizeof(request),
                          "GET /api/stream-mp3/%s HTTP/1.1\r\nHost: saundo2\r\nConnection: close\r\n\r\n",
                          pending_uid);
        if (length <= 0 || (size_t)length >= sizeof(request) ||
            WIFI_SendData(STREAM_SOCKET, (const uint8_t *)request, (uint16_t)length,
                          &sent, 5000U) != WIFI_STATUS_OK || sent != (uint16_t)length)
        {
            fail_stream("Envoi de la requete impossible.");
            return;
        }
        printf("[AUDIO] Chargement du flux MP3...\r\n");
        last_data_tick = HAL_GetTick();
        state = STREAM_RECEIVE;
    }
    if (state != STREAM_RECEIVE) return;
    decoded = 0;
    for (uint32_t i = 0U; i < DECODE_BATCH; ++i)
    {
        decoded = decode_frame();
        if (decoded < 0)
        {
            fail_stream("Trame MP3 invalide ou format non pris en charge.");
            return;
        }
        if (!decoded) break;
    }
    if (http_complete && body_received == content_length && mp3_used == 0U)
    {
        source_finished = 1U;
        close_socket();
        state = STREAM_DRAIN;
    }
    else if (http_complete && body_received == content_length && !decoded &&
             RING_SIZE - (ring_write - ring_read) >= MP3_SAMPLES)
    {
        fail_stream("Flux MP3 tronque.");
        return;
    }
    else if (body_received < content_length || !http_complete)
    {
        if (mp3_used < MP3_RECEIVE_LOW_WATER &&
            MP3_BUFFER_SIZE - mp3_used >= NETWORK_BLOCK)
        {
            uint32_t started = HAL_GetTick();
            if (WIFI_ReceiveData(STREAM_SOCKET, network, sizeof(network), &received, 20U) != WIFI_STATUS_OK)
            {
                fail_stream("Connexion interrompue pendant la lecture.");
                return;
            }
            if (HAL_GetTick() - started > max_receive_ms) max_receive_ms = HAL_GetTick() - started;
            if (received)
            {
                last_data_tick = HAL_GetTick();
                if (!consume_network(received))
                {
                    fail_stream("Reponse HTTP invalide ou UID inconnu.");
                    return;
                }
            }
            else if ((HAL_GetTick() - last_data_tick) >
                     (http_complete ? STREAM_TIMEOUT : FIRST_RESPONSE_TIMEOUT))
            {
                fail_stream("Delai de reception depasse.");
                return;
            }
        }
    }
    if (play_result != PLAY_OK) return;
    if (!playing && (ring_write - ring_read >= START_SAMPLES || source_finished))
    {
        if (!start_dac())
        {
            fail_stream("Demarrage du DAC impossible.");
            return;
        }
        printf("[AUDIO] Lecture en cours.\r\n");
    }
}
