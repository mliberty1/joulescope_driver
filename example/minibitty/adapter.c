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

#define MB_TRACE_SOF  (0xC3U)
#define COUNTER_FMT "%10lu "
enum mb_trace_type_e {
    MB_TRACE_TYPE_INVALID = 0,
    MB_TRACE_TYPE_READY = 1,
    MB_TRACE_TYPE_ENTER = 2,
    MB_TRACE_TYPE_EXIT = 3,  // optional duration if enter is omitted
    MB_TRACE_TYPE_ALLOC = 4,
    MB_TRACE_TYPE_FREE = 5,
    MB_TRACE_TYPE_RSV6 = 6,
    MB_TRACE_TYPE_RSV7 = 7,
    MB_TRACE_TYPE_TIMESYNC = 8,
    MB_TRACE_TYPE_TIMEMAP = 9,
    MB_TRACE_TYPE_FAULT = 10,
    MB_TRACE_TYPE_VALUE = 11,
    MB_TRACE_TYPE_LOG = 12,
    MB_TRACE_TYPE_RSV13 = 13,
    MB_TRACE_TYPE_RSV14 = 14,
    MB_TRACE_TYPE_OVERFLOW = 15,
};

const char * OBJ_NAME[16] = {
    "isr",
    "context",
    "task",
    "timer",
    "msg",
    "rsv",
    "heap",
    "trace",
    "fsm",
    "unknown_9",
    "unknown_A",
    "unknown_B",
    "unknown_C",
    "unknown_D",
    "unknown_E",
    "unknown_F",
};

static int usage(void) {
    printf(
        "usage: minibitty adapter [options] [device_filter]\n"
        "options:\n"
        "  none\n"
        );
    return 1;
}

static void on_trace(void * user_data, const char * topic, const struct jsdrv_union_s * value) {
    (void) user_data;
    (void) topic;
    if (value->type != JSDRV_UNION_BIN) {
        JSDRV_LOGW("trace: invalid type %d", value->type);
        return;
    }
    const uint8_t * p8 = value->value.bin;
    const uint32_t * p32 = (const uint32_t *) p8;
    const uint32_t * p32_end = p32 + ((value->size + 3) >> 2);
    while (p32 < p32_end) {
        if (MB_TRACE_SOF != (p32[0] & 0xff)) {
            JSDRV_LOGW("trace: invalid SOF");
            while (MB_TRACE_SOF != (p32[0] & 0xff)) {
                ++p32;
                if (p32 >= p32_end) {
                    return;
                }
            }
        }
        uint8_t length = (p32[0] >> 8) & 0x0f;
        uint8_t type = (p32[0] >> 12) & 0x0f;
        uint16_t metadata = p32[0] >> 16;
        uint32_t obj_type = (metadata >> 12) & 0x000f;
        const char * obj_name = OBJ_NAME[obj_type];
        uint32_t obj_id = metadata & 0x0fff;
        uint32_t counter = p32[1];
        p32 += 2;
        uint32_t file_id = 0;
        uint32_t line = 0;
        if (length) {
            file_id = (p32[0] >> 16) & 0x0000ffff;
            line = p32[0] & 0x0000ffff;
        }

        switch (type) {
            case MB_TRACE_TYPE_INVALID:
                JSDRV_LOGW("trace type invalid");
                break;
            case MB_TRACE_TYPE_READY:
                printf(COUNTER_FMT "%s.%d ready\n", counter, obj_name, obj_id);
                break;
            case MB_TRACE_TYPE_ENTER:
                printf(COUNTER_FMT "%s.%d enter\n", counter, obj_name, obj_id);
                break;
            case MB_TRACE_TYPE_EXIT:
                if (length == 0) {
                    printf(COUNTER_FMT "%s.%d exit\n", counter, obj_name, obj_id);
                } else if (length == 1) {
                    printf(COUNTER_FMT "%s.%d exit %lu\n", counter, obj_name, obj_id, p32[0]);
                } else {
                    JSDRV_LOGW("exit length invalid");
                }
                break;
            case MB_TRACE_TYPE_ALLOC:
                printf(COUNTER_FMT "%s.%d alloc @ %d.%d\n", counter, obj_name, obj_id, file_id, line);
                break;
            case MB_TRACE_TYPE_FREE:
                printf(COUNTER_FMT "%s.%d free @ %d.%d\n", counter, obj_name, obj_id, file_id, line);
                break;
            case MB_TRACE_TYPE_RSV6: break;
            case MB_TRACE_TYPE_RSV7: break;
            case MB_TRACE_TYPE_TIMESYNC: break;
            case MB_TRACE_TYPE_TIMEMAP: break;
            case MB_TRACE_TYPE_FAULT: break;
            case MB_TRACE_TYPE_VALUE: break;
            case MB_TRACE_TYPE_LOG:
                printf(COUNTER_FMT "LOG @ %d.%d\n", counter, file_id, line);
                break;
            case MB_TRACE_TYPE_RSV13: break;
            case MB_TRACE_TYPE_RSV14: break;
            case MB_TRACE_TYPE_OVERFLOW:
                printf(COUNTER_FMT "OVERFLOW %d\n", counter, metadata);
                break;
        }
        p32 += length;
    }

}

int on_adapter(struct app_s * self, int argc, char * argv[]) {
    uint64_t outstanding = 1;
    struct jsdrv_topic_s topic;
    char *device_filter = NULL;

    while (argc) {
        if (argv[0][0] != '-') {
            if (NULL != device_filter) {
                printf("Duplicate device_filter\n");
                return usage();
            }
            device_filter = argv[0];
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

    ROE(jsdrv_open(self->context, self->device.topic, JSDRV_DEVICE_OPEN_MODE_RESUME, JSDRV_TIMEOUT_MS_DEFAULT));

    jsdrv_topic_set(&topic, self->device.topic);
    jsdrv_topic_append(&topic, "h/!trace");
    jsdrv_subscribe(self->context, topic.topic, JSDRV_SFLAG_PUB, on_trace, NULL, 0);

    while (!quit_) {
        Sleep(10);
    }

    jsdrv_close(self->context, self->device.topic, JSDRV_TIMEOUT_MS_DEFAULT);

    return 0;
}
