/*
 * Copyright 2023 Jetperch LLC
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

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "jsdrv.h"
#include "jsdrv_prv/buffer.h"
#include "jsdrv_prv/cdef.h"
#include "jsdrv/cstr.h"
#include "jsdrv_prv/frontend.h"
#include "jsdrv_prv/list.h"
#include "jsdrv_prv/msg_queue.h"
#include "tinyprintf.h"
//#include "test.inc"

// copied from jsdrv.c, line 55
static const size_t STREAM_MSG_SZ = sizeof(struct jsdrvp_msg_s) - sizeof(union jsdrvp_payload_u) + sizeof(struct jsdrv_stream_signal_s);


struct sub_s {
    char topic[JSDRV_TOPIC_LENGTH_MAX];
    uint8_t flags;
    jsdrv_pubsub_subscribe_fn cbk_fn;
    void * cbk_user_data;
    struct jsdrv_list_s item;
};

struct jsdrv_context_s {
    struct msg_queue_s * msg_sent;
    struct jsdrv_list_s subscribers;
};

struct jsdrvp_msg_s * jsdrvp_msg_alloc(struct jsdrv_context_s * context) {
    (void) context;
    struct jsdrvp_msg_s * m = calloc(1, sizeof(struct jsdrvp_msg_s));
    jsdrv_list_initialize(&m->item);
    m->inner_msg_type = JSDRV_MSG_TYPE_NORMAL;
    return m;
}

struct jsdrvp_msg_s * jsdrvp_msg_alloc_data(struct jsdrv_context_s * context, const char * topic) {
    (void) context;
    struct jsdrvp_msg_s * m = calloc(1, STREAM_MSG_SZ);
    jsdrv_list_initialize(&m->item);
    m->inner_msg_type = JSDRV_MSG_TYPE_DATA;
    jsdrv_cstr_copy(m->topic, topic, sizeof(m->topic));
    m->value = jsdrv_union_bin(&m->payload.bin[0], 0);
    return m;
}

struct jsdrvp_msg_s * jsdrvp_msg_alloc_value(struct jsdrv_context_s * context, const char * topic, const struct jsdrv_union_s * value) {
    struct jsdrvp_msg_s * m = jsdrvp_msg_alloc(context);
    jsdrv_cstr_copy(m->topic, topic, sizeof(m->topic));
    m->value = *value;
    m->value.flags &= ~JSDRV_UNION_FLAG_HEAP_MEMORY;

    switch (value->type) {
        case JSDRV_UNION_JSON:  /* intentional fall-through */
        case JSDRV_UNION_STR:
            if (m->value.size == 0) {
                m->value.size = (uint32_t) (strlen(value->value.str) + 1);
            }
            /* intentional fall-through */
        case JSDRV_UNION_BIN:
            if (value->size > sizeof(m->payload.bin)) {
                uint8_t * ptr = malloc(value->size);
                memcpy(ptr, value->value.bin, value->size);
                m->value.value.bin = ptr;
                m->value.flags |= JSDRV_UNION_FLAG_HEAP_MEMORY;
            } else {
                m->value.value.bin = m->payload.bin;
                memcpy(m->payload.bin, value->value.bin, m->value.size);
            }
            break;
        default:
            break;
    }
    return m;
}

struct jsdrvp_msg_s * jsdrvp_msg_clone(struct jsdrv_context_s * context, const struct jsdrvp_msg_s * msg_src) {
    struct jsdrvp_msg_s * m;
    if (msg_src->inner_msg_type == JSDRV_MSG_TYPE_DATA) {
        m = jsdrvp_msg_alloc_data(context, msg_src->topic);
        m->value = msg_src->value;
        m->value.value.bin = &m->payload.bin[0];
        m->payload = msg_src->payload;
    } else {
        m = jsdrvp_msg_alloc(context);
        *m = *msg_src;
        switch (m->value.type) {
            case JSDRV_UNION_JSON:  // intentional fall-through
            case JSDRV_UNION_STR:
                m->value.value.str = m->payload.str;
                break;
            case JSDRV_UNION_BIN:
                if (m->value.flags & JSDRV_UNION_FLAG_HEAP_MEMORY) {
                    uint8_t *ptr = malloc(m->value.size);
                    memcpy(ptr, m->value.value.bin, m->value.size);
                    m->value.value.bin = ptr;
                } else {
                    m->value.value.bin = m->payload.bin;
                }
                break;
            default:
                break;
        }
    }
    jsdrv_list_initialize(&m->item);
    return m;
}

