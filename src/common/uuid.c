/*
 * SBS - StreamBox Broadcast System
 * UUID v4 generator
 */
#include "sbs/uuid.h"
#include "sbs/types.h"

#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <ctype.h>

void sbs_uuid_generate(sbs_uuid_t out)
{
    unsigned char buf[16];

    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) {
        /* Fallback: zero UUID (should never happen on Linux) */
        snprintf(out, SBS_UUID_STRING_LEN,
                 "00000000-0000-4000-8000-000000000000");
        return;
    }

    ssize_t n = read(fd, buf, sizeof(buf));
    close(fd);

    if (n != (ssize_t)sizeof(buf)) {
        snprintf(out, SBS_UUID_STRING_LEN,
                 "00000000-0000-4000-8000-000000000000");
        return;
    }

    /* Set version 4 (random) */
    buf[6] = (buf[6] & 0x0F) | 0x40;
    /* Set variant 1 (RFC 4122) */
    buf[8] = (buf[8] & 0x3F) | 0x80;

    snprintf(out, SBS_UUID_STRING_LEN,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             buf[0],  buf[1],  buf[2],  buf[3],
             buf[4],  buf[5],
             buf[6],  buf[7],
             buf[8],  buf[9],
             buf[10], buf[11], buf[12], buf[13], buf[14], buf[15]);
}

bool sbs_uuid_validate(const char *uuid)
{
    if (!uuid)
        return false;

    size_t len = strlen(uuid);
    if (len != 36)
        return false;

    /* Format: xxxxxxxx-xxxx-4xxx-[89ab]xxx-xxxxxxxxxxxx */
    for (size_t i = 0; i < 36; i++) {
        char c = uuid[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != '-') return false;
        } else if (i == 14) {
            /* Version nibble must be '4' */
            if (c != '4') return false;
        } else if (i == 19) {
            /* Variant nibble must be 8, 9, a, or b */
            char lc = (char)tolower((unsigned char)c);
            if (lc != '8' && lc != '9' && lc != 'a' && lc != 'b')
                return false;
        } else {
            if (!isxdigit((unsigned char)c))
                return false;
        }
    }

    return true;
}
