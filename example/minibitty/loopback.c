/*
 * Copyright 2025 Jetperch LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "minibitty_exe_prv.h"
#include "jsdrv/cstr.h"
#include "jsdrv_prv/platform.h"
#include <stdio.h>
#include <string.h>

#include <windows.h>  // todo remove

#define PING_SIZE_U32 ((512U - 12U) >> 2)

struct loopback_s {
    volatile uint64_t ping_count;
    volatile uint64_t pong_count;
    uint64_t outstanding;
    HANDLE event;
};

struct loopback_s loopback_ = {
    .ping_count = 0,
    .pong_count = 0,
    .outstanding = 1,
    .event = NULL,
};


static void on_pong(void * user_data, const char * topic, const struct jsdrv_union_s * value) {
    (void) user_data;
    (void) topic;
    const uint32_t * p_u32 = (const uint32_t *) value->value.bin;
    if (value->size != (PING_SIZE_U32 * 4)) {
        printf("ERROR pong size = %u\n", value->size);
        quit_ = true;
        return;
    }
    for (uint32_t i = 0; i < PING_SIZE_U32; ++i) {
        if (p_u32[i] != (uint32_t) (loopback_.pong_count + i)) {
            if (0 == loopback_.pong_count) {
                printf("ERROR pong: not yet in sync\n");
                return;
            }
            printf("ERROR pong %llu %lu %u: %lu != %lu\n", loopback_.pong_count, value->size, i,
                p_u32[i], (uint32_t) (loopback_.pong_count + i));
            quit_ = true;
            return;
        }
    }
    loopback_.pong_count++;
    SetEvent(loopback_.event);
}

static int link_lookback(struct app_s * self, const char * device) {
    uint64_t pong_prev = 0;
    int64_t time_prev = jsdrv_time_utc();
    int64_t time_now = 0;
    int64_t time_delta = 0;

    struct jsdrv_topic_s pong_topic;
    ROE(jsdrv_open(self->context, device, JSDRV_DEVICE_OPEN_MODE_RESUME, 0));
    Sleep(100);

    jsdrv_topic_set(&pong_topic, self->device.topic);
    jsdrv_topic_append(&pong_topic, "h/link/!pong");
    jsdrv_subscribe(self->context, pong_topic.topic, JSDRV_SFLAG_PUB, on_pong, NULL, 0);

    jsdrv_topic_set(&self->topic, self->device.topic);
    jsdrv_topic_append(&self->topic, "h/link/!ping");
    fflush(stdout);

    while (!quit_) {
        ResetEvent(loopback_.event);
        while ((loopback_.ping_count - loopback_.pong_count) < loopback_.outstanding) {
            uint32_t ping_data[PING_SIZE_U32];
            for (uint32_t i = 0; i < PING_SIZE_U32; ++i) {
                ping_data[i] = loopback_.ping_count + i;
            }
            jsdrv_publish(self->context, self->topic.topic, &jsdrv_union_bin((uint8_t *) ping_data, sizeof(ping_data)), 0);
            ++loopback_.ping_count;
        }
        time_now = jsdrv_time_utc();
        time_delta = time_now - time_prev;
        if (time_delta > JSDRV_TIME_SECOND) {
            uint64_t pong_delta = loopback_.pong_count - pong_prev;
            printf("Throughput: %llu frames = %llu bytes\n", pong_delta, pong_delta * PING_SIZE_U32 * 4);
            fflush(stdout);
            time_prev = time_now;
            pong_prev = loopback_.pong_count;
        }
        WaitForSingleObject(loopback_.event, 1);
    }

    return jsdrv_close(self->context, device, 0);
}

static int usage(void) {
    printf("usage: minibitty loopback device_path\n");
    return 1;
}

int on_loopback(struct app_s * self, int argc, char * argv[]) {
    char *device_filter = NULL;

    while (argc) {
        if (argv[0][0] != '-') {
            device_filter = argv[0];
            ARG_CONSUME();
        } else if ((0 == strcmp(argv[0], "--verbose")) || (0 == strcmp(argv[0], "-v"))) {
            self->verbose++;
            ARG_CONSUME();
        } else if ((0 == strcmp(argv[0], "--outstanding")) || (0 == strcmp(argv[0], "-o"))) {
            self->verbose++;
            ARG_CONSUME();
            ARG_REQUIRE();
            if (jsdrv_cstr_to_u64(argv[0], &loopback_.outstanding)) {
                printf("ERROR: invalid --outstanding value\n");
                return usage();
            }
            ARG_CONSUME();
        } else {
            return usage();
        }
    }

    if (NULL == device_filter) {
        printf("device_filter required\n");
        return usage();
    }

    ROE(app_match(self, device_filter));

    loopback_.event = CreateEvent(
            NULL,  // default security attributes
            TRUE,  // manual reset event
            TRUE,  // start signalled to pend initial transactions
            NULL   // no name
    );

    // todo loopback @ pubsub layer

    return link_lookback(self, self->device.topic);
}
