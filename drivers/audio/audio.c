/**
 * MIT License
 *
 * Copyright (c) 2022 Vincent Mistler
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */
#ifndef PWM_PIN0
#define PWM_PIN0 (AUDIO_PWM_PIN&0xfe)
#endif
#ifndef PWM_PIN1
#define PWM_PIN1 (PWM_PIN0+1)
#endif

#include "audio.h"

#ifdef AUDIO_PWM_PIN
#include "hardware/pwm.h"
#include "hardware/clocks.h"
#endif

/**
 * return the default i2s context used to store information about the setup
 */
i2s_config_t i2s_get_default_config(void) {
    i2s_config_t i2s_config = {
            .sample_freq = I2S_FREQUENCY,
            .channel_count = 2,
            .data_pin = 26,
            .clock_pin_base = 27,
            .pio = pio1,
            .sm = 0,
            .dma_channel = 0,
            .dma_buf = NULL,
            .dma_trans_count = 0,
            .volume = 0,
            .program_offset = 0,
            .initialized = false,
    };

    return i2s_config;
}

/**
 * Initialize the I2S driver. Must be called before calling i2s_write or i2s_dma_write
 * i2s_config: I2S context obtained by i2s_get_default_config()
 */


bool i2s_init(i2s_config_t *i2s_config) {
#ifndef AUDIO_PWM_PIN
    if (i2s_config->initialized)
        return true;

    const pio_program_t *program;
#ifdef I2S_CS4334
    program = &audio_i2s_cs4334_program;
#else
    program = &audio_i2s_program;
#endif
    if (!pio_can_add_program(i2s_config->pio, program))
        return false;

    const int sm = pio_claim_unused_sm(i2s_config->pio, false);
    if (sm < 0)
        return false;
    i2s_config->sm = (uint8_t)sm;
    i2s_config->program_offset = (uint16_t)pio_add_program(i2s_config->pio, program);

    const uint8_t func = i2s_config->pio == pio0 ? GPIO_FUNC_PIO0 : GPIO_FUNC_PIO1;
    gpio_set_function(i2s_config->data_pin, func);
    gpio_set_function(i2s_config->clock_pin_base, func);
    gpio_set_function(i2s_config->clock_pin_base + 1, func);

    uint32_t divider = clock_get_hz(clk_sys) * 4 / i2s_config->sample_freq;
#ifdef I2S_CS4334
    audio_i2s_cs4334_program_init(i2s_config->pio, i2s_config->sm,
                                  i2s_config->program_offset, i2s_config->data_pin,
                                  i2s_config->clock_pin_base);
    divider >>= 3;
#else
    audio_i2s_program_init(i2s_config->pio, i2s_config->sm,
                           i2s_config->program_offset, i2s_config->data_pin,
                           i2s_config->clock_pin_base);
#endif
    pio_sm_set_clkdiv_int_frac(i2s_config->pio, i2s_config->sm,
                               divider >> 8u, divider & 0xffu);
    pio_sm_clear_fifos(i2s_config->pio, i2s_config->sm);
    pio_sm_set_enabled(i2s_config->pio, i2s_config->sm, true);
    i2s_config->initialized = true;

    i2s_config->dma_buf = NULL;
    if (i2s_config->dma_trans_count != 0) {
        const int dma = dma_claim_unused_channel(false);
        if (dma < 0) {
            i2s_deinit(i2s_config);
            return false;
        }
        i2s_config->dma_channel = (uint8_t)dma;
        i2s_config->dma_buf = malloc(i2s_config->dma_trans_count * sizeof(uint32_t));
        if (!i2s_config->dma_buf) {
            dma_channel_unclaim(i2s_config->dma_channel);
            i2s_config->dma_channel = 0;
            i2s_deinit(i2s_config);
            return false;
        }

        dma_channel_config dma_config = dma_channel_get_default_config(i2s_config->dma_channel);
        channel_config_set_read_increment(&dma_config, true);
        channel_config_set_write_increment(&dma_config, false);
        channel_config_set_transfer_data_size(&dma_config, DMA_SIZE_32);
        channel_config_set_dreq(&dma_config,
                                pio_get_dreq(i2s_config->pio, i2s_config->sm, true));
        dma_channel_configure(i2s_config->dma_channel, &dma_config,
                              &i2s_config->pio->txf[i2s_config->sm],
                              i2s_config->dma_buf, i2s_config->dma_trans_count, false);
    }

    return true;
#else
    return false;
#endif
}

