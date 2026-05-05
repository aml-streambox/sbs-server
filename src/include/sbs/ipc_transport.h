/*
 * SBS - StreamBox Broadcast System
 * IPC transport — sendmsg/recvmsg with SCM_RIGHTS for DMA-BUF fd passing
 */
#ifndef SBS_IPC_TRANSPORT_H
#define SBS_IPC_TRANSPORT_H

#include "sbs/ipc.h"
#include "sbs/types.h"

/**
 * Send a video frame message with a DMA-BUF fd over a Unix domain socket.
 *
 * Uses sendmsg() with MSG_DONTWAIT | MSG_NOSIGNAL and SCM_RIGHTS ancillary
 * data to pass the file descriptor without blocking.
 *
 * @param sock_fd   Connected Unix domain socket
 * @param msg       128-byte video frame message (header + payload)
 * @param dmabuf_fd DMA-BUF file descriptor to send via SCM_RIGHTS (-1 to skip)
 *
 * @return 0 on success,
 *         SBS_ERR_WOULD_BLOCK if socket buffer is full,
 *         SBS_ERR_PIPE if peer disconnected,
 *         SBS_ERR_IO on other errors
 */
int sbs_ipc_send_frame(int sock_fd, const sbs_video_frame_msg_t *msg, int dmabuf_fd);

/**
 * Send a video frame message with one or two DMA-BUF fds over a Unix domain socket.
 *
 * @param sock_fd    Connected Unix domain socket
 * @param msg        128-byte video frame message (header + payload)
 * @param dmabuf_fd  Primary DMA-BUF file descriptor (-1 to skip)
 * @param dmabuf_fd2 Secondary DMA-BUF file descriptor for UV plane (-1 to skip)
 */
int sbs_ipc_send_frame2(int sock_fd, const sbs_video_frame_msg_t *msg,
                        int dmabuf_fd, int dmabuf_fd2);

/**
 * Receive a video frame message with a DMA-BUF fd from a Unix domain socket.
 *
 * Uses recvmsg() with MSG_DONTWAIT to non-blocking receive the message and
 * extract the DMA-BUF fd from SCM_RIGHTS ancillary data.
 *
 * @param sock_fd      Connected Unix domain socket
 * @param msg          Output: 128-byte video frame message
 * @param dmabuf_fd    Output: received DMA-BUF fd (-1 if none attached)
 *
 * @return 0 on success,
 *         SBS_ERR_WOULD_BLOCK if no data available,
 *         SBS_ERR_EOF if peer closed connection,
 *         SBS_ERR_IO on other errors
 */
int sbs_ipc_recv_frame(int sock_fd, sbs_video_frame_msg_t *msg, int *dmabuf_fd);

/**
 * Receive a video frame message with one or two DMA-BUF fds from a Unix domain socket.
 *
 * @param sock_fd      Connected Unix domain socket
 * @param msg          Output: 128-byte video frame message
 * @param dmabuf_fd    Output: received primary DMA-BUF fd (-1 if none)
 * @param dmabuf_fd2   Output: received secondary DMA-BUF fd (-1 if none, may be NULL)
 */
int sbs_ipc_recv_frame2(int sock_fd, sbs_video_frame_msg_t *msg,
                        int *dmabuf_fd, int *dmabuf_fd2);

/**
 * Send a generic control message (no fd) over a Unix domain socket.
 *
 * @param sock_fd      Connected Unix domain socket
 * @param data         Message data (header + payload)
 * @param data_len     Total message size in bytes
 *
 * @return 0 on success, negative error code on failure
 */
int sbs_ipc_send_msg(int sock_fd, const void *data, size_t data_len);

/**
 * Receive a generic message from a Unix domain socket.
 *
 * Reads up to max_len bytes. The caller must inspect the header to determine
 * the message type and whether ancillary data (fds) may be present.
 *
 * @param sock_fd      Connected Unix domain socket
 * @param data         Output buffer for message data
 * @param max_len      Maximum bytes to read
 * @param out_read     Output: actual bytes read (may be NULL)
 *
 * @return 0 on success, negative error code on failure
 */
int sbs_ipc_recv_msg(int sock_fd, void *data, size_t max_len, size_t *out_read);

#endif /* SBS_IPC_TRANSPORT_H */
