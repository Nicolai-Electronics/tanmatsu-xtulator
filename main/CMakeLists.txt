file(GLOB_RECURSE xtulator_sources
    xtulator/*.c
    xtulator/chipset/*.c
    xtulator/cpu/cpu.c
    xtulator/modules/audio/*.c
    xtulator/modules/disk/*.c
    xtulator/modules/input/*.c
    xtulator/modules/io/*.c
    xtulator/modules/video/*.c
)

if (CONFIG_IDF_TARGET_ESP32P4)
list(APPEND extra_requires esp-hosted-tanmatsu)
endif()

idf_component_register(
	SRCS
		"main.c"
		"common/display.c"
		"wifi_remote.c"
		"sdcard.c"
		${xtulator_sources}
	PRIV_REQUIRES
		esp_lcd
		fatfs
		nvs_flash
		badge-bsp
		gui
		wpa_supplicant
		custom-certificates
		wifi-manager
		esp-tls
		pax-gfx
		pax-codecs
		json
		freertos
		esp_driver_ppa
		${extra_requires}
	INCLUDE_DIRS
		"."
		xtulator
		xtulator/chipset
		xtulator/cpu
		xtulator/modules/audio
		xtulator/modules/disk
		xtulator/modules/input
		xtulator/modules/io
		xtulator/modules/video
	EMBED_FILES
		${files}
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error=parentheses -Wno-error=maybe-uninitialized -Wno-error=unused-but-set-variable)
