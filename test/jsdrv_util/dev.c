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

#include "jsdrv_util_prv.h"
#include <stdio.h>
#include <string.h>

static int usage() {
    printf("usage: jsdrv_util dev [--device {device_path}]} [--reset {app|update1|update2}]\n");
    return 1;
}

static void on_pub(void * user_data, const char * topic, const struct jsdrv_union_s * value) {
    struct app_s * self = (struct app_s *) user_data;
    (void) self;
    char buf[32];
    jsdrv_union_value_to_str(value, buf, sizeof(buf), 1);
    printf("on_pub(%s, %s)\n", topic, buf);
}

int on_dev(struct app_s * self, int argc, char * argv[]) {
    char * device = NULL;
    char * reset = NULL;
    printf("CAUTION: developer tools - not intended for normal operation!\n");

    while (argc) {
        if (argv[0][0] != '-') {
            return usage();
        } else if (0 == strcmp(argv[0], "--device")) {
            ARG_CONSUME();
            ARG_REQUIRE();
            device = argv[0];
            ARG_CONSUME();
        } else if (0 == strcmp(argv[0], "--reset")) {
            ARG_CONSUME();
            ARG_REQUIRE();
            reset = argv[0];
            ARG_CONSUME();
        } else if ((0 == strcmp(argv[0], "--verbose")) || (0 == strcmp(argv[0], "-v"))) {
            self->verbose++;
            ARG_CONSUME();
        } else {
            return usage();
        }
    }

    ROE(app_match(self, device));

    struct jsdrv_topic_s topic;
    jsdrv_topic_set(&topic, self->device.topic);

    jsdrv_topic_set(&topic, self->device.topic);
    jsdrv_topic_append(&topic, JSDRV_MSG_OPEN);
    ROE(jsdrv_publish(self->context, topic.topic, &jsdrv_union_i32(JSDRV_DEVICE_OPEN_MODE_RESUME), JSDRV_TIMEOUT_MS_DEFAULT));

    if (NULL != reset) {
        jsdrv_topic_set(&topic, self->device.topic);
        jsdrv_topic_append(&topic, "h/!reset");
        ROE(jsdrv_publish(self->context, topic.topic, &jsdrv_union_cstr(reset), JSDRV_TIMEOUT_MS_DEFAULT));
    }

    jsdrv_topic_set(&topic, self->device.topic);
    jsdrv_topic_append(&topic, JSDRV_MSG_CLOSE);
    ROE(jsdrv_publish(self->context, topic.topic, &jsdrv_union_i32(0), JSDRV_TIMEOUT_MS_DEFAULT));
    return 0;
}