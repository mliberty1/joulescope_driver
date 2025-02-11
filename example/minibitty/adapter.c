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


static void on_device_stats(void * user_data, const char * topic, const struct jsdrv_union_s * value) {
    (void) user_data;
    uint32_t * p_u32 = (uint32_t *) value->value.bin;
    printf("%s: %d\n", topic, p_u32[7]);
}

static int usage(void) {
    printf(
        "usage: minibitty adapter [options] [device_filter]\n"
        "options:\n"
        "  none\n"
        );
    return 1;
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

    // todo

    while (!quit_) {
        Sleep(10);
    }

    jsdrv_close(self->context, self->device.topic, JSDRV_TIMEOUT_MS_DEFAULT);

    return 0;
}