void i2s_deinit(i2s_config_t *i2s_config) {
#ifndef AUDIO_PWM_PIN
    if (!i2s_config->initialized && !i2s_config->dma_buf)
        return;

    if (i2s_config->dma_buf) {
        dma_channel_abort(i2s_config->dma_channel);
        dma_channel_unclaim(i2s_config->dma_channel);
        free(i2s_config->dma_buf);
        i2s_config->dma_buf = NULL;
    }
    if (i2s_config->initialized) {
        pio_sm_set_enabled(i2s_config->pio, i2s_config->sm, false);
        pio_sm_clear_fifos(i2s_config->pio, i2s_config->sm);
#ifdef I2S_CS4334
        pio_remove_program(i2s_config->pio, &audio_i2s_cs4334_program,
                           i2s_config->program_offset);
#else
        pio_remove_program(i2s_config->pio, &audio_i2s_program,
                           i2s_config->program_offset);
#endif
        pio_sm_unclaim(i2s_config->pio, i2s_config->sm);
        gpio_deinit(i2s_config->data_pin);
        gpio_deinit(i2s_config->clock_pin_base);
        gpio_deinit(i2s_config->clock_pin_base + 1);
    }
    i2s_config->initialized = false;
#endif
}

/**
 * Write samples to I2S directly and wait for completion (blocking)
 * i2s_config: I2S context obtained by i2s_get_default_config()
 *     sample: pointer to an array of len x 32 bits samples
 *             Each 32 bits sample contains 2x16 bits samples, 
 *             one for the left channel and one for the right channel
 *        len: length of sample in 32 bits words
 */
void i2s_write(const i2s_config_t *i2s_config, const int16_t *samples, const size_t len) {
    for (size_t i = 0; i < len; i++) {
        pio_sm_put_blocking(i2s_config->pio, i2s_config->sm, (uint32_t) samples[i]);
    }
}

/**
 * Write samples to DMA buffer and initiate DMA transfer (non blocking)
 * i2s_config: I2S context obtained by i2s_get_default_config()
 *     sample: pointer to an array of dma_trans_count x 32 bits samples
 */
void i2s_dma_write(i2s_config_t *i2s_config, const uint16_t *samples) {
    /* Wait the completion of the previous DMA transfer */
    dma_channel_wait_for_finish_blocking(i2s_config->dma_channel);
    /* Copy samples into the DMA buffer */
#ifdef AUDIO_PWM_PIN
    for(uint16_t i=0;i<i2s_config->dma_trans_count*2;i++) {
           
            i2s_config->dma_buf[i] = (65536/2+(samples[i]))>>(3+i2s_config->volume);

        }
#else
    if (i2s_config->volume == 0) {
        memcpy(i2s_config->dma_buf, samples, i2s_config->dma_trans_count * sizeof(int32_t));
    } else {
        for (uint16_t i = 0; i < i2s_config->dma_trans_count * 2; i++) {
            i2s_config->dma_buf[i] = samples[i] >> i2s_config->volume;
        }
    }
#endif
    /* Initiate the DMA transfer */
    dma_channel_transfer_from_buffer_now(i2s_config->dma_channel,
                                         i2s_config->dma_buf,
                                         i2s_config->dma_trans_count);
}

/**
 * Adjust the output volume
 * i2s_config: I2S context obtained by i2s_get_default_config()
 *     volume: desired volume between 0 (highest. volume) and 16 (lowest volume)
 */
void i2s_volume(i2s_config_t *i2s_config, uint8_t volume) {
    if (volume > 16) volume = 16;
    i2s_config->volume = volume;
}

/**
 * Increases the output volume
 */
void i2s_increase_volume(i2s_config_t *i2s_config) {
    if (i2s_config->volume > 0) {
        i2s_config->volume--;
    }
}

/**
 * Decreases the output volume
 */
void i2s_decrease_volume(i2s_config_t *i2s_config) {
    if (i2s_config->volume < 16) {
        i2s_config->volume++;
    }
}
