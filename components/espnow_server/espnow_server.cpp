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

#define NO_BINDING_TIMEOUT 120000 / portTICK_PERIOD_MS
#define STORAGE_NAMESPACE "netpack"
#define STORAGE_MAC_KEY "bp_mac_addr"

static const char *TAG = "espnow_server";

const esp_app_desc_t *description = esp_app_get_description();
const TickType_t espnowDelay = CONFIG_ESPNOW_SEND_DELAY / portTICK_PERIOD_MS;

uint8_t bindAddress[6];
uint8_t sendAddress[6];
nvs_handle_t bp_mac_handle;

static TaskHandle_t espnowTaskHandle = NULL;
static TaskHandle_t bindTaskHandle = NULL;
static RingbufHandle_t xRingReceivedEspnow = NULL;

bool isBinding = false;

void sendInProgressResponse()
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

void runBindTask(void *pvParameters)
{
    vTaskDelay(NO_BINDING_TIMEOUT);
    isBinding = false;
}

void registerPeer(uint8_t *address)
{
    esp_now_peer_info_t peerInfo;
    memset(&peerInfo, 0, sizeof(peerInfo));
    memcpy(peerInfo.peer_addr, address, 6);
    peerInfo.channel = 0;
    peerInfo.encrypt = false;
    if (esp_now_add_peer(&peerInfo) != ESP_OK)
    {
        ESP_LOGE(TAG, "ESP-NOW failed to add peer");
    }
}

int sendMSPViaEspnow(mspPacket_t *packet)
{
    MSP msp;
    int esp_err = -1;
    uint8_t packetSize = msp.getTotalPacketSize(packet);
    uint8_t nowDataOutput[packetSize];

    uint8_t result = msp.convertToByteArray(packet, nowDataOutput);

    if (!result)
    {
        ESP_LOGE(TAG, "Packet could not be converted to array");
        return esp_err;
    }

    esp_now_peer_num_t pn;
    esp_now_get_peer_num(&pn);

    esp_err = esp_now_send(sendAddress, (uint8_t *)&nowDataOutput, packetSize);

    ESP_LOGI(TAG, "Sent ESPNOW message");
    return esp_err;
}

static void espnowSendCB(const uint8_t *mac_addr, esp_now_send_status_t status)
{
    if (status == ESP_NOW_SEND_SUCCESS)
        xTaskNotify(espnowTaskHandle, (uint32_t)1, eSetValueWithOverwrite);
    else
        xTaskNotify(espnowTaskHandle, (uint32_t)0, eSetValueWithOverwrite);
}

static void espnowRecvCB(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len)
{
    MSP msp;
    esp_now_peer_info_t peerInfo;
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

                recievedAddress[0] = recievedAddress[0] & ~0x01;

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

                if (bindAddress[0] == sendAddress[0] && bindAddress[1] == sendAddress[1] && bindAddress[2] == sendAddress[2] &&
                    bindAddress[3] == sendAddress[3] && bindAddress[4] == sendAddress[4] && bindAddress[5] == sendAddress[5])
                {
                    memset(&sendAddress, 0, sizeof(sendAddress));
                    memcpy(sendAddress, recievedAddress, 6);

                    if (esp_now_fetch_peer(true, &peerInfo) == ESP_OK)
                        esp_now_del_peer(peerInfo.peer_addr);

                    registerPeer(sendAddress);
                }

                memset(&bindAddress, 0, sizeof(bindAddress));
                memcpy(bindAddress, recievedAddress, 6);

                ESP_LOGI(TAG, "Backpack UID set to: [%d,%d,%d,%d,%d,%d]", bindAddress[0], bindAddress[1],
                         bindAddress[2], bindAddress[3], bindAddress[4], bindAddress[5]);

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

void runESPNOWServer(void *pvParameters)
{

    TaskBufferParams *buffers = (TaskBufferParams *)pvParameters;
    xRingReceivedEspnow = buffers->write;

    espnowTaskHandle = xTaskGetCurrentTaskHandle();

    uint8_t sendAttempt = 0;
    uint32_t sendSuccess = 0;
    int sendStatus = -1;
    MSP msp;

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
        err = nvs_get_blob(bp_mac_handle, STORAGE_MAC_KEY, bindAddress, &size);
        if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND)
            ESP_LOGW(TAG, "Unable to retreive mac address from nvs");
        else
        {
            memcpy(sendAddress, bindAddress, 6);
            bindAddress[0] = bindAddress[0] & ~0x01;
            ESP_LOGI(TAG, "Backpack UID: [%d,%d,%d,%d,%d,%d]", bindAddress[0], bindAddress[1],
                     bindAddress[2], bindAddress[3], bindAddress[4], bindAddress[5]);
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

    ESP_ERROR_CHECK(esp_wifi_set_mac(WIFI_IF_STA, bindAddress));

    // Register default peer if UID set
    if (bindAddress[0] != 0 || bindAddress[1] != 0 || bindAddress[2] != 0 ||
        bindAddress[3] != 0 || bindAddress[4] != 0 || bindAddress[5] != 0)
        registerPeer(bindAddress);

    while (1)
    {
        // Send data from incoming buffer over ESPNOW
        size_t item_size;
        mspPacket_t *packet = (mspPacket_t *)xRingbufferReceive(buffers->read, &item_size, pdMS_TO_TICKS(1000));
        if (packet != NULL)
        {
            uint8_t packetSize = msp.getTotalPacketSize(packet);
            uint8_t nowDataOutput[packetSize];
            uint8_t result = msp.convertToByteArray(packet, nowDataOutput);

            if (result)
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
                case MSP_ELRS_SET_SEND_UID:
                {
                    ESP_LOGI(TAG, "Processing MSP_ELRS_SET_SEND_UID...");
                    uint8_t mode = packet->readByte();

                    // Unregister current peer
                    esp_now_del_peer(sendAddress);
                    memset(&sendAddress, 0, sizeof(sendAddress));

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

                        ESP_LOGI(TAG, "Setting to recieved address");
                        memcpy(sendAddress, receivedAddress, 6);
                    }
                    else
                    {
                        ESP_LOGI(TAG, "Resetting to default address");
                        memcpy(sendAddress, bindAddress, 6);
                    }

                    ESP_ERROR_CHECK(esp_wifi_set_mac(WIFI_IF_STA, sendAddress));

                    if (sendAddress[0] != 0 || sendAddress[1] != 0 || sendAddress[2] != 0 ||
                        sendAddress[3] != 0 || sendAddress[4] != 0 || sendAddress[5] != 0)
                        registerPeer(sendAddress);

                    ESP_LOGI(TAG, "Send UID set to: [%d,%d,%d,%d,%d,%d]", sendAddress[0], sendAddress[1],
                             sendAddress[2], sendAddress[3], sendAddress[4], sendAddress[5]);

                    break;
                }
                default:
                {
                    sendAttempt = 0;
                    do
                    {
                        sendStatus = sendMSPViaEspnow(packet);
                        if (sendStatus == ESP_OK)
                            xTaskNotifyWait(0x00, ULONG_MAX, &sendSuccess, portMAX_DELAY);
                        else
                        {
                            ESP_LOGW(TAG, "ESPNOW message send status: %d", sendStatus);
                            break;
                        }
                        vTaskDelay(espnowDelay);
                    } while (++sendAttempt < CONFIG_ESPNOW_MAX_SEND_ATTEMPTS && !sendSuccess);

                    ESP_LOGI(TAG, "ESPNOW message send attempts: %d", sendAttempt);
                    break;
                }
                }
            }

            vRingbufferReturnItem(buffers->read, (void *)packet);
        }
    }
}
