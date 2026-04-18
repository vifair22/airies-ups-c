#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include <cmocka.h>
#include <string.h>

#include "cli/cli.h"

/* --- opt_get --- */

static void test_opt_get_found(void **state)
{
    (void)state;
    char *argv[] = { "cmd", "--socket", "/tmp/test.sock", "status" };
    const char *val = opt_get(4, argv, 1, "--socket");
    assert_non_null(val);
    assert_string_equal(val, "/tmp/test.sock");
}

static void test_opt_get_not_found(void **state)
{
    (void)state;
    char *argv[] = { "cmd", "status" };
    assert_null(opt_get(2, argv, 1, "--socket"));
}

static void test_opt_get_last_arg(void **state)
{
    (void)state;
    /* --socket is the last arg, so there's no value after it */
    char *argv[] = { "cmd", "--socket" };
    assert_null(opt_get(2, argv, 1, "--socket"));
}

/* --- opt_has --- */

static void test_opt_has_present(void **state)
{
    (void)state;
    char *argv[] = { "cmd", "--verbose", "status" };
    assert_int_equal(opt_has(3, argv, 1, "--verbose"), 1);
}

static void test_opt_has_absent(void **state)
{
    (void)state;
    char *argv[] = { "cmd", "status" };
    assert_int_equal(opt_has(2, argv, 1, "--verbose"), 0);
}

/* --- is_flag_token --- */

static void test_is_flag_token_valid(void **state)
{
    (void)state;
    assert_int_equal(is_flag_token("--help"), 1);
    assert_int_equal(is_flag_token("--socket"), 1);
    assert_int_equal(is_flag_token("--"), 1);
}

static void test_is_flag_token_invalid(void **state)
{
    (void)state;
    assert_int_equal(is_flag_token("status"), 0);
    assert_int_equal(is_flag_token("-h"), 0);
    assert_int_equal(is_flag_token(""), 0);
    assert_int_equal(is_flag_token(NULL), 0);
}

/* --- has_help_flag --- */

static void test_has_help_flag_long(void **state)
{
    (void)state;
    char *argv[] = { "cmd", "--help" };
    assert_int_equal(has_help_flag(2, argv, 1), 1);
}

static void test_has_help_flag_short(void **state)
{
    (void)state;
    char *argv[] = { "cmd", "-h" };
    assert_int_equal(has_help_flag(2, argv, 1), 1);
}

static void test_has_help_flag_absent(void **state)
{
    (void)state;
    char *argv[] = { "cmd", "status" };
    assert_int_equal(has_help_flag(2, argv, 1), 0);
}

static void test_has_help_flag_mixed(void **state)
{
    (void)state;
    char *argv[] = { "cmd", "--socket", "/tmp/s", "--help" };
    assert_int_equal(has_help_flag(4, argv, 1), 1);
}

/* --- find_subcmd --- */

static void test_find_subcmd_simple(void **state)
{
    (void)state;
    static const flag_spec_t specs[] = {
        { "--socket", 1 },
    };
    char *argv[] = { "cmd", "status" };
    const char *sub = find_subcmd(2, argv, 1, specs, 1);
    assert_non_null(sub);
    assert_string_equal(sub, "status");
}

static void test_find_subcmd_after_flag(void **state)
{
    (void)state;
    static const flag_spec_t specs[] = {
        { "--socket", 1 },
    };
    char *argv[] = { "cmd", "--socket", "/tmp/s", "status" };
    const char *sub = find_subcmd(4, argv, 1, specs, 1);
    assert_non_null(sub);
    assert_string_equal(sub, "status");
}

static void test_find_subcmd_none(void **state)
{
    (void)state;
    static const flag_spec_t specs[] = {
        { "--socket", 1 },
    };
    char *argv[] = { "cmd", "--socket", "/tmp/s" };
    assert_null(find_subcmd(3, argv, 1, specs, 1));
}

