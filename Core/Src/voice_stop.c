#include "voice_stop.h"
#include "audio_stream.h"
#include "wifi.h"
#include "wifi_probe.h"
#include "stm32l4xx_hal.h"
#include <stdio.h>
#include <string.h>

#define VOICE_SOCKET 1U
#define MIC_DMA_SAMPLES 1024U
#define MIC_RING_SAMPLES 16384U
#define MIC_SEND_SAMPLES 1200U
#define VOICE_POLL_MS 200U
#define VOICE_READY_TIMEOUT_MS 10000U

extern DFSDM_Channel_HandleTypeDef hdfsdm1_channel1;

static DFSDM_Filter_HandleTypeDef mic_filter;
static DMA_HandleTypeDef mic_dma;
static int16_t mic_buffer[MIC_DMA_SAMPLES];
static int16_t mic_ring[MIC_RING_SAMPLES];
static uint8_t send_buffer[MIC_SEND_SAMPLES];
static volatile uint32_t mic_read;
static volatile uint32_t mic_write;
static volatile uint8_t mic_overrun;
static volatile uint8_t mic_error;
static uint8_t initialized;
static uint8_t attempted;
static uint8_t socket_open;
static uint8_t capturing;
static uint8_t ready;
static uint32_t started;
static uint32_t last_poll;
static uint32_t voice_session;
static uint32_t max_send_ms;
static uint32_t max_pending_samples;
static char response[64];
static uint32_t response_used;

void HAL_DFSDM_FilterMspInit(DFSDM_Filter_HandleTypeDef *filter)
{
    if (filter != &mic_filter) return;
    __HAL_RCC_DMA1_CLK_ENABLE();
    mic_dma.Instance = DMA1_Channel4;
    mic_dma.Init.Request = DMA_REQUEST_0;
    mic_dma.Init.Direction = DMA_PERIPH_TO_MEMORY;
    mic_dma.Init.PeriphInc = DMA_PINC_DISABLE;
    mic_dma.Init.MemInc = DMA_MINC_ENABLE;
    mic_dma.Init.PeriphDataAlignment = DMA_PDATAALIGN_HALFWORD;
    mic_dma.Init.MemDataAlignment = DMA_MDATAALIGN_HALFWORD;
    mic_dma.Init.Mode = DMA_CIRCULAR;
    mic_dma.Init.Priority = DMA_PRIORITY_HIGH;
    __HAL_LINKDMA(filter, hdmaReg, mic_dma);
    if (HAL_DMA_Init(&mic_dma) != HAL_OK)
    {
        mic_error = 1U;
        return;
    }
    HAL_NVIC_SetPriority(DMA1_Channel4_IRQn, 1, 0);
    HAL_NVIC_EnableIRQ(DMA1_Channel4_IRQn);
}

void DMA1_Channel4_IRQHandler(void)
{
    HAL_DMA_IRQHandler(&mic_dma);
}

static void mic_copy(uint32_t offset)
{
    uint32_t write = mic_write;
    if (write - mic_read > MIC_RING_SAMPLES - MIC_DMA_SAMPLES / 2U)
    {
        mic_overrun = 1U;
        return;
    }
    for (uint32_t i = 0; i < MIC_DMA_SAMPLES / 2U; ++i)
    {
        int32_t sample = (int32_t)mic_buffer[offset + i] * 4;
        if (sample > 32767) sample = 32767;
        if (sample < -32768) sample = -32768;
        mic_ring[(write + i) & (MIC_RING_SAMPLES - 1U)] = (int16_t)sample;
    }
    __DMB();
    mic_write = write + MIC_DMA_SAMPLES / 2U;
}

void HAL_DFSDM_FilterRegConvHalfCpltCallback(DFSDM_Filter_HandleTypeDef *filter)
{
    if (filter == &mic_filter) mic_copy(0U);
}

void HAL_DFSDM_FilterRegConvCpltCallback(DFSDM_Filter_HandleTypeDef *filter)
{
    if (filter == &mic_filter) mic_copy(MIC_DMA_SAMPLES / 2U);
}

void HAL_DFSDM_FilterErrorCallback(DFSDM_Filter_HandleTypeDef *filter)
{
    if (filter == &mic_filter) mic_error = 1U;
}

