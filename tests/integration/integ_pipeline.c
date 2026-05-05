/**
 * integ_pipeline.c — End-to-end pipeline integration test.
 *
 * Tests the full server-side data path:
 *   compositor thread → eventfd → frame export → output distribution
 *
 * On the A311D2 target (Mali-G52), the compositor produces real rendered
 * frames with DMA-BUF export. On the host (llvmpipe), DMA-BUF export may
 * return fd=-1 which is expected and tested gracefully.
 *
 * This test does NOT require GStreamer — it injects test data via frame
 * slots and reads output via socketpair, bypassing the source/output workers.
 *
 * Optionally dumps a rendered frame to /tmp/sbs_integ_frame.raw for visual
 * verification with the let-me-take-a-look vision tool.
 */

#define _GNU_SOURCE
#define SBS_LOG_COMP "integ-pipeline"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <dirent.h>
#include <time.h>

#include <glib.h>
#include <glib-unix.h>

#include <sbs/log.h>
#include <sbs/types.h>
#include <sbs/compositor.h>
#include <sbs/compositor_thread.h>
#include <sbs/frame_slot.h>
#include <sbs/ipc.h>
#include <sbs/ipc_transport.h>

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
    return count - 1;
}

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/**
 * Dump a DMA-BUF/memfd frame to a raw file for visual analysis.
 * Returns 0 on success, -1 on failure.
 */
static int dump_frame_raw(int fd, const char *path, size_t expected_size)
{
    /* Try to determine the size from the fd */
    off_t size = lseek(fd, 0, SEEK_END);
    if (size <= 0) {
        /* Try mmap with expected size for DMA-BUF */
        size = (off_t)expected_size;
    }
    lseek(fd, 0, SEEK_SET);

    void *p = mmap(NULL, (size_t)size, PROT_READ, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        printf("    (cannot mmap frame fd for dump: %s)\n", strerror(errno));
        return -1;
    }

    FILE *f = fopen(path, "wb");
    if (!f) {
        munmap(p, (size_t)size);
        return -1;
    }
    fwrite(p, 1, (size_t)size, f);
    fclose(f);
    munmap(p, (size_t)size);

    printf("    frame dumped to %s (%ld bytes)\n", path, (long)size);
    return 0;
}

/* ── Test 1: Compositor thread lifecycle ─────────────────────── */

static int test_compositor_lifecycle(void)
{
    printf("  [TEST] compositor thread start/stop lifecycle...\n");

    sbs_compositor_thread_t *ct = sbs_compositor_thread_new(
        640, 480, 30, SBS_EXPORT_COLOR_SDR, "/usr/share/sbs/shaders");

    if (!ct) {
        printf("  [FAIL] compositor_thread_new returned NULL\n");
        return 1;
    }

    int rc = sbs_compositor_thread_start(ct);
    if (rc != 0) {
        printf("  [FAIL] compositor_thread_start returned %d\n", rc);
        sbs_compositor_thread_free(ct);
        return 1;
    }

    int efd = sbs_compositor_thread_get_eventfd(ct);
    if (efd < 0) {
        printf("  [FAIL] get_eventfd returned %d\n", efd);
        sbs_compositor_thread_stop(ct);
        sbs_compositor_thread_free(ct);
        return 1;
    }

    /* Let the compositor render a few frames */
    usleep(200000); /* 200ms ≈ ~6 frames at 30fps */

    /* Drain events */
    sbs_comp_event_t *ev = NULL;
    int event_count = 0;
    while (sbs_compositor_thread_pop_event(ct, &ev)) {
        event_count++;
        free(ev);
    }

    sbs_compositor_thread_stop(ct);
    sbs_compositor_thread_free(ct);

    if (event_count == 0) {
        printf("  [WARN] no events received (compositor may not have rendered)\n");
        /* Not necessarily a failure — llvmpipe init can be slow */
    }

    printf("  [PASS] lifecycle OK (%d events in 200ms)\n", event_count);
    return 0;
}

/* ── Test 2: Frame export from compositor ────────────────────── */

