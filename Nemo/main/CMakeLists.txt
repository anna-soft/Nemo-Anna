# idf_component_register(SRCS "anna_device_instance_info_provider.cpp" "anna_commissionable_data_provider.cpp" "host_serial_rx.cpp" "app_settings.cpp" "app_main.cpp" "app_driver.cpp"
#                           
#                           
#                            "../../../components/anna_cfg/anna_cfg.cpp"
#                       INCLUDE_DIRS "."
#                       REQUIRES "esp_matter" "led_controller" "anna_cfg")

# if (CONFIG_ENABLE_SET_CERT_DECLARATION_API)
#     target_add_binary_data(${COMPONENT_TARGET} "certification_declaration/certification_declaration.der" BINARY)
# endif()

# set_property(TARGET ${COMPONENT_LIB} PROPERTY CXX_STANDARD 17)
# target_compile_options(${COMPONENT_LIB} PRIVATE "-DCHIP_HAVE_CONFIG_H")

idf_component_register(
    SRC_DIRS          
        "." 
    PRIV_INCLUDE_DIRS 
        "." 
        "${ESP_MATTER_PATH}/examples/common/utils" 

    REQUIRES esp_matter anna_cfg nvs_flash led_controller mode_manager esp_adc json mbedtls
)

if (CONFIG_ENABLE_SET_CERT_DECLARATION_API)
    target_add_binary_data(${COMPONENT_TARGET} "certification_declaration/certification_declaration.der" BINARY)
endif()

set_property(TARGET ${COMPONENT_LIB} PROPERTY CXX_STANDARD 17)
target_compile_options(${COMPONENT_LIB} PRIVATE "-DCHIP_HAVE_CONFIG_H")

