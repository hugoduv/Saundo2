#include "audio_stream.h"
#include "wifi_probe.h"
#include "wifi.h"
#include "nfc_events.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STREAM_SOCKET 0U
#define NETWORK_BLOCK 1024U
#define RING_SIZE 32768U
#define START_SAMPLES 16384U
#define DMA_SIZE 1024U
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
    WAV_RIFF, WAV_CHUNK, WAV_FMT, WAV_SKIP, WAV_DATA, WAV_DONE
} wav_state_t;

typedef enum {
    PLAY_OK, PLAY_FINISHED, PLAY_UNDERRUN, PLAY_DMA_ERROR
} play_result_t;

static stream_state_t state = STREAM_IDLE;
static uint32_t stream_session;
static char pending_uid[NFC_UID_STR_LEN];
static uint8_t socket_open;
static uint8_t paused;
static uint8_t network[NETWORK_BLOCK];
static uint8_t ring[RING_SIZE];
static uint8_t dma_buffer[DMA_SIZE] __attribute__((aligned(4)));
static volatile uint32_t ring_read;
static volatile uint32_t ring_write;
static volatile uint8_t source_finished;
static volatile uint8_t playing;
static volatile uint8_t half_final[2];
static volatile play_result_t play_result;

static char http_header[HTTP_HEADER_SIZE];
static uint32_t http_used;
static uint32_t content_length;
static uint8_t http_complete;
static uint32_t last_data_tick;
static wav_state_t wav_state;
static uint8_t wav_header[16];
static uint32_t wav_header_used;
static uint32_t wav_offset;
static uint32_t wav_end;
static uint32_t chunk_remaining;
static uint8_t chunk_padding;
static uint8_t format_valid;
static uint32_t data_remaining;
static uint32_t total_samples;
static uint32_t sample_position;
static uint8_t sample_low;
static uint8_t have_low;

static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t read_le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static void stop_dac(void)
{
    playing = 0U;
    (void)HAL_TIM_Base_Stop(&htim6);
    (void)HAL_DAC_Stop_DMA(&hdac1, DAC_CHANNEL_1);
    (void)HAL_DAC_SetValue(&hdac1, DAC_CHANNEL_1, DAC_ALIGN_8B_R, 128U);
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
static uint8_t fill_half(uint8_t *out)
{
    uint32_t read = ring_read;
    uint32_t write = ring_write;
    uint32_t available = write - read;
    uint32_t count = available < DMA_SIZE / 2U ? available : DMA_SIZE / 2U;
    uint32_t i;
    __DMB();
    for (i = 0U; i < count; ++i)
        out[i] = ring[(read + i) & (RING_SIZE - 1U)];
    for (; i < DMA_SIZE / 2U; ++i)
        out[i] = 128U;
    __DMB();
    ring_read = read + count;
    if (source_finished && ring_read == ring_write)
        return 1U;
    if (count < DMA_SIZE / 2U)
        return 2U;
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
    if (result == 2U)
    {
        stop_dac();
        play_result = PLAY_UNDERRUN;
    }
    else
    {
        half_final[half] = result;
    }
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
    if (first == 2U || second == 2U) return 0;
    half_final[0] = first;
    half_final[1] = second;
    play_result = PLAY_OK;
    (void)HAL_DAC_SetValue(&hdac1, DAC_CHANNEL_1, DAC_ALIGN_8B_R, 128U);
    playing = 1U;
    if (HAL_DAC_Start_DMA(&hdac1, DAC_CHANNEL_1, (uint32_t *)dma_buffer,
                          DMA_SIZE, DAC_ALIGN_8B_R) != HAL_OK)
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
        strncmp(http_header, "HTTP/1.0 200 ", 13U) != 0)
        return 0;
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
            if (end == value || *end != '\r' || length < 44UL ||
                length > 0x7FFFFFFFUL) return 0;
            content_length = (uint32_t)length;
            found = 1U;
        }
        line = strstr(line, "\r\n");
    }
    return found;
}

