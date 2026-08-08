set(OPENALAUDIODEVICE_SRC
    mss/mss.h
    OpenAL3DSample.cpp
    OpenALAudioInternal.h
    OpenALDriver.cpp
    OpenALSample.cpp
    OpenALStream.cpp
    OpenALWaveFile.cpp
)

add_library(core_openalaudiodevice STATIC)

target_sources(core_openalaudiodevice PRIVATE ${OPENALAUDIODEVICE_SRC})

# WWAudio includes "mss.h", GameEngineDevice includes "mss/mss.h"; both must resolve.
target_include_directories(core_openalaudiodevice PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/mss
)

target_link_libraries(core_openalaudiodevice PUBLIC
    OpenAL::OpenAL
)

set_target_properties(core_openalaudiodevice PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
