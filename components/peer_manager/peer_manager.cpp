#include <stdio.h>
#include <cstring>
#include <stdexcept>
#include "esp_log.h"
#include "peer_manager.h"
#include "msp.h"

static const char *TAG = "peer_manager";

static void espnowSendCB(const uint8_t *mac_addr, esp_now_send_status_t status)
{
    peerManager.notifyPeer(mac_addr, status);
}

static void sendToPeerTask(void *pvParameters)
{
    MSP msp;
    int sendStatus;
    uint8_t sendAttempt;
    uint32_t sendSuccess;

    Peer_t *peer = (Peer_t *)pvParameters;

    while (1)
    {
        size_t item_size;
        mspPacket_t *packet = (mspPacket_t *)xRingbufferReceive(peer->buffer, &item_size, portMAX_DELAY);

        if (packet != NULL)
        {
            uint8_t packetSize = msp.getTotalPacketSize(packet);
            uint8_t nowDataOutput[packetSize];

            uint8_t result = msp.convertToByteArray(packet, nowDataOutput);
            if (!result)
            {
                ESP_LOGE(TAG, "Packet could not be converted to array");
                vRingbufferReturnItem(peer->buffer, (void *)packet);
                continue;
            }

            sendAttempt = 0;
            do
            {
                sendStatus = esp_now_send(peer->peerInfo.peer_addr, (uint8_t *)&nowDataOutput, packetSize);

                if (sendStatus == ESP_OK)
                    xTaskNotifyWait(0x00, ULONG_MAX, &sendSuccess, portMAX_DELAY);
                else
                {
                    ESP_LOGW(TAG, "ESPNOW message send status: %d", sendStatus);
                    break;
                }

            } while (++sendAttempt < MAX_RETRIES && !sendSuccess);

            vRingbufferReturnItem(peer->buffer, (void *)packet);
        }
    }
}

Peer::Peer(uint8_t address[])
{
    memset(&peerInfo, 0, sizeof(peerInfo));
    memcpy(peerInfo.peer_addr, address, 6);
    peerInfo.channel = 0;
    peerInfo.encrypt = false;
}

Peer::~Peer()
{
    vTaskDelete(taskHandle);
    vRingbufferDelete(buffer);
    esp_now_del_peer(peerInfo.peer_addr);
}

PeerManager::PeerManager()
{
    vSemaphoreCreateBinary(xSemaphore);
    ESP_ERROR_CHECK(esp_now_register_send_cb(espnowSendCB));
}

PeerManager::~PeerManager()
{
    vSemaphoreDelete(xSemaphore);
}

const Peer_t *
PeerManager::findPeer(const uint8_t *address)
{
    for (const Peer_t &peer : peers)
    {
        if (peer.peerInfo.peer_addr[0] == address[0] &&
            peer.peerInfo.peer_addr[1] == address[1] &&
            peer.peerInfo.peer_addr[2] == address[2] &&
            peer.peerInfo.peer_addr[3] == address[3] &&
            peer.peerInfo.peer_addr[4] == address[4] &&
            peer.peerInfo.peer_addr[5] == address[5])
            return &peer;
    }
    return nullptr;
}

void PeerManager::addPeer(uint8_t address[])
{
    if (xSemaphoreTake(xSemaphore, portMAX_DELAY) == pdTRUE)
    {
        peers.emplace_back(address);

        int status = esp_now_add_peer(&peers.back().peerInfo);
        if (status != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to register new peer: %d", status);
            peers.pop_back();
            xSemaphoreGive(xSemaphore);
            return;
        }

        xTaskCreate(sendToPeerTask, "PeerSenderTask", 4096, (void *)&peers.back(), 9, &peers.back().taskHandle);

        xSemaphoreGive(xSemaphore);
    }
}

void PeerManager::removePeer(uint8_t *address)
{
    if (xSemaphoreTake(xSemaphore, portMAX_DELAY) == pdTRUE)
    {
        for (int i = 0; i < peers.size(); i++)
        {
            const Peer_t *peer = &peers[i];

            if (peer->peerInfo.peer_addr[0] == address[0] &&
                peer->peerInfo.peer_addr[1] == address[1] &&
                peer->peerInfo.peer_addr[2] == address[2] &&
                peer->peerInfo.peer_addr[3] == address[3] &&
                peer->peerInfo.peer_addr[4] == address[4] &&
                peer->peerInfo.peer_addr[5] == address[5])
            {
                peers.erase(peers.begin() + i);
                xSemaphoreGive(xSemaphore);
                return;
            }
        }
        xSemaphoreGive(xSemaphore);
    }
}

void PeerManager::clearPeers()
{
    if (xSemaphoreTake(xSemaphore, portMAX_DELAY) == pdTRUE)
    {
        peers.clear();
        xSemaphoreGive(xSemaphore);
    }
}

void PeerManager::sendToPeer(uint8_t *address, mspPacket_t *packet)
{
    if (xSemaphoreTake(xSemaphore, portMAX_DELAY) == pdTRUE)
    {
        const Peer_t *peer = findPeer(address);
        if (peer != nullptr)
            xRingbufferSend(peer->buffer, packet, sizeof(mspPacket_t), pdMS_TO_TICKS(1000));
        xSemaphoreGive(xSemaphore);
    }
}

void PeerManager::sendToPeers(mspPacket_t *packet)
{
    if (xSemaphoreTake(xSemaphore, portMAX_DELAY) == pdTRUE)
    {
        for (const Peer_t &peer : peers)
        {
            xRingbufferSend(peer.buffer, packet, sizeof(mspPacket_t), pdMS_TO_TICKS(1000));
        }
        xSemaphoreGive(xSemaphore);
    }
}

void PeerManager::notifyPeer(const uint8_t *address, esp_now_send_status_t status)
{
    if (xSemaphoreTake(xSemaphore, portMAX_DELAY) == pdTRUE)
    {
        const Peer_t *peer = findPeer(address);
        if (peer == nullptr)
        {
            xSemaphoreGive(xSemaphore);
            return;
        }

        if (status == ESP_NOW_SEND_SUCCESS)
            xTaskNotify(peer->taskHandle, (uint32_t)1, eSetValueWithOverwrite);
        else
            xTaskNotify(peer->taskHandle, (uint32_t)0, eSetValueWithOverwrite);

        xSemaphoreGive(xSemaphore);
    }
}