/*
 * SBS - StreamBox Broadcast System
 * Unit tests for IPC transport (sendmsg/recvmsg with SCM_RIGHTS)
 *
 * Uses socketpair() for testing and memfd_create() as a test fd.
 */
#define _GNU_SOURCE

#include "sbs_test.h"
#include "sbs/ipc.h"
#include "sbs/ipc_transport.h"
#include "sbs/types.h"

#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/socket.h>

/* Helper: create a socketpair and set non-blocking */
static void make_socketpair(int sv[2])
{
    int rc = socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, sv);
    SBS_ASSERT(rc == 0);
}

/* Helper: create a memfd as a test DMA-BUF fd substitute */
static int make_test_fd(void)
{
    int fd = memfd_create("sbs-test", MFD_CLOEXEC);
    SBS_ASSERT(fd >= 0);
    /* Write a pattern so we can verify the fd is the right one */
    uint32_t magic = 0xDEADBEEF;
    ssize_t n = write(fd, &magic, sizeof(magic));
    SBS_ASSERT(n == sizeof(magic));
    return fd;
}

/* Helper: verify the fd contains our test pattern */
static void verify_test_fd(int fd)
{
    SBS_ASSERT(fd >= 0);
    lseek(fd, 0, SEEK_SET);
    uint32_t magic = 0;
    ssize_t n = read(fd, &magic, sizeof(magic));
    SBS_ASSERT(n == sizeof(magic));
    SBS_ASSERT_EQ(magic, (uint32_t)0xDEADBEEF);
}

/* ── Tests ────────────────────────────────────────────────────── */

SBS_TEST(ipc_transport, send_recv_frame_with_fd) {
    int sv[2];
    make_socketpair(sv);

    /* Create test fd and frame message */
    int send_fd = make_test_fd();

    sbs_video_frame_msg_t send_msg;
    sbs_video_frame_msg_init(&send_msg, SBS_IPC_MSG_VIDEO_FRAME);
    send_msg.width    = 1920;
    send_msg.height   = 1080;
    send_msg.pts_ns   = 123456789;
    send_msg.sequence = 42;

    /* Send */
    int rc = sbs_ipc_send_frame(sv[0], &send_msg, send_fd);
    SBS_ASSERT_EQ(rc, SBS_OK);
    close(send_fd);

    /* Receive */
    sbs_video_frame_msg_t recv_msg;
    int recv_fd = -1;
    rc = sbs_ipc_recv_frame(sv[1], &recv_msg, &recv_fd);
    SBS_ASSERT_EQ(rc, SBS_OK);

    /* Verify message contents */
    SBS_ASSERT_EQ(recv_msg.header.msg_type, (uint32_t)SBS_IPC_MSG_VIDEO_FRAME);
    SBS_ASSERT_EQ(recv_msg.width, (uint32_t)1920);
    SBS_ASSERT_EQ(recv_msg.height, (uint32_t)1080);
    SBS_ASSERT_EQ(recv_msg.pts_ns, (uint64_t)123456789);
    SBS_ASSERT_EQ(recv_msg.sequence, (uint64_t)42);

    /* Verify fd was received and contains the right data */
    verify_test_fd(recv_fd);
    close(recv_fd);

    close(sv[0]);
    close(sv[1]);
}

SBS_TEST(ipc_transport, send_frame_no_fd) {
    int sv[2];
    make_socketpair(sv);

    sbs_video_frame_msg_t send_msg;
    sbs_video_frame_msg_init(&send_msg, SBS_IPC_MSG_COMPOSED_FRAME);
    send_msg.width  = 3840;
    send_msg.height = 2160;

    /* Send without fd (-1) */
    int rc = sbs_ipc_send_frame(sv[0], &send_msg, -1);
    SBS_ASSERT_EQ(rc, SBS_OK);

    /* Receive */
    sbs_video_frame_msg_t recv_msg;
    int recv_fd = -1;
    rc = sbs_ipc_recv_frame(sv[1], &recv_msg, &recv_fd);
    SBS_ASSERT_EQ(rc, SBS_OK);
    SBS_ASSERT_EQ(recv_msg.width, (uint32_t)3840);
    SBS_ASSERT_EQ(recv_msg.height, (uint32_t)2160);
    SBS_ASSERT_EQ(recv_fd, -1);  /* No fd received */

    close(sv[0]);
    close(sv[1]);
}