static uint8_t sample_to_dac(int16_t sample)
{
    uint32_t fade = total_samples < FADE_SAMPLES ? total_samples : FADE_SAMPLES;
    uint32_t gain = fade;
    int32_t output;
    if (sample_position < fade) gain = sample_position;
    if (total_samples - sample_position - 1U < gain)
        gain = total_samples - sample_position - 1U;
    output = 128 + ((int32_t)sample * (int32_t)gain) / (256 * (int32_t)fade);
    if (output < 0) output = 0;
    if (output > 255) output = 255;
    return (uint8_t)output;
}

static int wav_byte(uint8_t byte, uint32_t *write)
{
    uint32_t size;
    if (wav_state != WAV_RIFF && wav_offset >= wav_end) return 0;
    ++wav_offset;
    switch (wav_state)
    {
    case WAV_RIFF:
        wav_header[wav_header_used++] = byte;
        if (wav_header_used == 12U)
        {
            size = read_le32(&wav_header[4]);
            if (memcmp(wav_header, "RIFF", 4) != 0 ||
                memcmp(&wav_header[8], "WAVE", 4) != 0 ||
                size < 36U || size > 0x7FFFFFF7UL || size + 8U != content_length)
                return 0;
            wav_end = size + 8U;
            wav_header_used = 0U;
            wav_state = WAV_CHUNK;
        }
        break;
    case WAV_CHUNK:
        wav_header[wav_header_used++] = byte;
        if (wav_header_used == 8U)
        {
            size = read_le32(&wav_header[4]);
            if (size > wav_end - wav_offset ||
                (size & 1U && size == wav_end - wav_offset)) return 0;
            chunk_remaining = size;
            chunk_padding = (uint8_t)(size & 1U);
            wav_header_used = 0U;
            if (memcmp(wav_header, "fmt ", 4) == 0)
            {
                if (size < 16U || format_valid) return 0;
                wav_state = WAV_FMT;
            }
            else if (memcmp(wav_header, "data", 4) == 0)
            {
                if (!format_valid || size == 0U || (size & 1U)) return 0;
                data_remaining = size;
                total_samples = size / 2U;
                wav_state = WAV_DATA;
            }
            else
            {
                chunk_remaining += chunk_padding;
                wav_state = chunk_remaining ? WAV_SKIP : WAV_CHUNK;
            }
        }
        break;
    case WAV_FMT:
        if (wav_header_used < 16U) wav_header[wav_header_used++] = byte;
        --chunk_remaining;
        if (chunk_remaining == 0U)
        {
            if (read_le16(wav_header) != 1U || read_le16(&wav_header[2]) != 1U ||
                read_le32(&wav_header[4]) != 16000U || read_le32(&wav_header[8]) != 32000U ||
                read_le16(&wav_header[12]) != 2U || read_le16(&wav_header[14]) != 16U)
                return 0;
            format_valid = 1U;
            wav_header_used = 0U;
            chunk_remaining = chunk_padding;
            wav_state = chunk_remaining ? WAV_SKIP : WAV_CHUNK;
        }
        break;
    case WAV_SKIP:
        if (--chunk_remaining == 0U) wav_state = WAV_CHUNK;
        break;
    case WAV_DATA:
        --data_remaining;
        if (!have_low)
        {
            sample_low = byte;
            have_low = 1U;
        }
        else
        {
            int16_t sample = (int16_t)((uint16_t)sample_low | ((uint16_t)byte << 8));
            ring[*write & (RING_SIZE - 1U)] = sample_to_dac(sample);
            ++*write;
            ++sample_position;
            have_low = 0U;
        }
        if (data_remaining == 0U) wav_state = WAV_DONE;
        break;
    case WAV_DONE:
        return 0;
    }
    return 1;
}

