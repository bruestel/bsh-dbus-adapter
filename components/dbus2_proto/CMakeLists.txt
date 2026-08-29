# Pure protocol logic: no ESP-IDF, no FreeRTOS, no UART. Buildable both as an
# ESP-IDF component and as a plain host library for the unit tests.

file(GLOB DBUS2_PROTO_SRCS ${CMAKE_CURRENT_LIST_DIR}/src/*.cpp)

if(ESP_PLATFORM)
    idf_component_register(SRCS ${DBUS2_PROTO_SRCS} INCLUDE_DIRS include)
    target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_20)
else()
    add_library(dbus2_proto STATIC ${DBUS2_PROTO_SRCS})
    target_include_directories(dbus2_proto PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include)
    target_compile_features(dbus2_proto PUBLIC cxx_std_20)
endif()
