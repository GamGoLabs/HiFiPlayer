if(NOT TARGET oboe::oboe)
add_library(oboe::oboe SHARED IMPORTED)
set_target_properties(oboe::oboe PROPERTIES
    IMPORTED_LOCATION "C:/Users/ingsb/.gradle/caches/8.9/transforms/e6fde8ad6dd13e13dc38df3d6592def9/transformed/oboe-1.9.3/prefab/modules/oboe/libs/android.x86_64/liboboe.so"
    INTERFACE_INCLUDE_DIRECTORIES "C:/Users/ingsb/.gradle/caches/8.9/transforms/e6fde8ad6dd13e13dc38df3d6592def9/transformed/oboe-1.9.3/prefab/modules/oboe/include"
    INTERFACE_LINK_LIBRARIES ""
)
endif()

