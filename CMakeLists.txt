# Phonics Cards - M5Stack PaperColor (C151, ESP32-S3)
cmake_minimum_required(VERSION 3.16)

set(PROJECT_VER "0.1.0")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(phonics_cards)

# ---------------------------------------------------------------------------
# Per-component optimisation split. Both halves of this were found the hard
# way on real hardware:
#
#  * M5GFX / everything else at -O2 (see sdkconfig.defaults): its per-pixel
#    framebuffer templates collapse without inlining. At -Og one fillScreen()
#    of the 400x600 RGB888 framebuffer measured ~17 s.
#
#  * M5Unified at -Og: it sizes its speaker task's stack as
#    1280 + dma_buf_len*4 bytes (max 5376). At -O2 the compiler inlines
#    _setup_i2s()'s clock arithmetic into spk_task, the frame no longer fits,
#    and the board panics on boot with
#        "***ERROR*** A stack overflow in task spk_task has been detected".
#    -Og keeps those callees out of line, so the frame stays small.
#
# Nothing in this project's own code depends on either setting.
# ---------------------------------------------------------------------------
if(TARGET __idf_m5stack__m5unified)
    target_compile_options(__idf_m5stack__m5unified PRIVATE -Og)
    message(STATUS "m5unified: forcing -Og (spk_task stack headroom)")
endif()
