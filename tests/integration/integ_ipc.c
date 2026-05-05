/**
 * integ_ipc.c — Integration test for IPC frame delivery pipeline.
 *
 * Tests the full path: mock source worker → IPC transport (SCM_RIGHTS) →
 * frame reception → frame slot update.
 *
 * Uses socketpair(AF_UNIX, SOCK_SEQPACKET) to simulate the supervisor-worker
 * link, forks a child that sends video frame messages with memfd-backed fds,
 * and verifies the parent receives them and publishes to a frame slot.
 *
 * Does NOT require Vulkan or GStreamer. Runs on host and target.
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <dirent.h>
#include <time.h>

#include <glib.h>
#include <glib-unix.h>

#include <sbs/log.h>
#include <sbs/types.h>
#include <sbs/ipc.h>
#include <sbs/ipc_transport.h>
#include <sbs/frame_slot.h>

/* ── Helpers ──────────────────────────────────────────────────── */

static int count_open_fds(void)
{
    DIR *d = opendir("/proc/self/fd");
    if (!d) return -1;
    int count = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] != '.') count++;
    }
    closedir(d);
    /* Subtract 1 for the dirfd used by opendir */
    return count - 1;
}

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/** Create a memfd with known pattern data. Caller owns the fd. */
static int make_test_memfd(uint32_t seq, size_t size)
{
    int fd = memfd_create("test-frame", MFD_CLOEXEC);
    if (fd < 0) return -1;

    if (ftruncate(fd, (off_t)size) < 0) {
        close(fd);
        return -1;
    }

    /* Write a recognizable pattern: 4-byte sequence number repeated */
    uint8_t *p = mmap(NULL, size, PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        close(fd);
        return -1;
    }
    for (size_t i = 0; i + 4 <= size; i += 4) {
        memcpy(p + i, &seq, 4);
    }
    munmap(p, size);

    return fd;
}

/** Verify a memfd contains the expected pattern. */
static bool verify_memfd_pattern(int fd, uint32_t expected_seq, size_t size)
{
    uint8_t *p = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) return false;

    bool ok = true;
    for (size_t i = 0; i + 4 <= size; i += 4) {
        uint32_t val;
        memcpy(&val, p + i, 4);
        if (val != expected_seq) {
            ok = false;
            break;
        }
    }
    munmap(p, size);
    return ok;
}

/* ── Test 1: Single frame through socketpair ─────────────────── */

static int test_single_frame(void)
{
    printf("  [TEST] single frame send/recv with fd passing...\n");

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv) < 0) {
        printf("  [FAIL] socketpair: %s\n", strerror(errno));
        return 1;
    }

    /* Make both ends non-blocking */
    fcntl(sv[0], F_SETFL, fcntl(sv[0], F_GETFL) | O_NONBLOCK);
    fcntl(sv[1], F_SETFL, fcntl(sv[1], F_GETFL) | O_NONBLOCK);

    /* Create test frame message */
    sbs_video_frame_msg_t send_msg;
    sbs_video_frame_msg_init(&send_msg, SBS_IPC_MSG_VIDEO_FRAME);
    send_msg.width       = 1920;
    send_msg.height      = 1080;
    send_msg.drm_format  = 0x34324241; /* DRM_FORMAT_ABGR8888 */
    send_msg.sequence    = 42;
    send_msg.pts_ns      = 16666666;
    send_msg.n_planes    = 1;
    send_msg.plane_stride[0] = 1920 * 4;

    int test_fd = make_test_memfd(42, 4096);
    if (test_fd < 0) {
        printf("  [FAIL] memfd_create\n");
        close(sv[0]); close(sv[1]);
        return 1;
    }

    /* Send frame on sv[0] */
    int rc = sbs_ipc_send_frame(sv[0], &send_msg, test_fd);
    close(test_fd); /* sender can close after send — fd is dup'd by kernel */

    if (rc != SBS_OK) {
        printf("  [FAIL] send_frame returned %d\n", rc);
        close(sv[0]); close(sv[1]);
        return 1;
    }

    /* Receive frame on sv[1] */
    sbs_video_frame_msg_t recv_msg;
    int recv_fd = -1;
    rc = sbs_ipc_recv_frame(sv[1], &recv_msg, &recv_fd);

    if (rc != SBS_OK) {
        printf("  [FAIL] recv_frame returned %d\n", rc);
        close(sv[0]); close(sv[1]);
        return 1;
    }

    /* Verify message fields */
    bool pass = true;
    if (recv_msg.width != 1920)      { printf("  [FAIL] width mismatch\n"); pass = false; }
    if (recv_msg.height != 1080)     { printf("  [FAIL] height mismatch\n"); pass = false; }
    if (recv_msg.sequence != 42)     { printf("  [FAIL] sequence mismatch\n"); pass = false; }
    if (recv_msg.pts_ns != 16666666) { printf("  [FAIL] pts mismatch\n"); pass = false; }
    if (recv_fd < 0)                 { printf("  [FAIL] recv_fd < 0\n"); pass = false; }

    /* Verify the fd data */
    if (recv_fd >= 0) {
        if (!verify_memfd_pattern(recv_fd, 42, 4096)) {
            printf("  [FAIL] memfd pattern mismatch\n");
            pass = false;
        }
        close(recv_fd);
    }

    close(sv[0]);
    close(sv[1]);

    if (pass) printf("  [PASS] single frame\n");
    return pass ? 0 : 1;
}

