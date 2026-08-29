# The decoding core is deliberately free of JSON and ESP-IDF: it operates on
# already-parsed structures, so the whole path from frame to value can be
# replayed on a host under sanitizers without pulling in a parser or a network
# stack. Only ProfileParser needs cJSON.

set(DBUS2_DECODER_CORE
    ${CMAKE_CURRENT_LIST_DIR}/src/Value.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/Extract.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/Transform.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/Engine.cpp
)
set(DBUS2_DECODER_PARSER ${CMAKE_CURRENT_LIST_DIR}/src/ProfileParser.cpp)

if(ESP_PLATFORM)
    idf_component_register(
        SRCS ${DBUS2_DECODER_CORE} ${DBUS2_DECODER_PARSER}
        INCLUDE_DIRS include
        REQUIRES espressif__cjson
    )
    target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_20)
else()
    add_library(dbus2_decoder STATIC ${DBUS2_DECODER_CORE})
    target_include_directories(dbus2_decoder PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include)
    target_compile_features(dbus2_decoder PUBLIC cxx_std_20)

    # The parser needs cJSON. On a host it is only available once a device build
    # has fetched the managed component, so the parser tests are optional rather
    # than a reason for the whole suite to fail on a clean checkout.
    set(CJSON_DIR ${CMAKE_CURRENT_LIST_DIR}/../../managed_components/espressif__cjson/cJSON)
    if(EXISTS ${CJSON_DIR}/cJSON.c)
        add_library(dbus2_decoder_parser STATIC ${DBUS2_DECODER_PARSER} ${CJSON_DIR}/cJSON.c)
        target_include_directories(dbus2_decoder_parser PUBLIC
            ${CMAKE_CURRENT_LIST_DIR}/include ${CJSON_DIR})
        target_link_libraries(dbus2_decoder_parser PUBLIC dbus2_decoder)
        target_compile_features(dbus2_decoder_parser PUBLIC cxx_std_20)
        set(DBUS2_HAVE_PARSER TRUE PARENT_SCOPE)
    else()
        message(STATUS "cJSON not found -- profile parser tests skipped. "
                       "Run a device build once to fetch it.")
    endif()
endif()