void jsdrvp_msg_free(struct jsdrv_context_s * context, struct jsdrvp_msg_s * msg) {
    (void) context;
    if (msg->value.flags & JSDRV_UNION_FLAG_HEAP_MEMORY) {
        free((void *) msg->value.value.bin);
    }
    free(msg);
}

static void subscribe(struct jsdrv_context_s * context, struct jsdrvp_msg_s * msg) {
    struct sub_s * s = calloc(1, sizeof(struct sub_s));
    jsdrv_cstr_copy(s->topic, msg->payload.sub.topic, sizeof(s->topic));
    s->cbk_fn = msg->payload.sub.subscriber.internal_fn;
    s->cbk_user_data = msg->payload.sub.subscriber.user_data;
    s->flags = msg->payload.sub.subscriber.flags;
    jsdrv_list_initialize(&s->item);
    jsdrv_list_add_tail(&context->subscribers, &s->item);
}

static void unsubscribe(struct jsdrv_context_s * context, struct jsdrvp_msg_s * msg) {
    struct jsdrv_list_s * item;
    struct sub_s * s;
    jsdrv_list_foreach(&context->subscribers, item) {
        s = JSDRV_CONTAINER_OF(item, struct sub_s, item);
        if ((0 == strcmp(s->topic, msg->payload.sub.topic)) &&
                (s->cbk_fn == msg->extra.frontend.subscriber.internal_fn) &&
                (s->cbk_user_data == msg->extra.frontend.subscriber.user_data)) {
            jsdrv_list_remove(item);
            free(s);
        }
    }
}

void jsdrvp_backend_send(struct jsdrv_context_s * context, struct jsdrvp_msg_s * msg) {
    msg_queue_push(context->msg_sent, msg);
}

static void msg_send_process_next(struct jsdrv_context_s * context, uint32_t timeout_ms) {
    struct jsdrvp_msg_s * msg = NULL;
    assert_int_equal(0, msg_queue_pop(context->msg_sent, &msg, timeout_ms));
    if (0 == strcmp(JSDRV_PUBSUB_SUBSCRIBE, msg->topic)) {
        char * topic = msg->payload.sub.topic;
        check_expected_ptr(topic);
        subscribe(context, msg);
    } else if (0 == strcmp(JSDRV_PUBSUB_UNSUBSCRIBE, msg->topic)) {
        char * topic = msg->payload.sub.topic;
        check_expected_ptr(topic);
        unsubscribe(context, msg);
    } else if (jsdrv_cstr_ends_with(msg->topic, "$")) {
        const char * meta_topic = msg->topic;
        check_expected_ptr(meta_topic);
    } else if (0 == strcmp(JSDRV_BUFFER_MGR_MSG_ACTION_LIST, msg->topic)) {
        const size_t buf_list_length = msg->value.size;
        const uint8_t *buf_list_buffers = msg->value.value.bin;
        check_expected(buf_list_length);
        check_expected_ptr(buf_list_buffers);
    } else if (jsdrv_cstr_starts_with(msg->topic, "m/+/")) {
        // unknown topic, not supported
        assert_true(0);
    } else if (jsdrv_cstr_starts_with(msg->topic, "m/")) {
        char buffer_id_str[4] = {0, 0, 0, 0};
        uint32_t buffer_id = 0;
        char * ch = &msg->topic[2];
        for (int i = 0; i < 3; ++i) {
            buffer_id_str[i] = ch[i];
        }
        assert_int_equal('/', ch[3]);
        assert_int_equal(0, jsdrv_cstr_to_u32(buffer_id_str, &buffer_id));
        ch += 4;
        if (0 == strcmp(JSDRV_BUFFER_MSG_LIST, ch)) {
            const size_t sig_list_length = msg->value.size;
            const uint8_t *sig_list_buffers = msg->value.value.bin;
            check_expected(sig_list_length);
            check_expected_ptr(sig_list_buffers);
        } else {
            // unknown topic, not supported
            assert_true(0);
        }
    } else {
        // ???
    }
    jsdrvp_msg_free(context, msg);
}