/* ── Test 2: Frame slot update ───────────────────────────────── */

static int test_frame_slot_update(void)
{
    printf("  [TEST] frame slot update via IPC receive...\n");

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv) < 0) {
        printf("  [FAIL] socketpair: %s\n", strerror(errno));
        return 1;
    }
    fcntl(sv[0], F_SETFL, fcntl(sv[0], F_GETFL) | O_NONBLOCK);
    fcntl(sv[1], F_SETFL, fcntl(sv[1], F_GETFL) | O_NONBLOCK);

    /* Init frame slot */
    sbs_frame_slot_t slot;
    sbs_frame_slot_init(&slot);

    /* Send a frame */
    sbs_video_frame_msg_t msg;
    sbs_video_frame_msg_init(&msg, SBS_IPC_MSG_VIDEO_FRAME);
    msg.width    = 640;
    msg.height   = 480;
    msg.sequence = 7;
    msg.pts_ns   = 100000000;

    int test_fd = make_test_memfd(7, 1024);
    if (test_fd < 0) {
        printf("  [FAIL] memfd_create\n");
        close(sv[0]); close(sv[1]);
        return 1;
    }

    int rc = sbs_ipc_send_frame(sv[0], &msg, test_fd);
    close(test_fd);
    if (rc != SBS_OK) {
        printf("  [FAIL] send_frame returned %d\n", rc);
        close(sv[0]); close(sv[1]);
        return 1;
    }

    /* Receive and publish to frame slot (replicating supervisor logic) */
    sbs_video_frame_msg_t recv_msg;
    int dmabuf_fd = -1;
    rc = sbs_ipc_recv_frame(sv[1], &recv_msg, &dmabuf_fd);
    if (rc != SBS_OK) {
        printf("  [FAIL] recv_frame returned %d\n", rc);
        close(sv[0]); close(sv[1]);
        return 1;
    }

    /* Publish to slot — same convention as source_supervisor.c */
    uint64_t ts_us = (uint64_t)(g_get_monotonic_time() / 1000);
    sbs_frame_slot_publish(&slot, (void *)(intptr_t)dmabuf_fd, ts_us);

    /* Acquire from slot (compositor side) */
    uint64_t acquired_ts = 0;
    void *acquired = sbs_frame_slot_acquire(&slot, &acquired_ts);
    int acquired_fd = (int)(intptr_t)acquired;

    bool pass = true;
    if (acquired_fd != dmabuf_fd) {
        printf("  [FAIL] slot fd mismatch: expected %d, got %d\n",
               dmabuf_fd, acquired_fd);
        pass = false;
    }
    if (acquired_ts != ts_us) {
        printf("  [FAIL] slot timestamp mismatch\n");
        pass = false;
    }

    /* Verify we can still read the fd data */
    if (acquired_fd >= 0) {
        if (!verify_memfd_pattern(acquired_fd, 7, 1024)) {
            printf("  [FAIL] acquired fd pattern mismatch\n");
            pass = false;
        }
        close(acquired_fd);
    }

    close(sv[0]);
    close(sv[1]);

    if (pass) printf("  [PASS] frame slot update\n");
    return pass ? 0 : 1;
}

/* ── Test 3: Multi-frame burst with fork'd mock worker ───────── */

#define BURST_COUNT   20
#define FRAME_SIZE    (1920 * 1080 * 4)  /* ~8MB ABGR8888 */

typedef struct {
    sbs_frame_slot_t *slot;
    int               conn_fd;
    int               frames_received;
    int               last_seq;
    int               last_fd;
    GMainLoop        *loop;
} recv_ctx_t;

