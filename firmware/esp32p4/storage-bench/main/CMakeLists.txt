idf_component_register(SRCS bench_main.cpp REQUIRES aw esp_psram esp_timer)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-pedantic -Wno-old-style-cast)
