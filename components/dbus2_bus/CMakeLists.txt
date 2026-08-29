idf_component_register(
    SRCS "src/Manager.cpp"
    INCLUDE_DIRS "include"
    REQUIRES dbus2_proto esp_driver_uart esp_timer
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_20)