static gboolean on_recv_frame(gint fd, GIOCondition cond, gpointer user_data)
{
    recv_ctx_t *ctx = user_data;

    if (cond & (G_IO_HUP | G_IO_ERR)) {
        g_main_loop_quit(ctx->loop);
        return G_SOURCE_REMOVE;
    }

    sbs_video_frame_msg_t msg;
    int dmabuf_fd = -1;
    int rc = sbs_ipc_recv_frame(ctx->conn_fd, &msg, &dmabuf_fd);

    if (rc == SBS_ERR_WOULD_BLOCK) {
        return G_SOURCE_CONTINUE;
    }
    if (rc == SBS_ERR_EOF) {
        g_main_loop_quit(ctx->loop);
        return G_SOURCE_REMOVE;
    }
    if (rc != SBS_OK) {
        printf("  [WARN] recv error %d\n", rc);
        return G_SOURCE_CONTINUE;
    }

    if (msg.header.msg_type == SBS_IPC_MSG_VIDEO_FRAME) {
        /* Close previous fd if the compositor hasn't consumed it */
        if (ctx->last_fd >= 0) {
            close(ctx->last_fd);
        }

        /* Publish to frame slot */
        uint64_t ts_us = (uint64_t)(g_get_monotonic_time());
        sbs_frame_slot_publish(ctx->slot,
                                (void *)(intptr_t)dmabuf_fd, ts_us);

        ctx->last_seq = (int)msg.sequence;
        ctx->last_fd  = dmabuf_fd;
        ctx->frames_received++;
    } else {
        if (dmabuf_fd >= 0) close(dmabuf_fd);
    }

    /* Check if we got all frames */
    if (ctx->frames_received >= BURST_COUNT) {
        g_main_loop_quit(ctx->loop);
    }

    return G_SOURCE_CONTINUE;
}

static int test_multi_frame_fork(void)
{
    printf("  [TEST] %d-frame burst with fork'd mock worker...\n", BURST_COUNT);

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv) < 0) {
        printf("  [FAIL] socketpair: %s\n", strerror(errno));
        return 1;
    }

    /* Set socket buffers large enough for frame messages (128B each) */
    int bufsz = 256 * 1024;
    setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &bufsz, sizeof(bufsz));
    setsockopt(sv[1], SOL_SOCKET, SO_RCVBUF, &bufsz, sizeof(bufsz));

    pid_t child = fork();
    if (child < 0) {
        printf("  [FAIL] fork: %s\n", strerror(errno));
        close(sv[0]); close(sv[1]);
        return 1;
    }

    if (child == 0) {
        /* ── Mock worker child ─────────────────────────── */
        close(sv[1]); /* Close parent's end */

        /* Make our end non-blocking */
        fcntl(sv[0], F_SETFL, fcntl(sv[0], F_GETFL) | O_NONBLOCK);

        for (uint32_t seq = 0; seq < BURST_COUNT; seq++) {
            sbs_video_frame_msg_t msg;
            sbs_video_frame_msg_init(&msg, SBS_IPC_MSG_VIDEO_FRAME);
            msg.width        = 1920;
            msg.height       = 1080;
            msg.drm_format   = 0x34324241;
            msg.sequence     = seq;
            msg.pts_ns       = (uint64_t)seq * 16666666;
            msg.n_planes     = 1;
            msg.plane_stride[0] = 1920 * 4;

            /* Use a small memfd per frame (not full FRAME_SIZE) to keep test fast */
            int fd = make_test_memfd(seq, 4096);
            if (fd < 0) _exit(1);

            int rc;
            int attempts = 0;
            do {
                rc = sbs_ipc_send_frame(sv[0], &msg, fd);
                if (rc == SBS_ERR_WOULD_BLOCK) {
                    usleep(1000); /* 1ms backoff */
                    attempts++;
                }
            } while (rc == SBS_ERR_WOULD_BLOCK && attempts < 100);

            close(fd);
            if (rc != SBS_OK) _exit(2);

            /* Small delay to simulate real-time delivery */
            usleep(500);
        }

        close(sv[0]);
        _exit(0);
    }

    /* ── Parent: supervisor side ──────────────────────── */
    close(sv[0]); /* Close child's end */
    fcntl(sv[1], F_SETFL, fcntl(sv[1], F_GETFL) | O_NONBLOCK);

    sbs_frame_slot_t slot;
    sbs_frame_slot_init(&slot);

    GMainLoop *loop = g_main_loop_new(NULL, FALSE);

    recv_ctx_t ctx = {
        .slot            = &slot,
        .conn_fd         = sv[1],
        .frames_received = 0,
        .last_seq        = -1,
        .last_fd         = -1,
        .loop            = loop,
    };

    guint watch_id = g_unix_fd_add(sv[1],
                                    G_IO_IN | G_IO_HUP | G_IO_ERR,
                                    on_recv_frame, &ctx);

    /* Timeout safety: quit after 10 seconds */
    guint timeout_id = g_timeout_add(10000, (GSourceFunc)g_main_loop_quit, loop);

    int64_t start = now_ms();
    g_main_loop_run(loop);
    int64_t elapsed = now_ms() - start;

    g_source_remove(timeout_id);
    if (watch_id) {
        /* watch might have been removed by G_SOURCE_REMOVE */
    }

    /* Wait for child */
    int wstatus;
    waitpid(child, &wstatus, 0);

    /* Clean up last fd */
    if (ctx.last_fd >= 0) close(ctx.last_fd);
    close(sv[1]);
    g_main_loop_unref(loop);

    bool pass = true;

    if (!WIFEXITED(wstatus) || WEXITSTATUS(wstatus) != 0) {
        printf("  [FAIL] mock worker exited with status %d\n",
               WIFEXITED(wstatus) ? WEXITSTATUS(wstatus) : -1);
        pass = false;
    }

    if (ctx.frames_received != BURST_COUNT) {
        printf("  [FAIL] expected %d frames, got %d\n",
               BURST_COUNT, ctx.frames_received);
        pass = false;
    }

    if (ctx.last_seq != BURST_COUNT - 1) {
        printf("  [FAIL] last sequence %d, expected %d\n",
               ctx.last_seq, BURST_COUNT - 1);
        pass = false;
    }

    /* Verify frame slot has the latest frame */
    uint64_t slot_ts = 0;
    void *slot_frame = sbs_frame_slot_acquire(&slot, &slot_ts);
    if ((intptr_t)slot_frame == 0) {
        printf("  [FAIL] frame slot empty after burst\n");
        pass = false;
    }

    if (pass) {
        printf("  [PASS] %d frames received in %lldms (last_seq=%d)\n",
               ctx.frames_received, (long long)elapsed, ctx.last_seq);
    }

    return pass ? 0 : 1;
}

