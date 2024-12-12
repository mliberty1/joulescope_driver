/*
* Copyright 2022-2024 Jetperch LLC
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

#define JSDRV_LOG_LEVEL JSDRV_LOG_LEVEL_ALL
#include "jsdrv.h"
#include "jsdrv/error_code.h"
#include "jsdrv_prv/backend.h"
#include "jsdrv_prv/cdef.h"
#include "jsdrv_prv/dbc.h"
#include "jsdrv_prv/frontend.h"
#include "jsdrv_prv/msg_queue.h"
#include "jsdrv_prv/thread.h"
#include <inttypes.h>

#include "mb/comm/frame.h"
#include "mb/comm/link.h"


#define FRAME_SIZE_BYTES           (512U)
#define PAYLOAD_SIZE_U32_MAX       (FRAME_SIZE_BYTES / sizeof(uint32_t) - 3U)

// todo move to an appropriate place
#define MB_USB_EP_BULK_IN  0x82
#define MB_USB_EP_BULK_OUT 0x01

enum state_e {
    ST_NOT_PRESENT = 0,  //
    ST_CLOSED = 1,
    ST_OPENING = 2,
    ST_OPEN = 3,
    ST_CLOSING = 4,
};


struct dev_s {
    struct jsdrvp_ul_device_s ul; // MUST BE FIRST!
    struct jsdrvp_ll_device_s ll;
    struct jsdrv_context_s * context;
    uint16_t out_frame_id;
    uint16_t in_frame_id;
    uint64_t in_frame_count;

    volatile bool do_exit;
    jsdrv_thread_t thread;
    uint8_t state;  // state_e

    struct jsdrv_time_map_s time_map;
};

static const char * prefix_match_and_strip(const char * prefix, const char * topic) {
    while (*prefix) {
        if (*prefix++ != *topic++) {
            return NULL;
        }
    }
    if (*topic++ != '/') {
        return NULL;
    }
    return topic;
}

static int32_t jsdrvb_bulk_in_stream_open(struct dev_s * d) {
    int32_t rv = 0;
    struct jsdrvp_msg_s * m;
    m = jsdrvp_msg_alloc_value(d->context, JSDRV_USBBK_MSG_BULK_IN_STREAM_OPEN, &jsdrv_union_i32(0));
    m->extra.bkusb_stream.endpoint = MB_USB_EP_BULK_IN;
    msg_queue_push(d->ll.cmd_q, m);
    return rv;
}

static int32_t d_open(struct dev_s * d) {
    JSDRV_LOGI("open");
    int32_t rc;
    d->out_frame_id = 0;

    if (d->state == ST_NOT_PRESENT) {
        JSDRV_LOGE("open but not present");
        return JSDRV_ERROR_NOT_FOUND;
    }
    if (d->state != ST_CLOSED) {
        JSDRV_LOGE("open but not closed");
        return JSDRV_ERROR_IN_USE;
    }
    struct jsdrvp_msg_s * m = jsdrvp_msg_alloc_value(d->context, JSDRV_MSG_OPEN, &jsdrv_union_i32(0));
    msg_queue_push(d->ll.cmd_q, m);
    d->state = ST_OPENING;
    jsdrvb_bulk_in_stream_open(d);
    return 0;
}

static int32_t d_close(struct dev_s * d) {
    int32_t rv = 0;
    JSDRV_LOGI("close");
    if ((d->state == ST_OPENING) || (d->state == ST_OPEN)) {
        struct jsdrvp_msg_s * m = jsdrvp_msg_alloc_value(d->context, JSDRV_MSG_CLOSE, &jsdrv_union_i32(0));
        msg_queue_push(d->ll.cmd_q, m);
        d->state = ST_CLOSING;
    }
    return rv;
}

static void send_to_device(struct dev_s * d, enum mb_frame_service_type_e service_type, uint16_t metadata,
                           const uint32_t * data, uint32_t length) {
    JSDRV_DBC_NOT_NULL(data);
    JSDRV_DBC_EQUAL(service_type, service_type & 0x0f);
    if ((length == 0) || (length > PAYLOAD_SIZE_U32_MAX)) {
        JSDRV_LOGE("send_to_device: invalid length %ul", length);
        return;
    }
    uint32_t length_u32 = ((length + 3) >> 2) - 1;
    struct jsdrvp_msg_s * m = jsdrvp_msg_alloc_value(d->context, JSDRV_USBBK_MSG_BULK_OUT_DATA, &jsdrv_union_i32(0));
    m->value.type = JSDRV_UNION_BIN;
    m->value.value.bin = m->payload.bin;
    m->extra.bkusb_stream.endpoint = MB_USB_EP_BULK_OUT;
    m->value.size = 4 * (3 + length_u32 + 1);
    uint8_t * data_u8 = m->payload.bin;
    uint16_t * data_u16 = (uint16_t *) data_u8;
    uint32_t * data_u32 =  (uint32_t *) data_u8;

    data_u8[0] = MB_FRAMER_SOF1;
    data_u8[1] = MB_FRAMER_SOF2 | service_type;
    data_u16[1] = (MB_FRAME_FT_DATA << 11) | (d->out_frame_id & MB_FRAMER_FRAME_ID_MAX);
    d->out_frame_id = (d->out_frame_id + 1) & MB_FRAMER_FRAME_ID_MAX;
    data_u8[4] = length_u32;
    data_u8[5] = mb_frame_length_check(length_u32);
    data_u16[3] = metadata;
    memcpy(&data_u8[8], data, (length_u32 + 1) * 4);
    data_u32[2 + 1 + length_u32] = 0;  // no frame_check on USB
    msg_queue_push(d->ll.cmd_q, m);
}

static bool handle_cmd(struct dev_s * d, struct jsdrvp_msg_s * msg) {
    int32_t rc = 0;
    bool rv = true;
    if (!msg) {
        return false;
    }
    if (d->state == ST_NOT_PRESENT) {
        JSDRV_LOGE("handle_cmd but not present");
    }

    const char * topic = prefix_match_and_strip(d->ll.prefix, msg->topic);
    if (msg->topic[0] == JSDRV_MSG_COMMAND_PREFIX_CHAR) {
        if (0 == strcmp(JSDRV_MSG_FINALIZE, msg->topic)) {
            // full driver shutdown
            d->do_exit = true;
            rv = false;
        } else {
            JSDRV_LOGE("handle_cmd unsupported %s", msg->topic);
        }
    } else if (!topic) {
        JSDRV_LOGE("handle_cmd mismatch %s, %s", msg->topic, d->ll.prefix);
    } else if (topic[0] == JSDRV_MSG_COMMAND_PREFIX_CHAR) {
        if (0 == strcmp(JSDRV_MSG_OPEN, topic)) {
            int32_t opt = 0;
            if ((msg->value.type == JSDRV_UNION_U32) || (msg->value.type == JSDRV_UNION_I32)) {
                opt = msg->value.value.i32;
            }
            d_open(d);
        } else if (0 == strcmp(JSDRV_MSG_CLOSE, topic)) {
            rc = d_close(d);
        } else if (0 == strcmp(JSDRV_MSG_FINALIZE, topic)) {
            // just finalize this upper-level driver (keep lower-level running)
            d->do_exit = true;
            rv = false;
        } else {
            JSDRV_LOGE("handle_cmd unsupported %s", msg->topic);
        }
    //} else if (d->state != ST_OPEN) {
    //    // todo error code.
    } else if ((topic[0] == 'h') && (topic[1] == '/')) {
        if (0 == strcmp("h/link/!ping", topic)) {
            JSDRV_LOGI("PING");
            send_to_device(d, MB_FRAME_ST_LINK, MB_LINK_MSG_PING,
                (uint32_t *) msg->value.value.bin, (msg->value.size + 3) >> 2);
        } else {
            JSDRV_LOGE("topic invalid: %s", msg->topic);
        }
    } else {
        // todo publish.
    }
    return rv;
}

static void handle_in_link(struct dev_s * d, uint16_t metadata, uint32_t * data, uint8_t length) {
    uint8_t msg_type = (uint8_t) (metadata & 0xff);
    switch (msg_type) {
        case MB_LINK_MSG_INVALID:
            JSDRV_LOGW("link msg: invalid");
            break;
        case MB_LINK_MSG_STATUS:
            // todo
            break;
        case MB_LINK_MSG_TIMESYNC_REQ:
            // todo
            break;
        case MB_LINK_MSG_TIMESYNC_RSP:
            JSDRV_LOGW("link msg: timesync response unexpected");
            break;
            // todo
        case MB_LINK_MSG_PING:
            // todo
            break;
        case MB_LINK_MSG_PONG:
            JSDRV_LOGI("pong");
            // todo
            break;
        default:
            JSDRV_LOGW("link msg: unknown %d", msg_type);
            break;
    }
}

static void handle_in_trace(struct dev_s * d, uint16_t metadata, uint32_t * data, uint8_t length) {
    (void) d;
    (void) metadata;
    (void) data;
    (void) length;
    // todo
}

static void handle_in_pubsub(struct dev_s * d, uint16_t metadata, uint32_t * data, uint8_t length) {
    (void) d;
    (void) metadata;
    (void) data;
    (void) length;
    // todo
}

static void handle_stream_in_frame(struct dev_s * d, uint32_t * p_u32) {
    uint8_t * p_u8 = (uint8_t *) p_u32;
    uint16_t * p_u16 = (uint16_t *) p_u32;
    if (p_u8[0] != MB_FRAMER_SOF1) {
        JSDRV_LOGW("frame SOF1 mismatch: 0x%02x", p_u8[0]);
        return;
    }
    if ((p_u8[1] & MB_FRAMER_SOF2_MASK) != MB_FRAMER_SOF2) {
        JSDRV_LOGW("frame SOF2 mismatch: 0x%02x", p_u8[1]);
        return;
    }
    uint8_t service_type = p_u8[1] & ~MB_FRAMER_SOF2_MASK;
    uint16_t frame_id = p_u16[1] & MB_FRAMER_FRAME_ID_MAX;
    uint8_t frame_type = p_u8[3] >> 3;
    if (frame_type != MB_FRAME_FT_DATA) {
        JSDRV_LOGW("unexpected frame type: 0x%02x", frame_type);
        return;
    }
    if (d->in_frame_id != frame_id) {
        JSDRV_LOGW("in frame_id mismatch %d != %d", (int) d->in_frame_id, (int) frame_id);
        // todo keep statistics
    }
    d->in_frame_id = (frame_id + 1) & MB_FRAMER_FRAME_ID_MAX;
    ++d->in_frame_count;

    uint8_t length = p_u8[4];
    uint8_t length_check_expect = mb_frame_length_check(length);
    uint8_t length_check_actual = p_u8[5];
    if (length_check_expect != length_check_actual) {
        JSDRV_LOGW("frame length check mismatch: 0x%02x != 0x%02x", length_check_expect, length_check_actual);
    }
    uint16_t metadata = p_u16[3];

    switch (service_type) {
        case MB_FRAME_ST_INVALID:
            JSDRV_LOGW("invalid service type");
            break;
        case MB_FRAME_ST_LINK:
            handle_in_link(d, metadata, p_u32 + 2, length + 1);
            break;
        case MB_FRAME_ST_TRACE:
            handle_in_trace(d, metadata, p_u32 + 2, length + 1);
            break;
        case MB_FRAME_ST_PUBSUB:
            handle_in_pubsub(d, metadata, p_u32 + 2, length + 1);
            break;
        default:
            JSDRV_LOGW("unsupported service type %d", (int) service_type);
            break;
    }
}

static void handle_stream_in(struct dev_s * d, struct jsdrvp_msg_s * msg) {
    JSDRV_ASSERT(msg->value.type == JSDRV_UNION_BIN);
    uint32_t frame_count = (msg->value.size + FRAME_SIZE_BYTES - 1) / FRAME_SIZE_BYTES;
    for (uint32_t i = 0; i < frame_count; ++i) {
        uint32_t * p_u32 = (uint32_t *) &msg->value.value.bin[i * FRAME_SIZE_BYTES];
        handle_stream_in_frame(d, p_u32);
    }
}

static bool handle_rsp(struct dev_s * d, struct jsdrvp_msg_s * msg) {
    bool rv = true;
    if (!msg) {
        return false;
    }
    if (0 == strcmp(JSDRV_USBBK_MSG_STREAM_IN_DATA, msg->topic)) {
        JSDRV_LOGD3("stream_in_data sz=%d", (int) msg->value.size);
        handle_stream_in(d, msg);
        msg_queue_push(d->ll.cmd_q, msg);  // return
        return true;
    } else if (0 == strcmp(JSDRV_USBBK_MSG_BULK_OUT_DATA, msg->topic)) {
        JSDRV_LOGD2("stream_out_data done");
        // no action necessary
    } else if (msg->topic[0] == JSDRV_MSG_COMMAND_PREFIX_CHAR) {
        if (0 == strcmp(JSDRV_MSG_FINALIZE, msg->topic)) {
            d->do_exit = true;
            rv = false;
        } else {
            JSDRV_LOGE("handle_rsp unsupported %s", msg->topic);
        }
    } else {
        JSDRV_LOGE("handle_rsp unsupported %s", msg->topic);
    }
    jsdrvp_msg_free(d->context, msg);
    return rv;
}

static THREAD_RETURN_TYPE driver_thread(THREAD_ARG_TYPE lpParam) {
    struct jsdrvp_msg_s * msg;
    struct dev_s *d = (struct dev_s *) lpParam;
    JSDRV_LOGI("JS220 USB upper-level thread started for %s", d->ll.prefix);
    d->state = ST_CLOSED;

#if _WIN32
    HANDLE handles[MAXIMUM_WAIT_OBJECTS];
    DWORD handle_count;
    handles[0] = msg_queue_handle_get(d->ul.cmd_q);
    handles[1] = msg_queue_handle_get(d->ll.rsp_q);
    handle_count = 2;
#else
    struct pollfd fds[2];
    fds[0].fd = msg_queue_handle_get(d->ul.cmd_q);
    fds[0].events = POLLIN;
    fds[1].fd = msg_queue_handle_get(d->ll.rsp_q);
    fds[1].events = POLLIN;
#endif

    while (!d->do_exit) {
#if _WIN32
        WaitForMultipleObjects(handle_count, handles, false, 5000);
#else
        poll(fds, 2, 2);
#endif
        JSDRV_LOGD2("ul thread tick");
        while (handle_cmd(d, msg_queue_pop_immediate(d->ul.cmd_q))) {
            ;
        }
        // note: ResetEvent handled automatically by msg_queue_pop_immediate
        while (handle_rsp(d, msg_queue_pop_immediate(d->ll.rsp_q))) {
            ;
        }
    }

    JSDRV_LOGI("JS220 USB upper-level thread done %s", d->ll.prefix);
    THREAD_RETURN();
}

static void join(struct jsdrvp_ul_device_s * device) {
    struct dev_s * d = (struct dev_s *) device;
    jsdrvp_send_finalize_msg(d->context, d->ul.cmd_q, "");
    // and wait for thread to exit.
    jsdrv_thread_join(&d->thread, 1000);
    jsdrv_free(d);
}

int32_t jsdrvp_ul_mb_device_usb_factory(struct jsdrvp_ul_device_s ** device, struct jsdrv_context_s * context, struct jsdrvp_ll_device_s * ll) {
    JSDRV_DBC_NOT_NULL(device);
    JSDRV_DBC_NOT_NULL(context);
    JSDRV_DBC_NOT_NULL(ll);
    *device = NULL;
    struct dev_s * d = jsdrv_alloc_clr(sizeof(struct dev_s));
    JSDRV_LOGI("jsdrvp_ul_mb_device_factory %p", d);
    d->context = context;
    d->ll = *ll;
    d->ul.cmd_q = msg_queue_init();
    d->ul.join = join;
    if (jsdrv_thread_create(&d->thread, driver_thread, d, 1)) {
        return JSDRV_ERROR_UNSPECIFIED;
    }
    *device = &d->ul;
    return 0;
}