#define expect_meta(topic__) expect_string(msg_send_process_next, meta_topic, topic__)

#define expect_subscribe(topic_) \
    expect_string(msg_send_process_next, topic, topic_);

#define expect_unsubscribe(topic_) \
    expect_string(msg_send_process_next, topic, topic_);

#define expect_buf_list(ex_list, ex_len) \
    expect_value(msg_send_process_next, buf_list_length, ex_len); \
    expect_memory(msg_send_process_next, buf_list_buffers, ex_list, ex_len)

#define expect_sig_list(ex_list, ex_len) \
    expect_value(msg_send_process_next, sig_list_length, ex_len); \
    expect_memory(msg_send_process_next, sig_list_buffers, ex_list, ex_len)

struct jsdrv_context_s * initialize() {
    uint8_t ex_list_buffer[] = {0};
    struct jsdrv_context_s * context = malloc(sizeof(struct jsdrv_context_s));
    memset(context, 0, sizeof(*context));
    context->msg_sent = msg_queue_init();
    assert_non_null(context->msg_sent);
    jsdrv_list_initialize(&context->subscribers);
    assert_int_equal(0, jsdrv_buffer_initialize(context));

    expect_meta(JSDRV_BUFFER_MGR_MSG_ACTION_ADD "$");
    msg_send_process_next(context, 100);
    expect_meta(JSDRV_BUFFER_MGR_MSG_ACTION_REMOVE "$");
    msg_send_process_next(context, 100);
    expect_meta(JSDRV_BUFFER_MGR_MSG_ACTION_LIST "$");
    msg_send_process_next(context, 100);
    expect_subscribe(JSDRV_BUFFER_MGR_MSG_ACTION_ADD);
    msg_send_process_next(context, 100);
    expect_subscribe(JSDRV_BUFFER_MGR_MSG_ACTION_REMOVE);
    msg_send_process_next(context, 100);
    expect_buf_list(ex_list_buffer, sizeof(ex_list_buffer));
    msg_send_process_next(context, 100);

    return context;
}

void finalize(struct jsdrv_context_s * context) {
    struct jsdrv_list_s * item;
    jsdrv_buffer_finalize();
    expect_unsubscribe(JSDRV_BUFFER_MGR_MSG_ACTION_ADD);
    msg_send_process_next(context, 100);
    expect_unsubscribe(JSDRV_BUFFER_MGR_MSG_ACTION_REMOVE);
    msg_send_process_next(context, 100);

    while (jsdrv_list_length(&context->subscribers)) {
        item = jsdrv_list_remove_head(&context->subscribers);
        struct sub_s * sub = JSDRV_CONTAINER_OF(item, struct sub_s, item);
        free(sub);
    }
    free(context);
}

int32_t publish(struct jsdrv_context_s * context, struct jsdrvp_msg_s * msg) {
    struct jsdrv_list_s * item;
    char topic[JSDRV_TOPIC_LENGTH_MAX];
    char * t;
    jsdrv_cstr_copy(topic, msg->topic, sizeof(topic));
    t = &topic[strlen(topic)];
    while (1) {
        jsdrv_list_foreach(&context->subscribers, item) {
            struct sub_s *sub = JSDRV_CONTAINER_OF(item, struct sub_s, item);
            if (0 == strcmp(topic, sub->topic)) {
                sub->cbk_fn(sub->cbk_user_data, msg);
            }
        }
        while (1) {
            --t;
            if (t <= topic) {
                return 0;
            }
            if (*t == '/') {
                *t = 0;
                break;
            }
        }
    }
}