/* ── Test 4: Control messages (STATUS, SHUTDOWN) ─────────────── */

static int test_control_messages(void)
{
    printf("  [TEST] control messages (STATUS + SHUTDOWN)...\n");

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv) < 0) {
        printf("  [FAIL] socketpair: %s\n", strerror(errno));
        return 1;
    }
    fcntl(sv[0], F_SETFL, fcntl(sv[0], F_GETFL) | O_NONBLOCK);
    fcntl(sv[1], F_SETFL, fcntl(sv[1], F_GETFL) | O_NONBLOCK);

    bool pass = true;

    /* Send a STATUS message */
    sbs_status_msg_t status;
    sbs_status_msg_init(&status);
    status.state            = SBS_WORKER_STATE_RUNNING;
    status.frames_produced  = 1000;
    status.frames_dropped   = 5;
    status.frame_width      = 1920;
    status.frame_height     = 1080;

    int rc = sbs_ipc_send_msg(sv[0], &status, sizeof(status));
    if (rc != SBS_OK) {
        printf("  [FAIL] send STATUS: %d\n", rc);
        pass = false;
    }

    /* Receive it */
    if (pass) {
        sbs_status_msg_t recv_status;
        size_t out_read = 0;
        rc = sbs_ipc_recv_msg(sv[1], &recv_status, sizeof(recv_status), &out_read);
        if (rc != SBS_OK) {
            printf("  [FAIL] recv STATUS: %d\n", rc);
            pass = false;
        } else {
            if (recv_status.header.msg_type != SBS_IPC_MSG_STATUS) {
                printf("  [FAIL] STATUS msg_type mismatch: 0x%04x\n",
                       recv_status.header.msg_type);
                pass = false;
            }
            if (recv_status.frames_produced != 1000) {
                printf("  [FAIL] frames_produced mismatch\n");
                pass = false;
            }
            if (recv_status.state != SBS_WORKER_STATE_RUNNING) {
                printf("  [FAIL] state mismatch\n");
                pass = false;
            }
        }
    }

    /* Send a SHUTDOWN message */
    sbs_shutdown_msg_t shutdown;
    sbs_shutdown_msg_init(&shutdown, 2000, 0 /* normal */);


    rc = sbs_ipc_send_msg(sv[0], &shutdown, sizeof(shutdown));
    if (rc != SBS_OK) {
        printf("  [FAIL] send SHUTDOWN: %d\n", rc);
        pass = false;
    }

    if (pass) {
        sbs_shutdown_msg_t recv_shutdown;
        size_t out_read = 0;
        rc = sbs_ipc_recv_msg(sv[1], &recv_shutdown, sizeof(recv_shutdown), &out_read);
        if (rc != SBS_OK) {
            printf("  [FAIL] recv SHUTDOWN: %d\n", rc);
            pass = false;
        } else {
            if (recv_shutdown.header.msg_type != SBS_IPC_MSG_SHUTDOWN) {
                printf("  [FAIL] SHUTDOWN msg_type mismatch\n");
                pass = false;
            }
            if (recv_shutdown.grace_period_ms != 2000) {
                printf("  [FAIL] grace_period mismatch\n");
                pass = false;
            }
        }
    }

    close(sv[0]);
    close(sv[1]);

    if (pass) printf("  [PASS] control messages\n");
    return pass ? 0 : 1;
}

