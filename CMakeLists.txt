cmake_minimum_required(VERSION 3.16)

# Ethernet-only build; exclude Wi-Fi remote/hosted components.
set(EXCLUDE_COMPONENTS
    esp_hosted
    esp_wifi_remote
    wifi_remote_over_at
    wifi_remote_over_eppp
)
set(EXTRA_COMPONENT_DIRS
    /home/bjorn/esp/esp-video-components/esp_video
    /home/bjorn/esp/esp-video-components/esp_cam_sensor
    /home/bjorn/esp/esp-video-components/esp_ipa
    /home/bjorn/esp/esp-video-components/esp_sccb_intf
)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(p4_mqtt_cam)
