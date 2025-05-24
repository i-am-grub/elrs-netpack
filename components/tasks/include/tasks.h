#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"

struct TaskBufferParams {
    RingbufHandle_t write;
    RingbufHandle_t read;
};