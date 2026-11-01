file(GLOB FONT_SRCS "fonts/*.c")
# Extra fonts from Tab5 Studio builds (studio/*.ttf); none in a normal build
file(GLOB STUDIO_TTF "studio/*.ttf")
file(GLOB STUDIO_BIN "studio/*.bin")  # images of a Studio scene

idf_component_register(SRCS "main.cpp" "ui.cpp" "wifi.cpp" "weather.cpp" "rx8130.cpp" "img_logo.c" "img_splash.c" "settings.cpp" "config.cpp" "web.cpp" "sdcard.cpp" "files.cpp" "layout.cpp" "livescene.cpp" "lv_mem_psram.c" "uiwatch.cpp" "audio.cpp" "alarm.cpp" ${FONT_SRCS}
                       INCLUDE_DIRS "."
                       EMBED_TXTFILES "web/index.html"
                       EMBED_FILES "ttf/inter.ttf" "ttf/inter_light.ttf" "ttf/orbitron.ttf" "ttf/roboto_mono.ttf" "ttf/bebas.ttf" "ttf/oswald.ttf" "ttf/rajdhani.ttf" "ttf/icons.ttf" ${STUDIO_TTF} ${STUDIO_BIN})

# lv_mem_psram.c provides the LVGL allocator (lv_malloc_core ...); LVGL is linked after main, so pull it in
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u lv_mem_init")