SBS_TEST(ipc_transport, recv_would_block_when_empty) {
    int sv[2];
    make_socketpair(sv);

    sbs_video_frame_msg_t msg;
    int fd = -1;
    int rc = sbs_ipc_recv_frame(sv[1], &msg, &fd);
    SBS_ASSERT_EQ(rc, SBS_ERR_WOULD_BLOCK);

    close(sv[0]);
    close(sv[1]);
}

SBS_TEST(ipc_transport, recv_eof_on_peer_close) {
    int sv[2];
    make_socketpair(sv);

    /* Close sender side */
    close(sv[0]);

    sbs_video_frame_msg_t msg;
    int fd = -1;
    int rc = sbs_ipc_recv_frame(sv[1], &msg, &fd);
    SBS_ASSERT_EQ(rc, SBS_ERR_EOF);

    close(sv[1]);
}

SBS_TEST(ipc_transport, send_recv_control_msg) {
    int sv[2];
    make_socketpair(sv);

    sbs_shutdown_msg_t send_msg;
    sbs_shutdown_msg_init(&send_msg, 2000, 0);

    int rc = sbs_ipc_send_msg(sv[0], &send_msg, sizeof(send_msg));
    SBS_ASSERT_EQ(rc, SBS_OK);

    sbs_shutdown_msg_t recv_msg;
    size_t nread = 0;
    rc = sbs_ipc_recv_msg(sv[1], &recv_msg, sizeof(recv_msg), &nread);
    SBS_ASSERT_EQ(rc, SBS_OK);
    SBS_ASSERT_EQ(nread, sizeof(recv_msg));
    SBS_ASSERT_EQ(recv_msg.header.msg_type, (uint32_t)SBS_IPC_MSG_SHUTDOWN);
    SBS_ASSERT_EQ(recv_msg.grace_period_ms, (uint32_t)2000);
    SBS_ASSERT_EQ(recv_msg.reason, (uint32_t)0);

    close(sv[0]);
    close(sv[1]);
}

SBS_TEST(ipc_transport, send_invalid_args) {
    sbs_video_frame_msg_t msg;
    sbs_video_frame_msg_init(&msg, SBS_IPC_MSG_VIDEO_FRAME);

    SBS_ASSERT_EQ(sbs_ipc_send_frame(-1, &msg, -1), SBS_ERR_INVAL);
    SBS_ASSERT_EQ(sbs_ipc_send_frame(0, NULL, -1), SBS_ERR_INVAL);
}

SBS_TEST(ipc_transport, recv_invalid_args) {
    sbs_video_frame_msg_t msg;
    int fd;
    SBS_ASSERT_EQ(sbs_ipc_recv_frame(-1, &msg, &fd), SBS_ERR_INVAL);
    SBS_ASSERT_EQ(sbs_ipc_recv_frame(0, NULL, &fd), SBS_ERR_INVAL);
    SBS_ASSERT_EQ(sbs_ipc_recv_frame(0, &msg, NULL), SBS_ERR_INVAL);
}

SBS_TEST(ipc_transport, multiple_frames_in_sequence) {
    int sv[2];
    make_socketpair(sv);

    /* Send 3 frames */
    for (uint64_t i = 0; i < 3; i++) {
        int test_fd = make_test_fd();
        sbs_video_frame_msg_t msg;
        sbs_video_frame_msg_init(&msg, SBS_IPC_MSG_VIDEO_FRAME);
        msg.sequence = i;
        msg.width = 1920;

        int rc = sbs_ipc_send_frame(sv[0], &msg, test_fd);
        SBS_ASSERT_EQ(rc, SBS_OK);
        close(test_fd);
    }

    /* Receive 3 frames */
    for (uint64_t i = 0; i < 3; i++) {
        sbs_video_frame_msg_t msg;
        int fd = -1;
        int rc = sbs_ipc_recv_frame(sv[1], &msg, &fd);
        SBS_ASSERT_EQ(rc, SBS_OK);
        SBS_ASSERT_EQ(msg.sequence, i);
        verify_test_fd(fd);
        close(fd);
    }

    close(sv[0]);
    close(sv[1]);
}

SBS_TEST_MAIN()