static int consume_network(uint16_t count)
{
    uint32_t write = ring_write;
    uint32_t i;
    for (i = 0U; i < count; ++i)
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
        else if (wav_state != WAV_DONE && !wav_byte(network[i], &write))
        {
            return 0;
        }
    }
    {
        uint32_t mask = __get_PRIMASK();
        __disable_irq();
        __DMB();
        ring_write = write;
        if (wav_state == WAV_DONE) source_finished = 1U;
        if (!mask) __enable_irq();
    }
    return 1;
}

static void reset_stream(void)
{
    ring_read = 0U;
    ring_write = 0U;
    source_finished = 0U;
    play_result = PLAY_OK;
    half_final[0] = half_final[1] = 0U;
    http_used = 0U;
    content_length = 0U;
    http_complete = 0U;
    wav_state = WAV_RIFF;
    wav_header_used = 0U;
    wav_offset = 0U;
    wav_end = 0U;
    chunk_remaining = 0U;
    chunk_padding = 0U;
    format_valid = 0U;
    data_remaining = 0U;
    total_samples = 0U;
    sample_position = 0U;
    have_low = 0U;
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
    if (paused || state == STREAM_IDLE || state == STREAM_FAILED || state == STREAM_FINISHED)
        return;
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
    if (state == STREAM_IDLE || state == STREAM_FAILED || pending_uid[0] == '\0')
        return;
    stop_dac();
    paused = 0U;
    play_result = PLAY_OK;
    state = STREAM_OPEN;
    printf("[AUDIO] Lecture depuis le debut.\r\n");
}

int audio_stream_active(void)
{
    return state != STREAM_IDLE && state != STREAM_FAILED;
}

uint32_t audio_stream_session(void)
{
    return stream_session;
}

void audio_stream_process(void)
{
    uint16_t received = 0U;
    uint16_t sent = 0U;
    char request[128];
    int length;

    if (state == STREAM_IDLE || state == STREAM_FAILED)
    {
        close_socket();
        return;
    }
    if (play_result == PLAY_UNDERRUN || play_result == PLAY_DMA_ERROR)
    {
        fail_stream(play_result == PLAY_UNDERRUN ?
                    "Buffer vide : lecture arretee." : "Erreur DMA DAC.");
        return;
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
                          "GET /api/stream/%s HTTP/1.1\r\nHost: saundo2\r\nConnection: close\r\n\r\n",
                          pending_uid);
        if (length <= 0 || (size_t)length >= sizeof(request) ||
            WIFI_SendData(STREAM_SOCKET, (const uint8_t *)request, (uint16_t)length,
                          &sent, 5000U) != WIFI_STATUS_OK || sent != (uint16_t)length)
        {
            fail_stream("Envoi de la requete impossible.");
            return;
        }
        printf("[AUDIO] Chargement du flux...\r\n");
        last_data_tick = HAL_GetTick();
        state = STREAM_RECEIVE;
    }
    if (state == STREAM_RECEIVE)
    {
        if (RING_SIZE - (ring_write - ring_read) < NETWORK_BLOCK / 2U + 1U)
            return;
        if (WIFI_ReceiveData(STREAM_SOCKET, network, sizeof(network), &received, 20U) != WIFI_STATUS_OK)
        {
            fail_stream("Connexion interrompue pendant la lecture.");
            return;
        }
        if (received)
        {
            last_data_tick = HAL_GetTick();
            if (!consume_network(received))
            {
                fail_stream("Reponse HTTP ou WAV invalide, ou UID inconnu.");
                return;
            }
        }
        else if ((HAL_GetTick() - last_data_tick) >
                 (http_complete ? STREAM_TIMEOUT : FIRST_RESPONSE_TIMEOUT))
        {
            fail_stream("Delai de reception depasse.");
            return;
        }
        if (play_result != PLAY_OK)
            return;
        if (!playing && (ring_write - ring_read >= START_SAMPLES || source_finished))
        {
            if (!start_dac())
            {
                fail_stream("Demarrage du DAC impossible.");
                return;
            }
            printf("[AUDIO] Lecture en cours.\r\n");
        }
        if (source_finished)
        {
            close_socket();
            state = STREAM_DRAIN;
        }
    }
}