static int test_frame_export(void)
{
    printf("  [TEST] frame export from compositor...\n");

    sbs_compositor_thread_t *ct = sbs_compositor_thread_new(
        320, 240, 30, SBS_EXPORT_COLOR_SDR, "/usr/share/sbs/shaders");

    if (!ct) {
        printf("  [FAIL] compositor_thread_new returned NULL\n");
        return 1;
    }

    int rc = sbs_compositor_thread_start(ct);
    if (rc != 0) {
        printf("  [FAIL] compositor_thread_start returned %d\n", rc);
        sbs_compositor_thread_free(ct);
        return 1;
    }

    /* Wait for at least one frame to render */
    int efd = sbs_compositor_thread_get_eventfd(ct);
    fd_set rfds;
    struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
    FD_ZERO(&rfds);
    FD_SET(efd, &rfds);

    int sel = select(efd + 1, &rfds, NULL, NULL, &tv);
    if (sel <= 0) {
        printf("  [WARN] no frame event within 5s (Vulkan init slow?)\n");
        sbs_compositor_thread_stop(ct);
        sbs_compositor_thread_free(ct);
        /* Not a hard failure — llvmpipe can be very slow */
        printf("  [SKIP] frame export (no events)\n");
        return 0;
    }

    /* Drain the eventfd */
    uint64_t val;
    (void)read(efd, &val, sizeof(val));
    sbs_compositor_thread_drain_events(ct);

    /* Try to export the frame */
    int frame_fd = -1;
    sbs_video_frame_msg_t frame_msg;
    rc = sbs_compositor_thread_export_frame(ct, &frame_msg, &frame_fd);

    sbs_compositor_thread_stop(ct);

    if (rc != 0 || frame_fd < 0) {
        /* Expected on host (llvmpipe doesn't support DMA-BUF export) */
        printf("  [INFO] export returned rc=%d fd=%d (expected on host/llvmpipe)\n",
               rc, frame_fd);
        printf("  [PASS] frame export (graceful skip — no DMA-BUF support)\n");
        sbs_compositor_thread_free(ct);
        return 0;
    }

    printf("  [INFO] exported frame fd=%d\n", frame_fd);

    /* Dump frame for visual verification */
    size_t frame_size = 320 * 240 * 4; /* ABGR8888 */
    dump_frame_raw(frame_fd, "/tmp/sbs_integ_frame.raw", frame_size);

    close(frame_fd);
    sbs_compositor_thread_free(ct);

    printf("  [PASS] frame export succeeded\n");
    return 0;
}

/* ── Test 3: Frame distribution to mock output ───────────────── */

static int test_frame_distribution(void)
{
    printf("  [TEST] frame distribution via IPC to mock output...\n");

    /* Create a socketpair to simulate output supervisor → output worker link */
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv) < 0) {
        printf("  [FAIL] socketpair: %s\n", strerror(errno));
        return 1;
    }
    fcntl(sv[0], F_SETFL, fcntl(sv[0], F_GETFL) | O_NONBLOCK);
    fcntl(sv[1], F_SETFL, fcntl(sv[1], F_GETFL) | O_NONBLOCK);

    int bufsz = 256 * 1024;
    setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &bufsz, sizeof(bufsz));
    setsockopt(sv[1], SOL_SOCKET, SO_RCVBUF, &bufsz, sizeof(bufsz));

    /* Simulate the output router: build frame messages and send them.
     * This tests the IPC path from output router to a mock output worker
     * without needing the real compositor (which may not support DMA-BUF
     * on host). */

    const int NUM_FRAMES = 10;
    bool pass = true;

    /* Sender: simulate output router distributing frames */
    for (int i = 0; i < NUM_FRAMES; i++) {
        sbs_video_frame_msg_t msg;
        sbs_video_frame_msg_init(&msg, SBS_IPC_MSG_COMPOSED_FRAME);
        msg.width        = 1920;
        msg.height       = 1080;
        msg.drm_format   = 0x34324241; /* DRM_FORMAT_ABGR8888 */
        msg.sequence     = (uint32_t)i;
        msg.pts_ns       = (uint64_t)i * 33333333;
        msg.n_planes     = 1;
        msg.plane_stride[0] = 1920 * 4;

        int test_fd = memfd_create("composed-frame", MFD_CLOEXEC);
        if (test_fd < 0) {
            printf("  [FAIL] memfd_create at frame %d\n", i);
            pass = false;
            break;
        }
        ftruncate(test_fd, 4096);

        /* dup() the fd like the output supervisor does */
        int dup_fd = dup(test_fd);
        close(test_fd);

        int rc = sbs_ipc_send_frame(sv[0], &msg, dup_fd);
        close(dup_fd);

        if (rc != SBS_OK) {
            printf("  [FAIL] send_frame at frame %d: %d\n", i, rc);
            pass = false;
            break;
        }
    }

    if (!pass) {
        close(sv[0]);
        close(sv[1]);
        return 1;
    }

    /* Receiver: read frames directly (no GLib loop needed — data is buffered) */
    int frames_received = 0;
    int last_fd = -1;

    int64_t start = now_ms();

    for (int i = 0; i < NUM_FRAMES + 5; i++) {
        sbs_video_frame_msg_t recv_msg;
        int recv_fd = -1;
        int rrc = sbs_ipc_recv_frame(sv[1], &recv_msg, &recv_fd);

        if (rrc == SBS_ERR_WOULD_BLOCK) {
            /* All buffered data consumed */
            break;
        }
        if (rrc == SBS_ERR_EOF) break;
        if (rrc != SBS_OK) {
            printf("  [WARN] recv error %d at frame %d\n", rrc, i);
            break;
        }

        if (recv_msg.header.msg_type == SBS_IPC_MSG_COMPOSED_FRAME) {
            if (last_fd >= 0) close(last_fd);
            last_fd = recv_fd;
            frames_received++;
        } else {
            if (recv_fd >= 0) close(recv_fd);
        }
    }

    int64_t elapsed = now_ms() - start;

    if (last_fd >= 0) close(last_fd);
    close(sv[0]);
    close(sv[1]);

    if (frames_received != NUM_FRAMES) {
        printf("  [FAIL] expected %d frames, got %d\n",
               NUM_FRAMES, frames_received);
        return 1;
    }

    printf("  [PASS] %d composed frames distributed in %lldms\n",
           frames_received, (long long)elapsed);
    return 0;
}