void voice_stop_init(void)
{
    if (hdfsdm1_channel1.Init.OutputClock.Divider != 50U)
    {
        printf("[VOICE] Horloge microphone invalide : divider attendu 50.\r\n");
        return;
    }
    mic_filter.Instance = DFSDM1_Filter0;
    mic_filter.Init.RegularParam.Trigger = DFSDM_FILTER_SW_TRIGGER;
    mic_filter.Init.RegularParam.FastMode = ENABLE;
    mic_filter.Init.RegularParam.DmaMode = ENABLE;
    mic_filter.Init.InjectedParam.Trigger = DFSDM_FILTER_SW_TRIGGER;
    mic_filter.Init.InjectedParam.ScanMode = DISABLE;
    mic_filter.Init.InjectedParam.DmaMode = DISABLE;
    mic_filter.Init.InjectedParam.ExtTrigger = DFSDM_FILTER_EXT_TRIG_TIM1_TRGO;
    mic_filter.Init.InjectedParam.ExtTriggerEdge = DFSDM_FILTER_EXT_TRIG_RISING_EDGE;
    mic_filter.Init.FilterParam.SincOrder = DFSDM_FILTER_SINC3_ORDER;
    mic_filter.Init.FilterParam.Oversampling = 100U;
    mic_filter.Init.FilterParam.IntOversampling = 1U;
    if (HAL_DFSDM_FilterInit(&mic_filter) != HAL_OK || mic_error ||
        HAL_DFSDM_FilterConfigRegChannel(&mic_filter, DFSDM_CHANNEL_1,
                                          DFSDM_CONTINUOUS_CONV_ON) != HAL_OK)
    {
        printf("[VOICE] Initialisation du filtre microphone impossible.\r\n");
        return;
    }
    initialized = 1U;
}

static void voice_close(void)
{
    if (capturing)
    {
        capturing = 0U;
        if (HAL_DFSDM_FilterRegularStop_DMA(&mic_filter) != HAL_OK)
        {
            printf("[VOICE] Arret du microphone impossible.\r\n");
            initialized = 0U;
        }
    }
    if (socket_open)
    {
        socket_open = 0U;
        if (WIFI_CloseClientConnection(VOICE_SOCKET) != WIFI_STATUS_OK)
            printf("[VOICE] Fermeture de la connexion impossible.\r\n");
    }
}

static void voice_fail(const char *reason)
{
    printf("[VOICE] %s\r\n", reason);
    voice_close();
}

static int voice_start(void)
{
    uint16_t port = wifi_probe_server_port();
    if (!initialized || !wifi_probe_is_ready() || port > 65533U)
    {
        printf("[VOICE] Microphone ou serveur indisponible.\r\n");
        return 0;
    }
    if (WIFI_OpenClientConnection(VOICE_SOCKET, WIFI_TCP_PROTOCOL, "VOICE",
                                  wifi_probe_server_ip(), (uint16_t)(port + 2U), 0) != WIFI_STATUS_OK)
    {
        printf("[VOICE] Connexion au serveur vocal impossible.\r\n");
        return 0;
    }
    socket_open = 1U;
    mic_read = mic_write = 0U;
    mic_error = mic_overrun = 0U;
    max_send_ms = max_pending_samples = 0U;
    ready = 0U;
    response_used = 0U;
    if (HAL_DFSDM_FilterRegularMsbStart_DMA(&mic_filter, mic_buffer, MIC_DMA_SAMPLES) != HAL_OK)
    {
        voice_fail("Demarrage du microphone impossible.");
        return 0;
    }
    capturing = 1U;
    started = last_poll = HAL_GetTick();
    printf("[VOICE] Demarrage de l'ecoute locale...\r\n");
    return 1;
}

