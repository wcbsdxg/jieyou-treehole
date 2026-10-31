# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

set(PROJECT_VER "0.9.7")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(xiaozhi)

# esp-sr 1.9.x 把库目录写成一个含空格的 "-L path" 参数，GCC 响应文件会把它拆开。
# project() 返回后组件目标已经完整，删掉错误参数并用 CMake 的目录 API 重新添加。
# 注意: 组件被裁剪掉时属性为空, 必须先判空, 否则 target_link_directories 会在配置期报错。
idf_component_get_property(esp_sr_lib espressif__esp-sr COMPONENT_LIB)
idf_component_get_property(esp_sr_dir espressif__esp-sr COMPONENT_DIR)
if(esp_sr_lib AND esp_sr_dir)
    foreach(link_property LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
        get_target_property(esp_sr_links ${esp_sr_lib} ${link_property})
        if(esp_sr_links)
            set(fixed_esp_sr_links "")
            foreach(esp_sr_link IN LISTS esp_sr_links)
                if(NOT esp_sr_link MATCHES "^-L ")
                    list(APPEND fixed_esp_sr_links "${esp_sr_link}")
                endif()
            endforeach()
            set_property(TARGET ${esp_sr_lib} PROPERTY ${link_property} "${fixed_esp_sr_links}")
        endif()
    endforeach()
    # 库目录按当前目标芯片拼(本工程固定 esp32s3; 用变量是为了换芯片不用手改)
    target_link_directories(${esp_sr_lib} PUBLIC
        "${esp_sr_dir}/lib/${IDF_TARGET}"
        "${esp_sr_dir}/esp-tts/esp_tts_chinese/${IDF_TARGET}")
else()
    message(WARNING "esp-sr 组件不存在, 跳过库目录修复(不影响本工程其它部分)")
endif()
