#include <stdio.h>
#include <sys/time.h>
#include <time.h>
#include "appfs.h"
#include "badgelink.h"
#include "bsp/device.h"
#include "bsp/display.h"
#include "bsp/input.h"
#include "bsp/led.h"
#include "bsp/power.h"
#include "bsp/rtc.h"
#include "common/display.h"
#include "custom_certificates.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_types.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/idf_additions.h"
#include "gui_footer.h"
#include "gui_menu.h"
#include "gui_style.h"
#include "hal/lcd_types.h"
#include "nvs_flash.h"
#include "pax_fonts.h"
#include "pax_gfx.h"
#include "pax_text.h"
#include "portmacro.h"
#include "sdcard.h"
#include "sdkconfig.h"
#include "wifi_connection.h"
#include "wifi_remote.h"
#include "xtulator.h"

// Constants
static char const TAG[] = "main";

// Global variables
static QueueHandle_t input_event_queue      = NULL;
static wl_handle_t   wl_handle              = WL_INVALID_HANDLE;
static bool          wifi_stack_initialized = false;
static bool          wifi_stack_task_done   = false;

gui_theme_t theme = {
    .palette =
        {
            .color_foreground          = 0xFF340132,  // #340132
            .color_background          = 0xFFEEEAEE,  // #EEEAEE
            .color_active_foreground   = 0xFF340132,  // #340132
            .color_active_background   = 0xFFFFFFFF,  // #FFFFFF
            .color_highlight_primary   = 0xFF01BC99,  // #01BC99
            .color_highlight_secondary = 0xFFFFCF53,  // #FFCF53
            .color_highlight_tertiary  = 0xFFFF017F,  // #FF017F
        },
    .footer =
        {
            .height             = 32,
            .vertical_margin    = 7,
            .horizontal_margin  = 20,
            .text_height        = 16,
            .vertical_padding   = 20,
            .horizontal_padding = 0,
            .text_font          = pax_font_sky_mono,
        },
    .header =
        {
            .height             = 32,
            .vertical_margin    = 7,
            .horizontal_margin  = 20,
            .text_height        = 16,
            .vertical_padding   = 20,
            .horizontal_padding = 0,
            .text_font          = pax_font_sky_mono,
        },
    .menu =
        {
            .height                = 480 - 64,
            .vertical_margin       = 20,
            .horizontal_margin     = 30,
            .text_height           = 16,
            .vertical_padding      = 6,
            .horizontal_padding    = 6,
            .text_font             = pax_font_sky_mono,
            .list_entry_height     = 32,
            .grid_horizontal_count = 4,
            .grid_vertical_count   = 3,
        },
};

void startup_screen(const char* text) {
    pax_buf_t* fb = display_get_buffer();
    pax_background(fb, theme.palette.color_background);
    gui_render_header_adv(fb, &theme, ((gui_header_field_t[]){{NULL, (char*)text}}), 1, NULL, 0);
    gui_render_footer_adv(fb, &theme, NULL, 0, NULL, 0);
    display_blit_buffer(fb);
}

bool wifi_stack_get_initialized(void) {
    return wifi_stack_initialized;
}

bool wifi_stack_get_task_done(void) {
    return wifi_stack_task_done;
}

static void wifi_task(void* pvParameters) {
    if (wifi_remote_initialize() == ESP_OK) {
        wifi_connection_init_stack();
        wifi_stack_initialized = true;
        wifi_connect_try_all();
    } else {
        bsp_power_set_radio_state(BSP_POWER_RADIO_STATE_OFF);
        ESP_LOGE(TAG, "WiFi radio not responding, did you flash ESP-HOSTED firmware?");
    }
    wifi_stack_task_done = true;
    vTaskDelete(NULL);
}

static void xtulator_task(void* pvParameters) {
    xtulator_main();
    vTaskDelete(NULL);
}

void app_main(void) {
    // Initialize the Non Volatile Storage service
    esp_err_t res = nvs_flash_init();
    if (res == ESP_ERR_NVS_NO_FREE_PAGES || res == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        res = nvs_flash_init();
    }
    ESP_ERROR_CHECK(res);

    // Initialize the Board Support Package
    esp_err_t bsp_init_result = bsp_device_initialize();

    if (bsp_init_result == ESP_OK) {
        ESP_ERROR_CHECK(bsp_input_get_queue(&input_event_queue));
        bsp_display_set_backlight_brightness(100);
        display_init();
    } else if (bsp_device_get_initialized_without_coprocessor()) {
        display_init();
        pax_buf_t* buffer = display_get_buffer();
        pax_background(buffer, 0xFFFFFF00);
        pax_draw_text(buffer, 0xFF000000, pax_font_sky_mono, 16, 0, 0, "Device started without coprocessor!");
        display_blit_buffer(buffer);
        vTaskDelay(pdMS_TO_TICKS(2000));
    } else {
        ESP_LOGE(TAG, "Failed to initialize BSP, bailing out.");
        return;
    }

    bsp_led_initialize();

    startup_screen("Mounting FAT filesystem...");

    esp_vfs_fat_mount_config_t fat_mount_config = {
        .format_if_mount_failed   = false,
        .max_files                = 10,
        .allocation_unit_size     = CONFIG_WL_SECTOR_SIZE,
        .disk_status_check_enable = false,
        .use_one_fat              = false,
    };

    res = esp_vfs_fat_spiflash_mount_rw_wl("/int", "locfd", &fat_mount_config, &wl_handle);
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount FAT filesystem: %s", esp_err_to_name(res));
        startup_screen("Error: Failed to mount FAT filesystem");
        pax_buf_t* buffer = display_get_buffer();
        pax_background(buffer, 0xFFFF0000);
        pax_draw_text(buffer, 0xFFFFFFFF, pax_font_sky_mono, 16, 0, 0, "Failed to initialize FAT filesystem");
        display_blit_buffer(buffer);
        return;
    }

    bsp_rtc_update_time();

    bool sdcard_inserted = false;
    bsp_input_read_action(BSP_INPUT_ACTION_TYPE_SD_CARD, &sdcard_inserted);

    /*if (sdcard_inserted) {
        printf("SD card detected\r\n");
#if defined(CONFIG_BSP_TARGET_TANMATSU) || defined(CONFIG_BSP_TARGET_KONSOOL) || \
    defined(CONFIG_BSP_TARGET_HACKERHOTEL_2026)*/
    sd_pwr_ctrl_handle_t sd_pwr_handle = initialize_sd_ldo();
    res                                = sd_mount_spi(sd_pwr_handle);

    if (res != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount SD card: %s", esp_err_to_name(res));
        pax_buf_t* buffer = display_get_buffer();
        pax_background(buffer, 0xFFFF0000);
        pax_draw_text(buffer, 0xFFFFFFFF, pax_font_sky_mono, 16, 0, 0, "Failed to initialize SD card");
        display_blit_buffer(buffer);
        return;
    } else {
        startup_screen("SD card mounted successfully");
    }
    /*#endif
        }*/

    ESP_ERROR_CHECK(initialize_custom_ca_store());

    // xTaskCreatePinnedToCore(wifi_task, TAG, 4096, NULL, 10, NULL, 1);

    pax_buf_t* buffer = display_get_buffer();
    pax_background(buffer, 0xFF000000);
    display_blit();

    bsp_input_set_backlight_brightness(100);

    xTaskCreatePinnedToCore(xtulator_task, TAG, 4096 * 4, NULL, 10, NULL, 1);
}
