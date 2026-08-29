idf_component_register(
    SRCS "main.cpp" "indicator.cpp" "recovery.cpp" "appliance.cpp" "diagnostics.cpp"
    INCLUDE_DIRS "."
    REQUIRES driver esp_app_format app_update spi_flash espcoredump esp_partition app_config dbus2_bus dbus2_proto dbus2_decoder app_config app_net app_web app_profiles app_store app_mqtt
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_20)
