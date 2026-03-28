set(srcs
    "anna_cfg_storage.c"
    "anna_cfg_parse.c"
    "anna_cfg_defaults.c"
    "anna_state_storage.c"
    "anna_cfg_matter_bindings.cpp"   # esp-matter 연동은 C++
)

idf_component_register(
    SRCS            ${srcs}
    INCLUDE_DIRS    "include"
    PRIV_INCLUDE_DIRS "."              # private_internals.h 등
    REQUIRES        json nvs_flash esp_matter
)

# esp-matter가 C++을 요구 → C++ 표준 지정
set_property(TARGET ${COMPONENT_LIB} PROPERTY CXX_STANDARD 17)
