
include_directories(../include)

#
# CcCopyRead
#
list(APPEND CCCOPYREAD_DRV_SOURCE
    ../kmtest_drv/kmtest_standalone.c
    CcCopyRead_drv.c)

add_library(cccopyread_drv SHARED ${CCCOPYREAD_DRV_SOURCE})
set_module_type(cccopyread_drv kernelmodedriver)
target_link_libraries(cccopyread_drv kmtest_printf ${PSEH_LIB})
add_importlibs(cccopyread_drv ntoskrnl hal)
add_target_compile_definitions(cccopyread_drv KMT_STANDALONE_DRIVER)
#add_pch(cccopyread_drv ../include/kmt_test.h)
add_rostests_file(TARGET cccopyread_drv)

#
# CcMapData
#
list(APPEND CCMAPDATA_DRV_SOURCE
    ../kmtest_drv/kmtest_standalone.c
    CcMapData_drv.c)

add_library(ccmapdata_drv SHARED ${CCMAPDATA_DRV_SOURCE})
set_module_type(ccmapdata_drv kernelmodedriver)
target_link_libraries(ccmapdata_drv kmtest_printf ${PSEH_LIB})
add_importlibs(ccmapdata_drv ntoskrnl hal)
add_target_compile_definitions(ccmapdata_drv KMT_STANDALONE_DRIVER)
#add_pch(ccmapdata_drv ../include/kmt_test.h)
add_rostests_file(TARGET ccmapdata_drv)
