#include "net_eth.h"
#include "config.h"
#include "led_indicator.h"
#include "wifi.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_eth_mac_w5500.h"
#include "esp_eth_phy_w5500.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"

static const char *TAG = "net_eth";
static esp_eth_handle_t s_eth_handle;
static esp_netif_t *s_eth_netif;
static esp_eth_netif_glue_handle_t s_eth_glue;
static bool s_spi_ready;
static esp_event_handler_instance_t s_eth_handler;
static esp_event_handler_instance_t s_ip_handler;

static void eth_event_handler(void *arg, esp_event_base_t base,
                              int32_t event_id, void *event_data)
{
    (void)base;
    if (event_data == NULL || *(esp_eth_handle_t *)event_data != arg) {
        return;
    }
    switch (event_id) {
    case ETHERNET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "Ethernet link up");
        break;
    case ETHERNET_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "Ethernet link down");
        led_switch(LED_ERR);
        break;
    case ETHERNET_EVENT_START:
        ESP_LOGI(TAG, "Ethernet started");
        break;
    case ETHERNET_EVENT_STOP:
        ESP_LOGI(TAG, "Ethernet stopped");
        break;
    default:
        break;
    }
}

static void ip_event_handler(void *arg, esp_event_base_t base,
                             int32_t event_id, void *event_data)
{
    (void)base;
    (void)event_id;
    const ip_event_got_ip_t *event = event_data;
    if (event == NULL || event->esp_netif != arg) {
        return;
    }
    ESP_LOGI(TAG, "ETH got IP: " IPSTR, IP2STR(&event->ip_info.ip));
    ESP_LOGI(TAG, "ETH netmask: " IPSTR, IP2STR(&event->ip_info.netmask));
    ESP_LOGI(TAG, "ETH gateway: " IPSTR, IP2STR(&event->ip_info.gw));
    led_switch(LED_TX);
}

static esp_err_t eth_spi_bus_init(void)
{
    if (s_spi_ready) {
        return ESP_OK;
    }

    spi_bus_config_t config = {
        .mosi_io_num = ETH_SPI_MOSI_GPIO,
        .miso_io_num = ETH_SPI_MISO_GPIO,
        .sclk_io_num = ETH_SPI_SCLK_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 32,
    };
    esp_err_t err = spi_bus_initialize(SPI3_HOST, &config, SPI_DMA_CH_AUTO);
    if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
        /* If bus is shared, application must have initialized these same pins. */
        s_spi_ready = true;
        return ESP_OK;
    }
    return err;
}

esp_err_t eth_init(void)
{
    if (s_eth_handle != NULL) {
        return ESP_OK;
    }

    esp_err_t err;
    if (ETH_INT_GPIO >= 0) {
        err = gpio_install_isr_service(0);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            return err;
        }
    }
    err = eth_spi_bus_init();
    if (err != ESP_OK) {
        return err;
    }

    /* Higher than the default Wi-Fi STA priority (100): prefer Ethernet. */
    esp_netif_inherent_config_t base = ESP_NETIF_INHERENT_DEFAULT_ETH();
    base.route_prio = 200;
    esp_netif_config_t netif_config = {
        .base = &base,
        .stack = ESP_NETIF_NETSTACK_DEFAULT_ETH,
    };
    esp_netif_t *netif = esp_netif_new(&netif_config);
    if (netif == NULL) {
        return ESP_ERR_NO_MEM;
    }

    spi_device_interface_config_t spi_config = {
        .mode = 0,
        .clock_speed_hz = 8 * 1000 * 1000,
        .spics_io_num = CS_ETH_GPIO,
        .queue_size = 20,
    };
    eth_w5500_config_t w5500_config = ETH_W5500_DEFAULT_CONFIG(SPI3_HOST, &spi_config);
    /* Matches the W5500 component API in the supplied Ethernet source. */
    w5500_config.base.int_gpio_num = ETH_INT_GPIO;
    if (ETH_INT_GPIO < 0) {
        w5500_config.base.poll_period_ms = 10;
    }

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    esp_eth_mac_t *mac = esp_eth_mac_new_w5500(&w5500_config, &mac_config);
    if (mac == NULL) {
        esp_netif_destroy(netif);
        return ESP_ERR_NO_MEM;
    }
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.reset_gpio_num = ETH_RST_GPIO;
    esp_eth_phy_t *phy = esp_eth_phy_new_w5500(&phy_config);
    if (phy == NULL) {
        mac->del(mac);
        esp_netif_destroy(netif);
        return ESP_ERR_NO_MEM;
    }

    esp_eth_handle_t handle = NULL;
    esp_eth_netif_glue_handle_t glue = NULL;
    uint8_t mac_addr[6];
    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    err = esp_eth_driver_install(&eth_config, &handle);
    if (err != ESP_OK) {
        goto fail_driver;
    }
    err = esp_read_mac(mac_addr, ESP_MAC_ETH);
    if (err != ESP_OK) {
        goto fail_installed;
    }
    err = esp_eth_ioctl(handle, ETH_CMD_S_MAC_ADDR, mac_addr);
    if (err != ESP_OK) {
        goto fail_installed;
    }

    glue = esp_eth_new_netif_glue(handle);
    if (glue == NULL) {
        err = ESP_ERR_NO_MEM;
        goto fail_installed;
    }
    err = esp_netif_attach(netif, glue);
    if (err != ESP_OK) {
        esp_eth_del_netif_glue(glue);
        goto fail_installed;
    }

    err = esp_event_handler_instance_register(ETH_EVENT, ESP_EVENT_ANY_ID,
                                              eth_event_handler, handle,
                                              &s_eth_handler);
    if (err != ESP_OK) {
        goto fail_attached;
    }
    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_ETH_GOT_IP,
                                              ip_event_handler, netif,
                                              &s_ip_handler);
    if (err != ESP_OK) {
        esp_event_handler_instance_unregister(ETH_EVENT, ESP_EVENT_ANY_ID,
                                              s_eth_handler);
        goto fail_attached;
    }

    s_eth_handle = handle;
    s_eth_netif = netif;
    s_eth_glue = glue;
    ESP_LOGI(TAG, "W5500 ready on SPI3 at 8 MHz; driver stopped");
    return ESP_OK;

fail_attached:
    esp_eth_del_netif_glue(glue);
fail_installed:
    esp_eth_driver_uninstall(handle);
fail_driver:
    phy->del(phy);
    mac->del(mac);
    esp_netif_destroy(netif);
    return err;
}

esp_eth_handle_t eth_get_handle(void)
{
    return s_eth_handle;
}

esp_netif_t *eth_get_netif(void)
{
    return s_eth_netif;
}

void eth_start_or_retry(void)
{
    esp_err_t err = wifi_manager_retry();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Retry: %s", esp_err_to_name(err));
    }
}