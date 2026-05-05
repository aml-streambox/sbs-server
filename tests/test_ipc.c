/*
 * SBS - StreamBox Broadcast System
 * Unit tests for IPC protocol
 */
#include "sbs_test.h"
#include "sbs/ipc.h"

#include <string.h>
#include <stdbool.h>

SBS_TEST(ipc, header_init_sets_fields) {
    sbs_ipc_msg_header_t hdr;
    sbs_ipc_header_init(&hdr, SBS_IPC_MSG_VIDEO_FRAME, 1024);

    SBS_ASSERT_EQ(hdr.msg_type, (uint32_t)SBS_IPC_MSG_VIDEO_FRAME);
    SBS_ASSERT_EQ(hdr.payload_size, (uint32_t)1024);
}

SBS_TEST(ipc, header_valid_accepts_video_frame) {
    sbs_ipc_msg_header_t hdr;
    sbs_ipc_header_init(&hdr, SBS_IPC_MSG_VIDEO_FRAME, 0);
    SBS_ASSERT(sbs_ipc_header_valid(&hdr));
}

SBS_TEST(ipc, header_valid_accepts_status) {
    sbs_ipc_msg_header_t hdr;
    sbs_ipc_header_init(&hdr, SBS_IPC_MSG_STATUS, 0);
    SBS_ASSERT(sbs_ipc_header_valid(&hdr));
}

SBS_TEST(ipc, header_valid_accepts_shutdown) {
    sbs_ipc_msg_header_t hdr;
    sbs_ipc_header_init(&hdr, SBS_IPC_MSG_SHUTDOWN, 0);
    SBS_ASSERT(sbs_ipc_header_valid(&hdr));
}

SBS_TEST(ipc, header_valid_accepts_signal_change) {
    sbs_ipc_msg_header_t hdr;
    sbs_ipc_header_init(&hdr, SBS_IPC_MSG_SIGNAL_CHANGE, 0);
    SBS_ASSERT(sbs_ipc_header_valid(&hdr));
}

SBS_TEST(ipc, header_valid_rejects_null) {
    SBS_ASSERT(!sbs_ipc_header_valid(NULL));
}

SBS_TEST(ipc, header_valid_rejects_bad_type) {
    sbs_ipc_msg_header_t hdr;
    sbs_ipc_header_init(&hdr, SBS_IPC_MSG_STATUS, 0);
    hdr.msg_type = 0x9999;
    SBS_ASSERT(!sbs_ipc_header_valid(&hdr));
}

SBS_TEST(ipc, header_valid_rejects_zero_type) {
    sbs_ipc_msg_header_t hdr = { .msg_type = 0, .payload_size = 0 };
    SBS_ASSERT(!sbs_ipc_header_valid(&hdr));
}

SBS_TEST(ipc, video_frame_msg_is_128_bytes) {
    SBS_ASSERT_EQ((uint32_t)sizeof(sbs_video_frame_msg_t), (uint32_t)128);
}

SBS_TEST(ipc, video_frame_msg_init) {
    sbs_video_frame_msg_t msg;
    sbs_video_frame_msg_init(&msg, SBS_IPC_MSG_VIDEO_FRAME);

    SBS_ASSERT_EQ(msg.header.msg_type, (uint32_t)SBS_IPC_MSG_VIDEO_FRAME);
    SBS_ASSERT_EQ(msg.header.payload_size,
                  (uint32_t)(sizeof(msg) - sizeof(msg.header)));
    SBS_ASSERT_EQ(msg.pts_ns, (uint64_t)0);
    SBS_ASSERT_EQ(msg.width, (uint32_t)0);
    SBS_ASSERT_EQ(msg.flags, (uint32_t)0);
}

SBS_TEST(ipc, status_msg_init) {
    sbs_status_msg_t msg;
    sbs_status_msg_init(&msg);

    SBS_ASSERT_EQ(msg.header.msg_type, (uint32_t)SBS_IPC_MSG_STATUS);
    SBS_ASSERT_EQ(msg.state, (uint32_t)SBS_WORKER_STATE_INIT);
}

SBS_TEST(ipc, shutdown_msg_init) {
    sbs_shutdown_msg_t msg;
    sbs_shutdown_msg_init(&msg, 2000, 1);

    SBS_ASSERT_EQ(msg.header.msg_type, (uint32_t)SBS_IPC_MSG_SHUTDOWN);
    SBS_ASSERT_EQ(msg.grace_period_ms, (uint32_t)2000);
    SBS_ASSERT_EQ(msg.reason, (uint32_t)1);
}

SBS_TEST_MAIN()
