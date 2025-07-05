/*
  XTulator: A portable, open-source 80186 PC emulator.
  Copyright (C)2020 Mike Chambers

  This program is free software; you can redistribute it and/or
  modify it under the terms of the GNU General Public License
  as published by the Free Software Foundation; either version 2
  of the License, or (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

#include "sdlconsole.h"
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "../../config.h"
#include "../../timing.h"
#include "../input/mouse.h"
#include "../input/sdlkeys.h"
#include "bsp/display.h"
#include "bsp/input.h"
#include "common/display.h"
#include "driver/ppa.h"
#include "esp_lcd_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "hal/lcd_types.h"
#include "hal/ppa_types.h"
#include "pax_gfx.h"

static QueueHandle_t keyboard_event_queue = NULL;

uint64_t sdlconsole_frameTime[30];
uint32_t sdlconsole_keyTimer;
uint8_t  sdlconsole_curkey, sdlconsole_lastKey, sdlconsole_frameIdx = 0, sdlconsole_grabbed = 0, sdlconsole_ctrl = 0,
                                               sdlconsole_alt = 0, sdlconsole_doRepeat = 0;
int sdlconsole_curw, sdlconsole_curh;

char* sdlconsole_title;

ppa_client_handle_t ppa_srm_handle  = NULL;
ppa_client_handle_t ppa_fill_handle = NULL;

void sdlconsole_keyRepeat(void* dummy) {
    sdlconsole_doRepeat = 1;
    timing_updateIntervalFreq(sdlconsole_keyTimer, 15);
}

pax_col_t* vga_change_buffer = NULL;

int sdlconsole_init(char* title) {
    sdlconsole_title = title;

    sdlconsole_keyTimer = timing_addTimer(sdlconsole_keyRepeat, NULL, 2, TIMING_DISABLED);

    bsp_input_get_queue(&keyboard_event_queue);

    vga_change_buffer = calloc(1024 * 1024, sizeof(uint32_t));

    printf("Register PPA client for SRM operation\r\n");
    ppa_client_config_t ppa_srm_config = {
        .oper_type             = PPA_OPERATION_SRM,
        .max_pending_trans_num = 1,
    };
    ESP_ERROR_CHECK(ppa_register_client(&ppa_srm_config, &ppa_srm_handle));
    ppa_client_config_t ppa_fill_config = {
        .oper_type             = PPA_OPERATION_FILL,
        .max_pending_trans_num = 1,
    };
    ESP_ERROR_CHECK(ppa_register_client(&ppa_fill_config, &ppa_fill_handle));

    return 0;
}

int sdlconsole_setWindow(int w, int h) {
    return 0;
}

void sdlconsole_setTitle(char* title) {  // appends something to the main title, doesn't replace it all
}

void sdlconsole_blit(uint32_t* pixels, int w, int h, int stride) {
    static uint64_t lasttime = 0;
    uint64_t        curtime;
    curtime = timing_getCur();

    pax_buf_t* fb     = display_get_buffer();
    uint8_t*   raw_fb = (uint8_t*)pax_buf_get_pixels(fb);

    ppa_srm_oper_config_t ppa_active_config = {0};

    ppa_active_config.in.buffer         = (uint8_t*)pixels;
    ppa_active_config.in.pic_w          = stride / sizeof(uint32_t);
    ppa_active_config.in.pic_h          = h;
    ppa_active_config.in.block_w        = w;
    ppa_active_config.in.block_h        = h;
    ppa_active_config.in.block_offset_x = 0;
    ppa_active_config.in.block_offset_y = 0;
    ppa_active_config.in.srm_cm         = PPA_SRM_COLOR_MODE_ARGB8888;

    ppa_active_config.alpha_update_mode = PPA_ALPHA_FIX_VALUE;
    ppa_active_config.alpha_fix_val     = 0xFF;

    ppa_active_config.out.buffer         = raw_fb;
    ppa_active_config.out.buffer_size    = 480 * 800 * 2;
    ppa_active_config.out.pic_w          = 480;
    ppa_active_config.out.pic_h          = 800;
    ppa_active_config.out.block_offset_x = 0;
    ppa_active_config.out.block_offset_y = 0;
    ppa_active_config.out.srm_cm         = PPA_SRM_COLOR_MODE_RGB565;

    ppa_active_config.scale_x        = 1.0;
    ppa_active_config.scale_y        = 1.0;
    ppa_active_config.rotation_angle = PPA_SRM_ROTATION_ANGLE_270;

    ppa_active_config.mode = PPA_TRANS_MODE_NON_BLOCKING;

    ppa_do_scale_rotate_mirror(ppa_srm_handle, &ppa_active_config);

    /*for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            uint32_t  position = y * (stride / sizeof(uint32_t)) + x;
            pax_col_t color    = pixels[position];
            if (vga_change_buffer[position] != color) {
                vga_change_buffer[position] = color;
                pax_set_pixel(fb, color, x, y);
            }
        }
    }*/

    display_blit();

    vPortYield();

    lasttime = curtime;
}

void sdlconsole_mousegrab() {
    sdlconsole_ctrl = sdlconsole_alt = 0;
    sdlconsole_grabbed               = 0;
}

int sdlconsole_loop() {
    if (sdlconsole_doRepeat) {
        sdlconsole_doRepeat = 0;
        sdlconsole_curkey   = sdlconsole_lastKey;
        return SDLCONSOLE_EVENT_KEY;
    }

    bsp_input_event_t event;
    if (xQueueReceive(keyboard_event_queue, &event, 0) == pdTRUE) {
        if (event.type == INPUT_EVENT_TYPE_SCANCODE) {
            sdlconsole_curkey = event.args_scancode.scancode;

            if (sdlconsole_curkey == 0x00) {
                return SDLCONSOLE_EVENT_NONE;
            }

            if ((sdlconsole_curkey & 0x7F) == sdlconsole_lastKey) {
                timing_timerDisable(sdlconsole_keyTimer);
            } else {
                sdlconsole_lastKey = sdlconsole_curkey;
                timing_updateIntervalFreq(sdlconsole_keyTimer, 2);
                timing_timerEnable(sdlconsole_keyTimer);
            }

            if (sdlconsole_curkey != 0x80) {
                return SDLCONSOLE_EVENT_KEY;
            }
        }
    }
    return SDLCONSOLE_EVENT_NONE;
}

uint8_t sdlconsole_getScancode() {
    // debug_log(DEBUG_DETAIL, "curkey: %02X\r\n", sdlconsole_curkey);
    return sdlconsole_curkey;
}