static void test_initialize_finalize(void **state) {
    (void) state;
    struct jsdrv_context_s * context = initialize();
    finalize(context);
}

static void test_add_remove(void **state) {
    (void) state;
    uint8_t ex_list_buffer0[] = {0};
    uint8_t ex_list_buffer1[] = {3, 0};
    struct jsdrv_context_s * context = initialize();
    publish(context, jsdrvp_msg_alloc_value(context, JSDRV_BUFFER_MGR_MSG_ACTION_ADD, &jsdrv_union_u8(3)));

    expect_subscribe("m/003");
    msg_send_process_next(context, 100);
    expect_buf_list(ex_list_buffer1, sizeof(ex_list_buffer1));
    msg_send_process_next(context, 100);

    publish(context, jsdrvp_msg_alloc_value(context, JSDRV_BUFFER_MGR_MSG_ACTION_REMOVE, &jsdrv_union_u8(3)));
    expect_unsubscribe("m/003");
    msg_send_process_next(context, 100);
    expect_buf_list(ex_list_buffer0, sizeof(ex_list_buffer0));
    msg_send_process_next(context, 100);

    finalize(context);
}

static void test_one_signal(void **state) {
    (void) state;
    struct jsdrvp_msg_s * msg;
    const uint8_t buffer_id = 3;
    const uint8_t signal_id = 5;
    uint8_t ex_list_buffer0[] = {0};
    uint8_t ex_list_buffer1[] = {buffer_id, 0};
    uint8_t ex_list_sig0[] = {0};
    uint8_t ex_list_sig1[] = {signal_id, 0};

    struct jsdrv_context_s * context = initialize();
    publish(context, jsdrvp_msg_alloc_value(context, JSDRV_BUFFER_MGR_MSG_ACTION_ADD, &jsdrv_union_u8(buffer_id)));
    expect_subscribe("m/003");
    msg_send_process_next(context, 100);
    expect_buf_list(ex_list_buffer1, sizeof(ex_list_buffer1));
    msg_send_process_next(context, 100);

    msg = jsdrvp_msg_alloc_value(context, "", &jsdrv_union_u8(signal_id));
    tfp_snprintf(msg->topic, sizeof(msg->topic), "m/%03u/%s", buffer_id, JSDRV_BUFFER_MSG_ACTION_SIGNAL_ADD);
    publish(context, msg);
    expect_sig_list(ex_list_sig1, sizeof(ex_list_sig1));
    msg_send_process_next(context, 100);

    msg = jsdrvp_msg_alloc_value(context, "", &jsdrv_union_str("u/js220/0123456/s/i/!data"));
    tfp_snprintf(msg->topic, sizeof(msg->topic), "m/%03u/s/%03u/s/topic", buffer_id, signal_id);
    publish(context, msg);
    expect_subscribe("u/js220/0123456/s/i/!data");
    msg_send_process_next(context, 100);

    // tear down
    msg = jsdrvp_msg_alloc_value(context, "", &jsdrv_union_u8(signal_id));
    tfp_snprintf(msg->topic, sizeof(msg->topic), "m/%03u/%s", buffer_id, JSDRV_BUFFER_MSG_ACTION_SIGNAL_REMOVE);
    publish(context, msg);
    expect_unsubscribe("u/js220/0123456/s/i/!data");
    msg_send_process_next(context, 100);
    expect_sig_list(ex_list_sig0, sizeof(ex_list_sig0));
    msg_send_process_next(context, 100);

    publish(context, jsdrvp_msg_alloc_value(context, JSDRV_BUFFER_MGR_MSG_ACTION_REMOVE, &jsdrv_union_u8(buffer_id)));
    expect_unsubscribe("m/003");
    msg_send_process_next(context, 100);
    expect_buf_list(ex_list_buffer0, sizeof(ex_list_buffer0));
    msg_send_process_next(context, 100);

    finalize(context);
}


int main(void) {
    const struct CMUnitTest tests[] = {
            cmocka_unit_test(test_initialize_finalize),
            cmocka_unit_test(test_add_remove),
            cmocka_unit_test(test_one_signal),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