/* ── Test 5: Fd leak detection ───────────────────────────────── */

static int test_fd_leak(void)
{
    printf("  [TEST] fd leak detection over 100 frame cycles...\n");

    int fds_before = count_open_fds();

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv) < 0) {
        printf("  [FAIL] socketpair: %s\n", strerror(errno));
        return 1;
    }
    fcntl(sv[0], F_SETFL, fcntl(sv[0], F_GETFL) | O_NONBLOCK);
    fcntl(sv[1], F_SETFL, fcntl(sv[1], F_GETFL) | O_NONBLOCK);

    sbs_frame_slot_t slot;
    sbs_frame_slot_init(&slot);
    int prev_fd = -1;

    for (int i = 0; i < 100; i++) {
        sbs_video_frame_msg_t msg;
        sbs_video_frame_msg_init(&msg, SBS_IPC_MSG_VIDEO_FRAME);
        msg.sequence = (uint32_t)i;
        msg.width    = 320;
        msg.height   = 240;

        int test_fd = make_test_memfd((uint32_t)i, 256);
        if (test_fd < 0) {
            printf("  [FAIL] memfd_create at iter %d\n", i);
            close(sv[0]); close(sv[1]);
            if (prev_fd >= 0) close(prev_fd);
            return 1;
        }

        int rc = sbs_ipc_send_frame(sv[0], &msg, test_fd);
        close(test_fd); /* sender side */
        if (rc != SBS_OK) {
            printf("  [FAIL] send at iter %d: %d\n", i, rc);
            close(sv[0]); close(sv[1]);
            if (prev_fd >= 0) close(prev_fd);
            return 1;
        }

        int recv_fd = -1;
        sbs_video_frame_msg_t recv_msg;
        rc = sbs_ipc_recv_frame(sv[1], &recv_msg, &recv_fd);
        if (rc != SBS_OK) {
            printf("  [FAIL] recv at iter %d: %d\n", i, rc);
            close(sv[0]); close(sv[1]);
            if (prev_fd >= 0) close(prev_fd);
            return 1;
        }

        /* Publish to slot */
        sbs_frame_slot_publish(&slot, (void *)(intptr_t)recv_fd, (uint64_t)i);

        /* Close the PREVIOUS fd (simulating compositor closing old fd after import) */
        if (prev_fd >= 0) {
            close(prev_fd);
        }
        prev_fd = recv_fd;
    }

    /* Close the last fd */
    if (prev_fd >= 0) close(prev_fd);

    close(sv[0]);
    close(sv[1]);

    int fds_after = count_open_fds();
    int leaked = fds_after - fds_before;

    if (leaked != 0) {
        printf("  [FAIL] fd leak: %d fds leaked (before=%d, after=%d)\n",
               leaked, fds_before, fds_after);
        return 1;
    }

    printf("  [PASS] 0 fds leaked over 100 cycles\n");
    return 0;
}

/* ── Main ─────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    sbs_log_init("integ-ipc", SBS_LOG_DEBUG);

    printf("═══ IPC Integration Tests ═══\n\n");

    int failures = 0;
    int64_t start = now_ms();

    failures += test_single_frame();
    failures += test_frame_slot_update();
    failures += test_multi_frame_fork();
    failures += test_control_messages();
    failures += test_fd_leak();

    int64_t elapsed = now_ms() - start;

    printf("\n═══ Results: %d/5 passed in %lldms ═══\n",
           5 - failures, (long long)elapsed);

    if (failures == 0) {
        printf("\n  ALL TESTS PASSED\n\n");
    } else {
        printf("\n  %d TEST(S) FAILED\n\n", failures);
    }

    return failures > 0 ? 1 : 0;
}
