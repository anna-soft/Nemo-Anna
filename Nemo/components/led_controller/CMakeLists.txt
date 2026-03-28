# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-guides/build-system.html

idf_component_register(SRCS      "led_controller.c"
                      INCLUDE_DIRS "include"
                      PRIV_REQUIRES "driver" "freertos" "log" "espressif__led_strip") 