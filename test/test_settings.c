#include <string.h>

#include "airmidi/settings.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static const uint8_t mac[6] = {0x10, 0xBD, 0xA3, 0x8F, 0x75, 0x30};

static void test_defaults_are_valid(void) {
    settings_t s;
    settings_defaults(&s, mac);
    TEST_ASSERT_EQUAL_STRING("airmidi-7530", s.name);
    TEST_ASSERT_EQUAL(AIRMIDI_MODE_PLAY, s.mode);
    TEST_ASSERT_EQUAL(500, s.sysex_timeout_ms);
    TEST_ASSERT_NULL(settings_check(&s));
}

static void test_name_rules(void) {
    settings_t s;
    settings_defaults(&s, mac);
    strcpy(s.name, "");
    TEST_ASSERT_NOT_NULL(settings_check(&s));
    strcpy(s.name, "stage left");
    TEST_ASSERT_NOT_NULL(settings_check(&s));
    strcpy(s.name, "-box");
    TEST_ASSERT_NOT_NULL(settings_check(&s));
    strcpy(s.name, "box-");
    TEST_ASSERT_NOT_NULL(settings_check(&s));
    strcpy(s.name, "Stage-Left-2");
    TEST_ASSERT_NULL(settings_check(&s));
    memset(s.name, 'a', sizeof s.name);  // not terminated: must not read past the array
    TEST_ASSERT_NOT_NULL(settings_check(&s));
    memset(s.name, 'a', SETTINGS_NAME_MAX);
    s.name[SETTINGS_NAME_MAX] = 0;
    TEST_ASSERT_NULL(settings_check(&s));
}

static void test_mode_and_timeout_ranges(void) {
    settings_t s;
    settings_defaults(&s, mac);
    s.mode = AIRMIDI_MODE_COUNT;
    TEST_ASSERT_NOT_NULL(settings_check(&s));
    s.mode = AIRMIDI_MODE_SETUP;
    TEST_ASSERT_NULL(settings_check(&s));
    s.sysex_timeout_ms = SETTINGS_SYSEX_TIMEOUT_MIN - 1;
    TEST_ASSERT_NOT_NULL(settings_check(&s));
    s.sysex_timeout_ms = SETTINGS_SYSEX_TIMEOUT_MAX + 1;
    TEST_ASSERT_NOT_NULL(settings_check(&s));
    s.sysex_timeout_ms = SETTINGS_SYSEX_TIMEOUT_MAX;
    TEST_ASSERT_NULL(settings_check(&s));
}

static void test_mode_names(void) {
    TEST_ASSERT_EQUAL_STRING("play", airmidi_mode_name(AIRMIDI_MODE_PLAY));
    TEST_ASSERT_EQUAL_STRING("network", airmidi_mode_name(AIRMIDI_MODE_NETWORK));
    TEST_ASSERT_EQUAL_STRING("setup", airmidi_mode_name(AIRMIDI_MODE_SETUP));
    TEST_ASSERT_EQUAL_STRING("?", airmidi_mode_name(AIRMIDI_MODE_COUNT));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_defaults_are_valid);
    RUN_TEST(test_name_rules);
    RUN_TEST(test_mode_and_timeout_ranges);
    RUN_TEST(test_mode_names);
    return UNITY_END();
}
