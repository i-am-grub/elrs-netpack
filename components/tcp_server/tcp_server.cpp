#include <stdio.h>
#include <string.h>
#include <errno.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/ringbuf.h"
#include "freertos/task.h"
#include "esp_netif.h"
#include "esp_netif_net_stack.h"
#include "esp_eth.h"
#include "esp_event.h"
#include "esp_log.h"
#include "ethernet_init.h"
#include "lwip/sockets.h"
#include "dhcpserver/dhcpserver_options.h"
#include "tasks.h"
#include "msp.h"
#include "mdns.h"

#define INVALID_SOCKET -1
#define LISTENER_MAX_QUEUE 1
#define SOCKET_MAX_LENGTH 1440 // at least equal to MSS
#define MAX_MSG_LENGTH 128
static const char *TAG = "tcp_server";

static QueueSetHandle_t queue_set = xQueueCreateSet(51);
static SemaphoreHandle_t xSemaphore = NULL;
static RingbufHandle_t xRingReceivedEspnow = NULL;

/* Structure to store information about individual connection */
struct connection_info
{
    int fd;
    struct sockaddr_in address;
};

static struct connection_info connections[LISTENER_MAX_QUEUE];
static int active_connections_count = 0;
static fd_set ready;

static bool mdns_setup = false;

static void initialize_mdns(esp_netif_t *eth_netif)
{
    ESP_ERROR_CHECK(mdns_init());
    ESP_ERROR_CHECK(mdns_hostname_set("elrs-netpack"));

    mdns_txt_item_t serviceTxtData[3] = {
        {"board", "esp32s3"},
        {"type", "time"},
        {"project", "elrs-netpack"}};

    ESP_ERROR_CHECK(mdns_service_add("ExpressLRS Backpack", "_elrs-backpack", "_tcp", CONFIG_TCP_SERVER_PORT, serviceTxtData, 3));

    ESP_ERROR_CHECK(mdns_register_netif(eth_netif));
    ESP_ERROR_CHECK(mdns_netif_action(eth_netif, MDNS_EVENT_ENABLE_IP4));
    ESP_ERROR_CHECK(mdns_netif_action(eth_netif, MDNS_EVENT_ANNOUNCE_IP4));
    ESP_ERROR_CHECK(mdns_netif_action(eth_netif, MDNS_EVENT_IP4_REVERSE_LOOKUP));

    mdns_setup = true;
}

/* Event handler for IP_EVENT_ETH_GOT_IP */
static void got_ip_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *data)
{
    esp_netif_t *eth_netif = (esp_netif_t *)arg;
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
    const esp_netif_ip_info_t *ip_info = &event->ip_info;

    ESP_LOGI(TAG, "Ethernet Got IP Address");
    ESP_LOGI(TAG, "~~~~~~~~~~~");
    ESP_LOGI(TAG, "ETHIP:" IPSTR, IP2STR(&ip_info->ip));
    ESP_LOGI(TAG, "ETHMASK:" IPSTR, IP2STR(&ip_info->netmask));
    ESP_LOGI(TAG, "ETHGW:" IPSTR, IP2STR(&ip_info->gw));
    ESP_LOGI(TAG, "~~~~~~~~~~~");

    if (!mdns_setup)
        initialize_mdns(eth_netif);
}

static void tcp_server_sender(void *pvParameters)
{
    MSP msp;

    // Send any processed data to active connections
    // Use the queue set to check for any new data instead of using timeout to recieve
    while (1)
    {
        QueueSetMemberHandle_t member = xQueueSelectFromSet(queue_set, portMAX_DELAY);
        if (member != NULL && xRingbufferCanRead(xRingReceivedEspnow, member) == pdTRUE)
        {
            ESP_LOGI(TAG, "Attempting to send processed packet over TCP server");

            if (xSemaphoreTake(xSemaphore, portMAX_DELAY) == pdTRUE)
            {
                ESP_LOGD(TAG, "Send task taken semaphore");

                // Timeout for receiving from buffer doesn't matter due to checking the queue set
                size_t item_size;
                mspPacket_t *packet = (mspPacket_t *)xRingbufferReceive(xRingReceivedEspnow, &item_size, 0);

                uint8_t packetSize = msp.getTotalPacketSize(packet);
                uint8_t nowDataOutput[packetSize];
                uint8_t result = msp.convertToByteArray(packet, nowDataOutput);

                if (result)
                {
                    for (int i = 0; i < active_connections_count; i++)
                    {
                        int fd = connections[i].fd;

                        if (fd > 0 && FD_ISSET(fd, &ready))
                        {
                            if (send(fd, &nowDataOutput, packetSize, 0) < 0)
                                ESP_LOGE(TAG, "Failed to send response: errno %d", errno);
                        }
                    }
                }
                else

                    ESP_LOGE(TAG, "Failed to convert packet from buffer");

                vRingbufferReturnItem(xRingReceivedEspnow, (void *)packet);
                xSemaphoreGive(xSemaphore);

                ESP_LOGD(TAG, "Send task released semaphore");
            }
            else
                ESP_LOGD(TAG, "Failed to take lock");
        }
    }
}

