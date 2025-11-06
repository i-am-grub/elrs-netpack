#include <stdio.h>
#include <stdlib.h>
#include <cstring>
#include "espnow_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "esp_app_desc.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_now.h"
#include "tasks.h"
#include "msptypes.h"
#include "msp.h"
#include "peer_manager.h"

#define NO_BINDING_TIMEOUT 120000 / portTICK_PERIOD_MS
#define STORAGE_NAMESPACE "netpack"
#define STORAGE_MAC_KEY "bp_mac_addr"

static const char *TAG = "espnow_server";

const esp_app_desc_t *description = esp_app_get_description();
const TickType_t espnowDelay = CONFIG_ESPNOW_SEND_DELAY / portTICK_PERIOD_MS;

static nvs_handle_t bp_mac_handle;

static TaskHandle_t espnowTaskHandle = NULL;
static TaskHandle_t bindTaskHandle = NULL;
static RingbufHandle_t xRingReceivedEspnow = NULL;

static bool isBinding = false;

static void sendInProgressResponse()
{
    mspPacket_t out;
    const uint8_t *response = (const uint8_t *)"P";
    out.reset();
    out.makeResponse();
    out.function = MSP_ELRS_BACKPACK_SET_MODE;
    for (uint32_t i = 0; i < 1; i++)
    {
        out.addByte(response[i]);
    }

    if (xRingbufferSend(xRingReceivedEspnow, &out, sizeof(mspPacket_t), pdMS_TO_TICKS(1000)) == pdTRUE)
        ESP_LOGI(TAG, "Added progress response to ring buffer");
    else
        ESP_LOGE(TAG, "Failed to add item progress response to ring buffer");
}

static void runBindTask(void *pvParameters)
{
    vTaskDelay(NO_BINDING_TIMEOUT);
    isBinding = false;
}

static void espnowRecvCB(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len)
{
    MSP msp;
    for (int i = 0; i < len; i++)
    {
        if (msp.processReceivedByte(data[i]))
        {
            mspPacket_t *packet = msp.getReceivedPacket();
            switch (packet->function)
            {
            case MSP_ELRS_BIND:
            {
                if (!isBinding)
                    break;

                if (bindTaskHandle != NULL)
                {
                    vTaskDelete(bindTaskHandle);
                    bindTaskHandle = NULL;
                }

                isBinding = false;

                uint8_t recievedAddress[6];
                for (int i = 0; i < 6; i++)
                {
                    recievedAddress[i] = packet->payload[i];
                }

                if (recievedAddress[0] == 0 && recievedAddress[1] == 0 && recievedAddress[2] == 0 &&
                    recievedAddress[3] == 0 && recievedAddress[4] == 0 && recievedAddress[5] == 0)
                {
                    ESP_LOGW(TAG, "Preventing UID from being saved to default value");
                    break;
                }

                if (nvs_open(STORAGE_NAMESPACE, NVS_READWRITE, &bp_mac_handle) == ESP_OK)
                {
                    if (nvs_set_blob(bp_mac_handle, STORAGE_MAC_KEY, &recievedAddress, sizeof(recievedAddress)) == ESP_OK)
                    {
                        if (nvs_commit(bp_mac_handle) == ESP_OK)
                            ESP_LOGI(TAG, "UID saved to nvs");
                        else
                            ESP_LOGE(TAG, "Failed to commit nvs data");
                    }
                    else
                        ESP_LOGE(TAG, "Failed to write MAC address to nvs");
                }
                else
                    ESP_LOGE(TAG, "Error opening NVS handle!");

                nvs_close(bp_mac_handle);

                recievedAddress[0] = recievedAddress[0] & ~0x01;
                recievedAddress[5] = 3;

                ESP_ERROR_CHECK(esp_wifi_set_mac(WIFI_IF_STA, recievedAddress));

                ESP_LOGI(TAG, "Backpack UID set to: [%d,%d,%d,%d,%d,%d]", recievedAddress[0], recievedAddress[1],
                         recievedAddress[2], recievedAddress[3], recievedAddress[4], recievedAddress[5]);

                break;
            }
            case MSP_ELRS_BACKPACK_SET_RECORDING_STATE:
            {
                if (xRingbufferSend(xRingReceivedEspnow, msp.getReceivedPacket(), sizeof(mspPacket_t), pdMS_TO_TICKS(1000)) == pdTRUE)
                    ESP_LOGI(TAG, "Recording state change added to buffer");
                else
                    ESP_LOGE(TAG, "Failed to add recieved ESPNOW data to ring buffer");
            }
            }

            msp.markPacketReceived();
        }
    }
}

