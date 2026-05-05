/*
 * SBS - StreamBox Broadcast System
 * Unit tests for UUID generation
 */
#include "sbs_test.h"
#include "sbs/uuid.h"

/* ── UUID generation ──────────────────────────────────────────── */

SBS_TEST(uuid, generate_produces_valid_format) {
    sbs_uuid_t uuid;
    sbs_uuid_generate(uuid);
    SBS_ASSERT_EQ(strlen(uuid), 36);
    SBS_ASSERT(sbs_uuid_validate(uuid));
}

SBS_TEST(uuid, generate_version_4_marker) {
    sbs_uuid_t uuid;
    sbs_uuid_generate(uuid);
    /* Position 14 must be '4' (version nibble) */
    SBS_ASSERT_EQ(uuid[14], '4');
}

SBS_TEST(uuid, generate_variant_1_marker) {
    sbs_uuid_t uuid;
    sbs_uuid_generate(uuid);
    /* Position 19 must be 8, 9, a, or b (variant nibble) */
    char c = uuid[19];
    SBS_ASSERT(c == '8' || c == '9' || c == 'a' || c == 'b');
}

SBS_TEST(uuid, generate_unique) {
    /* Generate 100 UUIDs and verify no duplicates */
    sbs_uuid_t uuids[100];
    for (int i = 0; i < 100; i++) {
        sbs_uuid_generate(uuids[i]);
    }
    for (int i = 0; i < 100; i++) {
        for (int j = i + 1; j < 100; j++) {
            SBS_ASSERT_STR_NE(uuids[i], uuids[j]);
        }
    }
}

SBS_TEST(uuid, generate_hyphen_positions) {
    sbs_uuid_t uuid;
    sbs_uuid_generate(uuid);
    SBS_ASSERT_EQ(uuid[8],  '-');
    SBS_ASSERT_EQ(uuid[13], '-');
    SBS_ASSERT_EQ(uuid[18], '-');
    SBS_ASSERT_EQ(uuid[23], '-');
}

/* ── UUID validation ──────────────────────────────────────────── */

SBS_TEST(uuid, validate_null_returns_false) {
    SBS_ASSERT_EQ(sbs_uuid_validate(NULL), false);
}

SBS_TEST(uuid, validate_empty_returns_false) {
    SBS_ASSERT_EQ(sbs_uuid_validate(""), false);
}

SBS_TEST(uuid, validate_wrong_length) {
    SBS_ASSERT_EQ(sbs_uuid_validate("abc"), false);
}

SBS_TEST(uuid, validate_bad_version) {
    /* Version nibble at pos 14 must be '4' */
    SBS_ASSERT_EQ(sbs_uuid_validate("12345678-1234-5234-9234-123456789abc"), false);
}

SBS_TEST(uuid, validate_bad_variant) {
    /* Variant nibble at pos 19 must be 8,9,a,b */
    SBS_ASSERT_EQ(sbs_uuid_validate("12345678-1234-4234-0234-123456789abc"), false);
}

SBS_TEST(uuid, validate_valid_uuid) {
    SBS_ASSERT_EQ(sbs_uuid_validate("550e8400-e29b-41d4-a716-446655440000"), true);
}

SBS_TEST(uuid, validate_bad_hex_char) {
    SBS_ASSERT_EQ(sbs_uuid_validate("550g8400-e29b-41d4-a716-446655440000"), false);
}

SBS_TEST_MAIN()
