extern "C" {
    #include "osapi.h"
    #include "driver/uart.h"
    #include "user_interface.h"
    #include "espconn.h"
}

#include "user_config.h"
#include "ws2812_i2c.h"

namespace {
    const char *ssid = SSID;
    const char *psk = PSK;

    esp_udp udp;
    espconn connection;
}

void ICACHE_FLASH_ATTR setupWiFi() {
    wifi_set_opmode(0x1);

    station_config config;

    config.bssid_set = 0;

    os_memcpy(&config.ssid, ssid, 12);
    os_memcpy(&config.password, psk, 17);
    os_printf("wifi_station_set_config() -> %d\n", wifi_station_set_config(&config));

    wifi_station_set_auto_connect(true);
}

void udp_recv_callback(void *arg, char *pdata, unsigned short len) {
    ws2812_push(reinterpret_cast<uint8_t*>(pdata), len);
}

void ICACHE_FLASH_ATTR setupUdpServer() {
    udp.local_port = 1234;
    connection.type = ESPCONN_UDP;
    connection.state = ESPCONN_NONE;
    connection.proto.udp = &udp;

    switch (espconn_create(&connection)) {
        case 0:
            os_printf("%d: Created server!\n", __LINE__);
            break;
        case ESPCONN_ISCONN:
            os_printf("%d: Already connected!\n", __LINE__);
            return;
        case ESPCONN_MEM:
            os_printf("%d: Out of memory!\n", __LINE__);
            return;
        case ESPCONN_ARG:
            os_printf("%d: Illegal argumenet!\n", __LINE__);
            return;
    }

    if (espconn_regist_recvcb(&connection, udp_recv_callback)) {
        os_printf("Unknown connection!\n");
        return;
    }
}

extern "C" void ICACHE_FLASH_ATTR user_init() {
    // configure UART TXD to be GPIO1, set as output
    PIN_FUNC_SELECT(PERIPHS_IO_MUX_U0TXD_U, FUNC_GPIO1);

    uart_init(BIT_RATE_115200, BIT_RATE_115200);

    setupWiFi();
    setupUdpServer();

    ws2812_init();
}
