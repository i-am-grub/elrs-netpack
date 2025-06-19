#include <stdio.h>
#include <vector>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "esp_now.h"
#include "msp.h"

#define MAX_RETRIES 5
#define BUFFER_MAX_ITEMS 64

typedef struct Peer
{
    esp_now_peer_info_t peerInfo;
    TaskHandle_t taskHandle = NULL;
    RingbufHandle_t buffer = xRingbufferCreateNoSplit(sizeof(mspPacket_t), BUFFER_MAX_ITEMS);

    Peer(uint8_t peer_address[]);
    ~Peer();
} Peer_t;

class PeerManager
{
private:
    std::vector<Peer_t> peers;
    SemaphoreHandle_t xSemaphore = NULL;
    const Peer_t* findPeer(const uint8_t *address);

public:
    PeerManager();
    void addPeer(uint8_t address[]);
    void removePeer(uint8_t *address);
    void clearPeers();
    void sendToPeer(uint8_t *address, mspPacket_t *packet);
    void sendToPeers(mspPacket_t *packet);
    void notifyPeer(const uint8_t *address, esp_now_send_status_t status);
} peerManager;