static void test_find_subcmd_skips_flag_values(void **state)
{
    (void)state;
    static const flag_spec_t specs[] = {
        { "--socket", 1 },
        { "--verbose", 0 },
    };
    /* "status" should be found after --socket consumes "/tmp/s" and --verbose has no value */
    char *argv[] = { "cmd", "--verbose", "--socket", "/tmp/s", "events" };
    const char *sub = find_subcmd(5, argv, 1, specs, 2);
    assert_non_null(sub);
    assert_string_equal(sub, "events");
}

/* --- validate_options --- */

static void test_validate_options_valid(void **state)
{
    (void)state;
    static const flag_spec_t specs[] = {
        { "--socket", 1 },
    };
    const char *positionals[] = { "status", "events" };

    set_topic(NULL);
    char *argv[] = { "cmd", "--socket", "/tmp/s", "status" };
    assert_int_equal(validate_options(4, argv, 1, specs, 1, positionals, 2), 0);
}

static void test_validate_options_unknown_flag(void **state)
{
    (void)state;
    static const flag_spec_t specs[] = {
        { "--socket", 1 },
    };

    set_topic(NULL);
    char *argv[] = { "cmd", "--bogus" };
    assert_int_equal(validate_options(2, argv, 1, specs, 1, NULL, 0), 1);
}

static void test_validate_options_missing_value(void **state)
{
    (void)state;
    static const flag_spec_t specs[] = {
        { "--socket", 1 },
    };

    set_topic(NULL);
    /* --socket requires a value but none follows */
    char *argv[] = { "cmd", "--socket" };
    assert_int_equal(validate_options(2, argv, 1, specs, 1, NULL, 0), 1);
}

static void test_validate_options_unknown_positional(void **state)
{
    (void)state;
    static const flag_spec_t specs[] = {
        { "--socket", 1 },
    };
    const char *positionals[] = { "status" };

    set_topic(NULL);
    char *argv[] = { "cmd", "boguscmd" };
    assert_int_equal(validate_options(2, argv, 1, specs, 1, positionals, 1), 1);
}

static void test_validate_options_help_always_ok(void **state)
{
    (void)state;
    /* --help should be accepted even with no specs */
    set_topic(NULL);
    char *argv[] = { "cmd", "--help" };
    assert_int_equal(validate_options(2, argv, 1, NULL, 0, NULL, 0), 0);
}

/* --- set_topic / current_topic --- */

static void test_topic_set_get(void **state)
{
    (void)state;
    set_topic("events");
    assert_string_equal(current_topic(), "events");

    set_topic(NULL);
    assert_null(current_topic());
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        /* opt_get */
        cmocka_unit_test(test_opt_get_found),
        cmocka_unit_test(test_opt_get_not_found),
        cmocka_unit_test(test_opt_get_last_arg),
        /* opt_has */
        cmocka_unit_test(test_opt_has_present),
        cmocka_unit_test(test_opt_has_absent),
        /* is_flag_token */
        cmocka_unit_test(test_is_flag_token_valid),
        cmocka_unit_test(test_is_flag_token_invalid),
        /* has_help_flag */
        cmocka_unit_test(test_has_help_flag_long),
        cmocka_unit_test(test_has_help_flag_short),
        cmocka_unit_test(test_has_help_flag_absent),
        cmocka_unit_test(test_has_help_flag_mixed),
        /* find_subcmd */
        cmocka_unit_test(test_find_subcmd_simple),
        cmocka_unit_test(test_find_subcmd_after_flag),
        cmocka_unit_test(test_find_subcmd_none),
        cmocka_unit_test(test_find_subcmd_skips_flag_values),
        /* validate_options */
        cmocka_unit_test(test_validate_options_valid),
        cmocka_unit_test(test_validate_options_unknown_flag),
        cmocka_unit_test(test_validate_options_missing_value),
        cmocka_unit_test(test_validate_options_unknown_positional),
        cmocka_unit_test(test_validate_options_help_always_ok),
        /* topic */
        cmocka_unit_test(test_topic_set_get),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
