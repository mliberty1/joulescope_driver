/*
 * Copyright 2022 Jetperch LLC
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
#include "jsdrv/version.h"
#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include <windows.h>  // todo remove

#define PING_SIZE_U32 ((512U - 12U) >> 2)


static int link_lookback(struct app_s * self, const char * device) {
    ROE(jsdrv_open(self->context, device, JSDRV_DEVICE_OPEN_MODE_RESUME, 0));
    Sleep(100);

    jsdrv_topic_set(&self->topic, self->device.topic);
    jsdrv_topic_append(&self->topic, "h/link/!ping");

    for (int k = 0; k < 10; ++k) {
        uint32_t offset = k * 200;
        uint32_t ping_data[PING_SIZE_U32];
        for (uint32_t i = 0; i < PING_SIZE_U32; ++i) {
            ping_data[i] = offset + i;
        }
        jsdrv_publish(self->context, self->topic.topic, &jsdrv_union_bin((uint8_t *) ping_data, sizeof(ping_data)), 0);
        Sleep(100);
    }

    Sleep(1000);

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
        } else {
            return usage();
        }
    }

    if (NULL == device_filter) {
        printf("device_filter required\n");
        return usage();
    }

    ROE(app_match(self, device_filter));


    // todo loopback @ pubsub layer

    return link_lookback(self, self->device.topic);
}
