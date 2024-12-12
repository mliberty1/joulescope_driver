/*
 * Copyright 2014-2024 Jetperch LLC
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

/**
 * @file
 *
 * @brief Message communication frame format.
 */

#ifndef MB_COMM_FRAME_H_
#define MB_COMM_FRAME_H_

#include "mb/cdef.h"
#include <stdint.h>

MB_CPP_GUARD_START

/// The value for the first start of frame byte.
#define MB_FRAMER_SOF1 ((uint8_t) 0x55)
/// The value for the second start of frame nibble.
#define MB_FRAMER_SOF2 ((uint8_t) 0x00)
/// The mask for SOF2
#define MB_FRAMER_SOF2_MASK ((uint8_t) 0xF0)
/// The framer header size in total_bytes.
#define MB_FRAMER_HEADER_SIZE (8)
/// The maximum payload length in words.
#define MB_FRAMER_PAYLOAD_WORDS_MAX (256)
/// The maximum payload length in words.
#define MB_FRAMER_PAYLOAD_BYTES_MAX (MB_FRAMER_PAYLOAD_WORDS_MAX * 4)
/// The framer footer size in total_bytes.
#define MB_FRAMER_FOOTER_SIZE (4)
/// The framer total maximum data size in bytes
#define MB_FRAMER_MAX_SIZE (\
    MB_FRAMER_HEADER_SIZE + \
    MB_FRAMER_PAYLOAD_MAX_SIZE + \
    MB_FRAMER_FOOTER_SIZE)
/// The framer link message (ACK) size in bytes
#define MB_FRAMER_LINK_SIZE (8)
#define MB_FRAMER_OVERHEAD_SIZE (MB_FRAMER_HEADER_SIZE + MB_FRAMER_FOOTER_SIZE)
#define MB_FRAMER_FRAME_ID_MAX ((1U << 11) - 1U)

/**
 * @brief The frame types.
 *
 * The 5-bit frame type values are carefully selected to ensure minimum
 * likelihood that a data frame is detected as a ACK frame.
 */
enum mb_frame_type_e {
    MB_FRAME_FT_DATA = 0x00,
    MB_FRAME_FT_ACK_ALL = 0x0F,
    MB_FRAME_FT_ACK_ONE = 0x17,
    MB_FRAME_FT_NACK_FRAME_ID = 0x1B,
    MB_FRAME_FT_NACK_FRAMING_ERROR = 0x1D,  // next expect frame_id
    MB_FRAME_FT_RESET = 0x1E,
};

/**
 * @brief The service type for data frames.
 *
 * Service types usually use the metadata field for additional
 * payload identification.
 */
enum mb_frame_service_type_e {
    MB_FRAME_ST_INVALID = 0,             ///< reserved, additional differentiation from link frames
    MB_FRAME_ST_LINK = 1,                ///< Link-layer message: see os/comm/link.h
    MB_FRAME_ST_TRACE = 2,               ///< Trace TIMER signal, TASK ready, invalid for other types
    MB_FRAME_ST_PUBSUB = 3,              ///< PubSub publish message
};

/**
* @brief Compute the length check field.
*
* @param length The length field value, which is ((size + 3) >> 2) - 1.
* @return The value for the length_check field.
*/
static inline uint8_t mb_frame_length_check(uint8_t length) {
    return (uint8_t) ((length * (uint32_t) 0xd8d9) >> 11);
}

MB_CPP_GUARD_END

/** @} */

#endif  /* MB_COMM_FRAME_H_ */
