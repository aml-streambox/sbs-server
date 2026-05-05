/*
 * SBS - StreamBox Broadcast System
 * IPC transport — sendmsg/recvmsg with SCM_RIGHTS for DMA-BUF fd passing
 *
 * Follows document/04-ipc-protocol.md section 4 exactly.
 */
#define _GNU_SOURCE

#include "sbs/ipc_transport.h"
#include "sbs/log.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

#define SBS_LOG_COMP "ipc-transport"

/* ── Send frame with SCM_RIGHTS (1-2 fds) ─────────────────────── */

int sbs_ipc_send_frame2(int sock_fd, const sbs_video_frame_msg_t *msg,
                        int dmabuf_fd, int dmabuf_fd2)
{
    if (sock_fd < 0 || !msg)
        return SBS_ERR_INVAL;

    struct iovec iov = {
        .iov_base = (void *)msg,
        .iov_len  = sizeof(*msg),
    };

    struct msghdr msghdr = {
        .msg_iov     = &iov,
        .msg_iovlen  = 1,
    };

    int nfds = 0;
    int fds[2];
    if (dmabuf_fd >= 0) {
        fds[nfds++] = dmabuf_fd;
        if (dmabuf_fd2 >= 0)
            fds[nfds++] = dmabuf_fd2;
    }

    union {
        char             buf[CMSG_SPACE(sizeof(int) * 2)];
        struct cmsghdr   align;
    } cmsg_buf;

    if (nfds > 0) {
        memset(&cmsg_buf, 0, sizeof(cmsg_buf));
        msghdr.msg_control    = cmsg_buf.buf;
        msghdr.msg_controllen = CMSG_SPACE(sizeof(int) * nfds);

        struct cmsghdr *cmsg  = CMSG_FIRSTHDR(&msghdr);
        cmsg->cmsg_level      = SOL_SOCKET;
        cmsg->cmsg_type       = SCM_RIGHTS;
        cmsg->cmsg_len        = CMSG_LEN(sizeof(int) * nfds);
        memcpy(CMSG_DATA(cmsg), fds, sizeof(int) * nfds);
    }

    ssize_t ret = sendmsg(sock_fd, &msghdr, MSG_DONTWAIT | MSG_NOSIGNAL);
    if (ret < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return SBS_ERR_WOULD_BLOCK;
        if (errno == EPIPE || errno == ECONNRESET)
            return SBS_ERR_PIPE;
        LOG_W("sendmsg failed: %s", strerror(errno));
        return SBS_ERR_IO;
    }

    if ((size_t)ret != sizeof(*msg)) {
        LOG_W("sendmsg short write: %zd/%zu", ret, sizeof(*msg));
        return SBS_ERR_IO;
    }

    return SBS_OK;
}

int sbs_ipc_send_frame(int sock_fd, const sbs_video_frame_msg_t *msg, int dmabuf_fd)
{
    return sbs_ipc_send_frame2(sock_fd, msg, dmabuf_fd, -1);
}

/* ── Receive frame with SCM_RIGHTS (1-2 fds) ──────────────────── */

int sbs_ipc_recv_frame2(int sock_fd, sbs_video_frame_msg_t *msg,
                        int *dmabuf_fd, int *dmabuf_fd2)
{
    if (sock_fd < 0 || !msg || !dmabuf_fd)
        return SBS_ERR_INVAL;

    *dmabuf_fd = -1;
    if (dmabuf_fd2)
        *dmabuf_fd2 = -1;

    struct iovec iov = {
        .iov_base = msg,
        .iov_len  = sizeof(*msg),
    };

    union {
        char             buf[CMSG_SPACE(sizeof(int) * 2)];
        struct cmsghdr   align;
    } cmsg_buf;
    memset(&cmsg_buf, 0, sizeof(cmsg_buf));

    struct msghdr msghdr = {
        .msg_iov        = &iov,
        .msg_iovlen     = 1,
        .msg_control    = cmsg_buf.buf,
        .msg_controllen = sizeof(cmsg_buf.buf),
    };

    ssize_t ret = recvmsg(sock_fd, &msghdr, MSG_DONTWAIT);
    if (ret < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return SBS_ERR_WOULD_BLOCK;
        if (errno == ECONNRESET)
            return SBS_ERR_PIPE;
        LOG_W("recvmsg failed: %s", strerror(errno));
        return SBS_ERR_IO;
    }

    if (ret == 0)
        return SBS_ERR_EOF;

    if ((size_t)ret != sizeof(*msg)) {
        LOG_W("recvmsg short read: %zd/%zu", ret, sizeof(*msg));
        /* Close any received fds to avoid leak on partial read */
        for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msghdr);
             cmsg != NULL;
             cmsg = CMSG_NXTHDR(&msghdr, cmsg)) {
            if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
                size_t n = (cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int);
                for (size_t i = 0; i < n; i++) {
                    int fd;
                    memcpy(&fd, CMSG_DATA(cmsg) + i * sizeof(int), sizeof(int));
                    if (fd >= 0)
                        close(fd);
                }
            }
        }
        return SBS_ERR_IO;
    }

    /* Extract DMA-BUF fd(s) from ancillary data */
    for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msghdr);
         cmsg != NULL;
         cmsg = CMSG_NXTHDR(&msghdr, cmsg)) {
        if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
            size_t n = (cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int);
            if (n >= 1)
                memcpy(dmabuf_fd, CMSG_DATA(cmsg), sizeof(int));
            if (n >= 2 && dmabuf_fd2)
                memcpy(dmabuf_fd2, CMSG_DATA(cmsg) + sizeof(int), sizeof(int));
            break;
        }
    }

    return SBS_OK;
}

int sbs_ipc_recv_frame(int sock_fd, sbs_video_frame_msg_t *msg, int *dmabuf_fd)
{
    return sbs_ipc_recv_frame2(sock_fd, msg, dmabuf_fd, NULL);
}

/* ── Send generic control message (no fd) ─────────────────────── */

int sbs_ipc_send_msg(int sock_fd, const void *data, size_t data_len)
{
    if (sock_fd < 0 || !data || data_len == 0)
        return SBS_ERR_INVAL;

    ssize_t ret = send(sock_fd, data, data_len, MSG_DONTWAIT | MSG_NOSIGNAL);
    if (ret < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return SBS_ERR_WOULD_BLOCK;
        if (errno == EPIPE || errno == ECONNRESET)
            return SBS_ERR_PIPE;
        LOG_W("send failed: %s", strerror(errno));
        return SBS_ERR_IO;
    }

    if ((size_t)ret != data_len) {
        LOG_W("send short write: %zd/%zu", ret, data_len);
        return SBS_ERR_IO;
    }

    return SBS_OK;
}

/* ── Receive generic message (no fd) ──────────────────────────── */

int sbs_ipc_recv_msg(int sock_fd, void *data, size_t max_len, size_t *out_read)
{
    if (sock_fd < 0 || !data || max_len == 0)
        return SBS_ERR_INVAL;

    ssize_t ret = recv(sock_fd, data, max_len, MSG_DONTWAIT);
    if (ret < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return SBS_ERR_WOULD_BLOCK;
        if (errno == ECONNRESET)
            return SBS_ERR_PIPE;
        LOG_W("recv failed: %s", strerror(errno));
        return SBS_ERR_IO;
    }

    if (ret == 0)
        return SBS_ERR_EOF;

    if (out_read)
        *out_read = (size_t)ret;

    return SBS_OK;
}