void processPacketFromHost(mspPacket_t *packet)
{
    switch (packet->function)
    {
    case MSP_ELRS_GET_BACKPACK_VERSION:
    {
        ESP_LOGI(TAG, "Processing MSP_ELRS_GET_BACKPACK_VERSION...");

        mspPacket_t out;
        out.reset();
        out.makeResponse();
        out.function = MSP_ELRS_GET_BACKPACK_VERSION;
        for (size_t i = 0; i < sizeof(description->version); i++)
        {
            out.addByte(description->version[i]);
        }

        if (xRingbufferSend(xRingReceivedEspnow, &out, sizeof(mspPacket_t), pdMS_TO_TICKS(1000)) == pdTRUE)
            ESP_LOGI(TAG, "Added device version to ring buffer");
        else
            ESP_LOGE(TAG, "Failed to add item to ring buffer");

        break;
    }
    case MSP_ELRS_BACKPACK_SET_MODE:
    {
        if (packet->payloadSize == 1)
        {
            if (packet->payload[0] == 'B')
            {
                ESP_LOGI(TAG, "Enter binding mode...");
                isBinding = true;
            }
            if (bindTaskHandle != NULL)
            {
                vTaskDelete(bindTaskHandle);
                bindTaskHandle = NULL;
            }

            xTaskCreate(runBindTask, "BindTask", 4096, NULL, 10, &bindTaskHandle);
            sendInProgressResponse();
        }
        break;
    }
    case MSP_ELRS_REGISTER_ESPNOW_PEER:
    {
        ESP_LOGI(TAG, "Processing MSP_ELRS_REGISTER_ESPNOW_PEER...");
        uint8_t mode = packet->readByte();

        // Set target send address
        if (mode == 0x01)
        {
            uint8_t receivedAddress[6];
            receivedAddress[0] = packet->readByte();
            receivedAddress[1] = packet->readByte();
            receivedAddress[2] = packet->readByte();
            receivedAddress[3] = packet->readByte();
            receivedAddress[4] = packet->readByte();
            receivedAddress[5] = packet->readByte();

            peerManager.addPeer(receivedAddress);
        }
        else if (mode == 0x02)
        {
            uint8_t receivedAddress[6];
            receivedAddress[0] = packet->readByte();
            receivedAddress[1] = packet->readByte();
            receivedAddress[2] = packet->readByte();
            receivedAddress[3] = packet->readByte();
            receivedAddress[4] = packet->readByte();
            receivedAddress[5] = packet->readByte();

            peerManager.removePeer(receivedAddress);
        }
        else if (mode == 0x03)
        {
            peerManager.clearPeers();
        }
        break;
    }
    case MSP_ELRS_SEND_RACE_OSD:
    {
        uint8_t receivedAddress[6];
        receivedAddress[0] = packet->readByte();
        receivedAddress[1] = packet->readByte();
        receivedAddress[2] = packet->readByte();
        receivedAddress[3] = packet->readByte();
        receivedAddress[4] = packet->readByte();
        receivedAddress[5] = packet->readByte();

        if (receivedAddress[0] == 0 &&
            receivedAddress[1] == 0 &&
            receivedAddress[2] == 0 &&
            receivedAddress[3] == 0 &&
            receivedAddress[4] == 0 &&
            receivedAddress[5] == 0)
            peerManager.sendToPeers(packet);

        else
            peerManager.sendToPeer(receivedAddress, packet);

        break;
    }
    default:
    {
        peerManager.sendToPeers(packet);
        break;
    }
    }
}

void runESPNOWServer(void *pvParameters)
{

    TaskBufferParams *buffers = (TaskBufferParams *)pvParameters;
    xRingReceivedEspnow = buffers->write;

    espnowTaskHandle = xTaskGetCurrentTaskHandle();

    MSP msp;
    uint8_t macAddress[6];
    

    esp_err_t err;

    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Get Backpack MAC address from NVS
    err = nvs_open(STORAGE_NAMESPACE, NVS_READWRITE, &bp_mac_handle);
    if (err == ESP_OK)
    {
        uint8_t mac_addr[6];
        size_t size = sizeof(mac_addr);
        err = nvs_get_blob(bp_mac_handle, STORAGE_MAC_KEY, macAddress, &size);
        if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND)
            ESP_LOGW(TAG, "Unable to retreive mac address from nvs");
        else
        {
            macAddress[0] = macAddress[0] & ~0x01;
            macAddress[5] = 3;
            ESP_LOGI(TAG, "Backpack UID: [%d,%d,%d,%d,%d,%d]", macAddress[0], macAddress[1],
                     macAddress[2], macAddress[3], macAddress[4], macAddress[5]);
        }
    }
    else
        ESP_LOGE(TAG, "Error (%s) opening NVS handle!", esp_err_to_name(err));

    nvs_close(bp_mac_handle);

    // Start WiFi for ESPNOW
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_channel(CONFIG_ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE));
    ESP_ERROR_CHECK(esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_LR));

    // Start ESPNOW
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_send_cb(espnowSendCB));
    ESP_ERROR_CHECK(esp_now_register_recv_cb(espnowRecvCB));

    ESP_ERROR_CHECK(esp_wifi_set_mac(WIFI_IF_STA, macAddress));

    while (1)
    {
        // Send data from incoming buffer over ESPNOW
        size_t item_size;
        mspPacket_t *packet = (mspPacket_t *)xRingbufferReceive(buffers->read, &item_size, portMAX_DELAY);
        if (packet != NULL)
        {
            uint8_t packetSize = msp.getTotalPacketSize(packet);
            uint8_t nowDataOutput[packetSize];
            uint8_t result = msp.convertToByteArray(packet, nowDataOutput);

            if (result)
            {
                processPacketFromHost(packet);
            }
        }

        vRingbufferReturnItem(buffers->read, (void *)packet);
    }
}
