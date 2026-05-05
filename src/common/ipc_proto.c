/*
 * SBS - StreamBox Broadcast System
 * IPC protocol helpers (Phase 2)
 */
#include "sbs/ipc.h"
#include "sbs/types.h"

#include <string.h>
#include <stdbool.h>

bool sbs_ipc_header_valid(const sbs_ipc_msg_header_t *hdr)
{
    if (!hdr)
        return false;

    uint32_t t = hdr->msg_type;

    /* Data messages: 0x0001 - 0x0003 */
    if (t >= SBS_IPC_MSG_VIDEO_FRAME && t <= SBS_IPC_MSG_AUDIO_BUFFER)
        return true;

    /* Control messages: 0x0010 - 0x0014 */
    if (t >= SBS_IPC_MSG_STATUS && t <= SBS_IPC_MSG_SIGNAL_CHANGE)
        return true;
    if (t == SBS_IPC_MSG_FRAME_RELEASE)
        return true;

    /* Response messages */
    if (t == SBS_IPC_MSG_ACK || t == SBS_IPC_MSG_ERROR)
        return true;

    return false;
}

void sbs_ipc_header_init(sbs_ipc_msg_header_t *hdr, sbs_ipc_msg_type_t type,
                          uint32_t payload_size)
{
    memset(hdr, 0, sizeof(*hdr));
    hdr->msg_type     = (uint32_t)type;
    hdr->payload_size = payload_size;
}