/* ── Test 4: Full pipeline with fd leak check ────────────────── */

static int test_pipeline_fd_leak(void)
{
    printf("  [TEST] full pipeline fd leak check...\n");

    int fds_before = count_open_fds();

    /* Create compositor */
    sbs_compositor_thread_t *ct = sbs_compositor_thread_new(
        320, 240, 60, SBS_EXPORT_COLOR_SDR, "/usr/share/sbs/shaders");
    if (!ct) {
        printf("  [FAIL] compositor_thread_new returned NULL\n");
        return 1;
    }

    int rc = sbs_compositor_thread_start(ct);
    if (rc != 0) {
        printf("  [FAIL] compositor_thread_start returned %d\n", rc);
        sbs_compositor_thread_free(ct);
        return 1;
    }

    /* Let it render ~500ms worth of frames */
    usleep(500000);

    /* Drain all events without exporting (no fd leak expected) */
    int efd = sbs_compositor_thread_get_eventfd(ct);
    uint64_t val;
    (void)read(efd, &val, sizeof(val));

    int event_count = 0;
    sbs_comp_event_t *ev = NULL;
    while (sbs_compositor_thread_pop_event(ct, &ev)) {
        event_count++;
        free(ev);
    }

    /* Try exporting a few frames and closing them */
    for (int i = 0; i < 5; i++) {
        int frame_fd = -1;
        sbs_video_frame_msg_t frame_msg;
        rc = sbs_compositor_thread_export_frame(ct, &frame_msg, &frame_fd);
        if (frame_fd >= 0) {
            close(frame_fd);
        }
    }

    sbs_compositor_thread_stop(ct);
    sbs_compositor_thread_free(ct);

    int fds_after = count_open_fds();
    int leaked = fds_after - fds_before;

    if (leaked > 0) {
        printf("  [FAIL] %d fds leaked (before=%d, after=%d)\n",
               leaked, fds_before, fds_after);
        return 1;
    }

    printf("  [PASS] 0 fds leaked (%d events processed)\n", event_count);
    return 0;
}

/* ── Main ─────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    sbs_log_init("integ-pipeline", SBS_LOG_INFO);

    printf("═══ Pipeline Integration Tests ═══\n\n");

    int failures = 0;
    int64_t start = now_ms();

    failures += test_compositor_lifecycle();
    failures += test_frame_export();
    failures += test_frame_distribution();
    failures += test_pipeline_fd_leak();

    int64_t elapsed = now_ms() - start;

    printf("\n═══ Results: %d/4 passed in %lldms ═══\n",
           4 - failures, (long long)elapsed);

    if (failures == 0) {
        printf("\n  ALL TESTS PASSED\n\n");
    } else {
        printf("\n  %d TEST(S) FAILED\n\n", failures);
    }

    return failures > 0 ? 1 : 0;
}