void run_tcp_server(void *pvParameters)
{
    MSP msp;
    TaskBufferParams *buffers = (TaskBufferParams *)pvParameters;
    xRingReceivedEspnow = buffers->read;

    // Add ring buffer to queue set
    if (xRingbufferAddToQueueSetRead(xRingReceivedEspnow, queue_set) != pdTRUE)
        ESP_LOGI(TAG, "Failed to add to queue set");

    vSemaphoreCreateBinary(xSemaphore);
    if (xSemaphore != NULL)
        ESP_LOGI(TAG, "Semaphore created");
    else
        ESP_LOGE(TAG, "Semaphore does not exist");

    // Initialize Ethernet driver
    uint8_t eth_port_cnt = 0;
    esp_eth_handle_t *eth_handles;
    ESP_ERROR_CHECK(ethernet_init_all(&eth_handles, &eth_port_cnt));

    char if_key_str[10];
    char if_desc_str[10];
    esp_netif_config_t cfg;
    esp_netif_inherent_config_t eth_netif_cfg;

    if (eth_port_cnt == 1)
    {
        // Use default config when using one interface
        eth_netif_cfg = *(ESP_NETIF_BASE_DEFAULT_ETH);
    }
    else
    {
        // Set config to support multiple interfaces
        eth_netif_cfg = (esp_netif_inherent_config_t)ESP_NETIF_INHERENT_DEFAULT_ETH();
    }
    cfg = (esp_netif_config_t){
        .base = &eth_netif_cfg,
        .stack = ESP_NETIF_NETSTACK_DEFAULT_ETH};
    sprintf(if_key_str, "ETH_%d", 0);
    sprintf(if_desc_str, "eth%d", 0);
    eth_netif_cfg.if_key = if_key_str;
    eth_netif_cfg.if_desc = if_desc_str;
    eth_netif_cfg.route_prio -= 0 * 5;
    esp_netif_t *eth_netif = esp_netif_new(&cfg);

    // Attach Ethernet driver to TCP/IP stack
    ESP_ERROR_CHECK(esp_netif_attach(eth_netif, esp_eth_new_netif_glue(eth_handles[0])));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, got_ip_event_handler, eth_netif));
    // Start Ethernet driver state machine
    for (int i = 0; i < eth_port_cnt; i++)
    {
        ESP_ERROR_CHECK(esp_eth_start(eth_handles[i]));
    }

    char *rxbuffer = NULL;
    char *txbuffer = NULL;

    // Initialize Berkeley socket which will listen on port TCP_SERVER_PORT for transmission from client
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0)
    {
        ESP_LOGE(TAG, "Failed to create socket: errno %d", errno);
        return;
    }

    int max_fd = server_fd;

    struct sockaddr_in address;
    socklen_t addrlen = sizeof(address);
    int enable = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable)) < 0)
    {
        ESP_LOGE(TAG, "Failed to set socket option reuseaddr: errno %d", errno);
        goto err;
    }

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(CONFIG_TCP_SERVER_PORT);

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0)
    {
        ESP_LOGE(TAG, "Failed to bind socket: errno %d", errno);
        goto err;
    }

    // Listen and wait for transmission to come
    if (listen(server_fd, LISTENER_MAX_QUEUE) < 0)
    {
        ESP_LOGE(TAG, "Failed to listen on socket: errno %d", errno);
        goto err;
    }
    ESP_LOGI(TAG, "Server listening on port %d", CONFIG_TCP_SERVER_PORT);

    rxbuffer = (char *)malloc(SOCKET_MAX_LENGTH);
    if (rxbuffer == NULL)
    {
        ESP_LOGE(TAG, "Failed to allocate rxbuffer");
        goto err;
    }
    txbuffer = (char *)malloc(MAX_MSG_LENGTH);
    if (txbuffer == NULL)
    {
        ESP_LOGE(TAG, "Failed to allocate txbuffer");
        goto err;
    }

    // Initialize connections array
    for (int i = 0; i < LISTENER_MAX_QUEUE; i++)
    {
        connections[i].fd = INVALID_SOCKET;
    }

    // Create TCP send task with lower priority than manager
    TaskHandle_t tcpTaskHandle;
    xTaskCreatePinnedToCore(tcp_server_sender, "SockerSenderTask", 4096, NULL, 8, &tcpTaskHandle, 1);

    while (1)
    {
        if (xSemaphoreTake(xSemaphore, portMAX_DELAY) == pdTRUE)
        {
            ESP_LOGD(TAG, "Manager has taken semaphore");

            // Clear and add server to set
            FD_ZERO(&ready);
            FD_SET(server_fd, &ready);

            // Add client(s) to set
            for (int i = 0; i < active_connections_count; i++)
            {
                int conn_fd = connections[i].fd;
                if (conn_fd > 0)
                {
                    FD_SET(conn_fd, &ready);
                    if (conn_fd > max_fd)
                    {
                        max_fd = conn_fd;
                    }
                }
            }

            xSemaphoreGive(xSemaphore);
            ESP_LOGD(TAG, "Manager released semaphore");
        }
        else
        {
            ESP_LOGD(TAG, "Manager timed out taking lock");
            continue;
        }

        // Wait for activity
        int activity = select(max_fd + 1, &ready, NULL, NULL, NULL);
        if (activity < 0)
        {
            ESP_LOGE(TAG, "Select error: errno %d", errno);
            continue;
        }

        if (xSemaphoreTake(xSemaphore, portMAX_DELAY) == pdFALSE)
        {
            ESP_LOGD(TAG, "Manager unable to take semaphore");
            continue;
        }

        // Check if new connections are pending
        if (FD_ISSET(server_fd, &ready) && active_connections_count < LISTENER_MAX_QUEUE)
        {
            // Accept new connection
            struct sockaddr_in *current_address_ptr = &connections[active_connections_count].address;
            int new_fd = accept(server_fd, (struct sockaddr *)current_address_ptr, &addrlen);

            if (new_fd < 0)
            {
                ESP_LOGE(TAG, "Failed to accept connection: errno %d", errno);
            }
            else
            {
                connections[active_connections_count].fd = new_fd;
                ESP_LOGI(TAG, "New connection accepted from %s:%d, socket fd: %d",
                         inet_ntoa(current_address_ptr->sin_addr),
                         ntohs(current_address_ptr->sin_port),
                         new_fd);
                active_connections_count++;
            }
        }

        // Clean up disconnected clients and compact the array
        for (int i = 0; i < active_connections_count; i++)
        {
            if (connections[i].fd == INVALID_SOCKET)
            {
                // Move the last connection to this slot and reduce count
                if (i < active_connections_count - 1)
                {
                    connections[i] = connections[active_connections_count - 1];
                }
                active_connections_count--;
                i--; // Recheck this position
            }
        }

        // Check existing connections for data
        for (int i = 0; i < active_connections_count; i++)
        {
            int fd = connections[i].fd;

            // Check if client sent message
            if (fd > 0 && FD_ISSET(fd, &ready))
            {
                memset(rxbuffer, 0, SOCKET_MAX_LENGTH);
                int n = read(fd, rxbuffer, SOCKET_MAX_LENGTH);

                if (n < 0)
                {
                    ESP_LOGE(TAG, "Error reading from socket: errno %d", errno);
                    close(fd);
                    connections[i].fd = INVALID_SOCKET;
                }
                else if (n == 0)
                {
                    // Connection closed by client
                    ESP_LOGI(TAG, "Client disconnected, socket fd: %d", fd);
                    close(fd);
                    connections[i].fd = INVALID_SOCKET;
                }
                else
                {
                    ESP_LOGI(TAG, "Received %d bytes from %s", n, inet_ntoa(connections[i].address.sin_addr));
                    for (int j = 0; j < n; j++)
                    {
                        if (msp.processReceivedByte(rxbuffer[j]))
                        {
                            ESP_LOGI(TAG, "Successfully processed msp packet from tcp socket");

                            UBaseType_t res = xRingbufferSend(buffers->write, msp.getReceivedPacket(), sizeof(mspPacket_t), pdMS_TO_TICKS(1000));
                            if (res != pdTRUE)
                            {
                                ESP_LOGE(TAG, "Failed to add item to ring buffer");
                            }

                            msp.markPacketReceived();
                        }
                    }
                }
            }
        }

        xSemaphoreGive(xSemaphore);
    }

err:
    if (rxbuffer)
    {
        free(rxbuffer);
    }
    if (txbuffer)
    {
        free(txbuffer);
    }
    if (server_fd != INVALID_SOCKET)
    {
        close(server_fd);
    }
}
