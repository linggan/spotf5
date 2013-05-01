project(spotf5)

find_library(MATHLIB m)
find_package (JPEG REQUIRED)

set(SPOT_F5_SRC 
    strlcat.c dct.c jutil.c newcommon.c f5.c spotf5.c)

add_executable(spotf5 ${SPOT_F5_SRC})
target_link_libraries(spotf5 ${MATHLIB} ${JPEG_LIBRARIES})


install(TARGETS spotf5 RUNTIME DESTINATION bin)