static int voice_receive(void)
{
    uint8_t data[32];
    uint16_t received = 0U;
    if (WIFI_ReceiveData(VOICE_SOCKET, data, sizeof(data), &received, 20U) != WIFI_STATUS_OK)
    {
        voice_fail("Connexion vocale interrompue.");
        return 0;
    }
    for (uint16_t i = 0; i < received; ++i)
    {
        if (data[i] == '\n')
        {
            response[response_used] = '\0';
            if (strcmp(response, "READY") == 0)
            {
                ready = 1U;
                printf("[VOICE] Ecoute active.\r\n");
            }
            else if (strcmp(response, "PAUSE") == 0 && ready)
            {
                printf("[VOICE] Pause reconnue.\r\n");
                audio_stream_pause();
            }
            else if (strcmp(response, "PLAY") == 0 && ready)
            {
                printf("[VOICE] Play reconnu.\r\n");
                audio_stream_play();
            }
            else if (strcmp(response, "RECOMMENCE") == 0 && ready)
            {
                printf("[VOICE] Recommence reconnu.\r\n");
                audio_stream_restart();
            }
            else
            {
                printf("[VOICE] Erreur du serveur vocal : %s\r\n", response);
                voice_close();
                return 0;
            }
            response_used = 0U;
        }
        else if (response_used < sizeof(response) - 1U)
            response[response_used++] = (char)data[i];
        else
        {
            voice_fail("Reponse du serveur vocal trop longue.");
            return 0;
        }
    }
    return 1;
}

int voice_stop_needs_service(void)
{
    return socket_open && (mic_error || mic_overrun ||
           (ready && mic_write - mic_read >= MIC_SEND_SAMPLES));
}

void voice_stop_process(void)
{
    if (!audio_stream_active())
    {
        voice_close();
        attempted = 0U;
        return;
    }
    if (voice_session != audio_stream_session())
    {
        voice_close();
        attempted = 0U;
        voice_session = audio_stream_session();
    }
    if (!attempted)
    {
        attempted = 1U;
        if (!voice_start()) return;
    }
    if (!socket_open) return;
    if (mic_error)
    {
        voice_fail("Erreur DMA microphone.");
        return;
    }
    if (mic_overrun)
    {
        uint32_t mask = __get_PRIMASK();
        uint32_t pending;
        uint32_t dropped;
        __disable_irq();
        pending = mic_write - mic_read;
        dropped = pending > MIC_SEND_SAMPLES ? pending - MIC_SEND_SAMPLES : 0U;
        mic_read += dropped;
        mic_overrun = 0U;
        if (!mask) __enable_irq();
        printf("[VOICE] Tampon sature : %lu echantillons anciens ignores ; attente max=%lu, envoi max=%lu ms.\r\n",
               (unsigned long)dropped, (unsigned long)max_pending_samples, (unsigned long)max_send_ms);
    }
    if (!ready && HAL_GetTick() - started > VOICE_READY_TIMEOUT_MS)
    {
        voice_fail("Delai de demarrage du moteur vocal depasse.");
        return;
    }
    if (HAL_GetTick() - last_poll >= VOICE_POLL_MS)
    {
        last_poll = HAL_GetTick();
        if (!voice_receive()) return;
    }
    if (!ready)
    {
        /* Discard startup audio so the microphone buffer cannot fill while waiting. */
        mic_read = mic_write;
        return;
    }
    if (mic_write - mic_read >= MIC_SEND_SAMPLES)
    {
        uint16_t sent = 0U;
        uint32_t read = mic_read;
        uint32_t pending = mic_write - read;
        uint32_t send_started;
        WIFI_Status_t status;
        if (pending > max_pending_samples) max_pending_samples = pending;
        __DMB();
        for (uint32_t i = 0; i < MIC_SEND_SAMPLES; ++i)
            send_buffer[i] = (uint8_t)(((int32_t)mic_ring[(read + i) & (MIC_RING_SAMPLES - 1U)] + 32768) >> 8);
        mic_read = read + MIC_SEND_SAMPLES;
        send_started = HAL_GetTick();
        status = WIFI_SendData(VOICE_SOCKET, (const uint8_t *)send_buffer,
                               sizeof(send_buffer), &sent, 1000U);
        if (HAL_GetTick() - send_started > max_send_ms)
            max_send_ms = HAL_GetTick() - send_started;
        if (status != WIFI_STATUS_OK || sent != sizeof(send_buffer))
            voice_fail("Envoi audio au serveur vocal impossible.");
    }
}
