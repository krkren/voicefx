# The voicefx plugin. Built by the engine when this folder is in TVP_PLUGIN_FOLDERS (listed
# after krkrz_dev's plugins, whose tp_stub and ncbind it uses), or on its own through the
# CMakeLists.txt at the top of this repository.
set(TPSTUB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../tp_stub CACHE PATH "Path to tp_stub directory")
include(${TPSTUB_DIR}/krkrz.cmake)

krkrz_plugin(voicefx NCBIND
    SOURCES
        main.cpp
        dsp.cpp
)
target_compile_features(voicefx PRIVATE cxx_std_17)
