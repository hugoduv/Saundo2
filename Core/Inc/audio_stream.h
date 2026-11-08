#ifndef AUDIO_STREAM_H
#define AUDIO_STREAM_H

#include "stm32l4xx_hal.h"

void audio_stream_tag_placed(const char *uid);
void audio_stream_tag_removed(void);
void audio_stream_pause(void);
void audio_stream_play(void);
void audio_stream_restart(void);
int audio_stream_active(void);
uint32_t audio_stream_session(void);
void audio_stream_process(void);
void audio_stream_dma_half(DAC_HandleTypeDef *dac);
void audio_stream_dma_complete(DAC_HandleTypeDef *dac);
void audio_stream_dma_error(DAC_HandleTypeDef *dac);

#endif
