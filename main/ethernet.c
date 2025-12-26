#include "ethernet.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include "esp_eth.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "sdkconfig.h"

static const char *TAG = "eth";

static EventGroupHandle_t s_eth_event_group;

#define ETH_CONNECTED_BIT BIT0
#define ETH_FAIL_BIT      BIT1

static void eth_event_handler(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data)
{
    esp_eth_handle_t eth_handle = *(esp_eth_handle_t *)arg;

    switch (event_id) {
    case ETHERNET_EVENT_CONNECTED: {
        uint8_t mac_addr[6];
        esp_eth_ioctl(eth_handle, ETH_CMD_G_MAC_ADDR, mac_addr);
        ESP_LOGI(TAG, "Ethernet link up %02x:%02x:%02x:%02x:%02x:%02x",
                 mac_addr[0], mac_addr[1], mac_addr[2],
                 mac_addr[3], mac_addr[4], mac_addr[5]);
        break;
    }
    case ETHERNET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "Ethernet link down");
        xEventGroupSetBits(s_eth_event_group, ETH_FAIL_BIT);
        break;
    case ETHERNET_EVENT_START:
        ESP_LOGI(TAG, "Ethernet started");
        break;
    case ETHERNET_EVENT_STOP:
        ESP_LOGI(TAG, "Ethernet stopped");
        xEventGroupSetBits(s_eth_event_group, ETH_FAIL_BIT);
        break;
    default:
        break;
    }
}

static void got_ip_event_handler(void *arg, esp_event_base_t event_base,
                                 int32_t event_id, void *event_data)
{
    const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)event_data;
    if (event) {
        ESP_LOGI(TAG, "ETHIP:" IPSTR, IP2STR(&event->ip_info.ip));
    }
    xEventGroupSetBits(s_eth_event_group, ETH_CONNECTED_BIT);
}

static esp_eth_phy_t *create_phy(void)
{
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = CONFIG_P4_ETH_PHY_ADDR;
    phy_config.reset_gpio_num = CONFIG_P4_ETH_PHY_RST_GPIO;

#if CONFIG_P4_ETH_PHY_LAN87XX
    return esp_eth_phy_new_lan87xx(&phy_config);
#elif CONFIG_P4_ETH_PHY_IP101
    return esp_eth_phy_new_ip101(&phy_config);
#elif CONFIG_P4_ETH_PHY_RTL8201
    return esp_eth_phy_new_rtl8201(&phy_config);
#elif CONFIG_P4_ETH_PHY_DP83848
    return esp_eth_phy_new_dp83848(&phy_config);
#elif CONFIG_P4_ETH_PHY_KSZ80XX
    return esp_eth_phy_new_ksz80xx(&phy_config);
#else
    return esp_eth_phy_new_generic(&phy_config);
#endif
}

esp_err_t ethernet_start_and_wait(void)
{
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_event_loop_create_default failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_ERROR_CHECK(esp_netif_init());

    s_eth_event_group = xEventGroupCreate();
    if (!s_eth_event_group) {
        return ESP_ERR_NO_MEM;
    }

    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *eth_netif = esp_netif_new(&netif_cfg);
    if (!eth_netif) {
        return ESP_FAIL;
    }

    #if defined(CONFIG_P4_ETH_USE_STATIC_IP) && CONFIG_P4_ETH_USE_STATIC_IP
        esp_netif_ip_info_t ip_info = {0};
        ESP_ERROR_CHECK(esp_netif_str_to_ip4(CONFIG_P4_ETH_STATIC_IP, &ip_info.ip));
        ESP_ERROR_CHECK(esp_netif_str_to_ip4(CONFIG_P4_ETH_STATIC_NETMASK, &ip_info.netmask));
        ESP_ERROR_CHECK(esp_netif_str_to_ip4(CONFIG_P4_ETH_STATIC_GW, &ip_info.gw));
        ESP_ERROR_CHECK(esp_netif_dhcpc_stop(eth_netif));
        ESP_ERROR_CHECK(esp_netif_set_ip_info(eth_netif, &ip_info));
        ESP_LOGI(TAG, "Static IP set to " IPSTR, IP2STR(&ip_info.ip));
    #endif

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_esp32_emac_config_t emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    emac_config.smi_gpio.mdc_num = CONFIG_P4_ETH_MDC_GPIO;
    emac_config.smi_gpio.mdio_num = CONFIG_P4_ETH_MDIO_GPIO;

    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac_config, &mac_config);
    if (!mac) return ESP_FAIL;

    esp_eth_phy_t *phy = create_phy();
    if (!phy) return ESP_FAIL;

    esp_eth_config_t config = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t eth_handle = NULL;
    err = esp_eth_driver_install(&config, &eth_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_eth_driver_install failed: %s", esp_err_to_name(err));
        return err;
    }

    esp_eth_netif_glue_handle_t glue = esp_eth_new_netif_glue(eth_handle);
    ESP_ERROR_CHECK(esp_netif_attach(eth_netif, glue));

    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &eth_event_handler, &eth_handle));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &got_ip_event_handler, NULL));

    err = esp_eth_start(eth_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_eth_start failed: %s", esp_err_to_name(err));
        return err;
    }

    EventBits_t bits = xEventGroupWaitBits(
        s_eth_event_group,
        ETH_CONNECTED_BIT | ETH_FAIL_BIT,
        pdFALSE,
        pdFALSE,
        portMAX_DELAY);

    if (bits & ETH_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Ethernet connected");
        return ESP_OK;
    }

    ESP_LOGE(TAG, "Ethernet failed to connect");
    return ESP_FAIL;
}
