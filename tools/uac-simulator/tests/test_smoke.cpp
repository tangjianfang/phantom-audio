#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <uac/version.h>

TEST_CASE("version banner") {
    CHECK(uac::kVersion[0] == 'v');
    CHECK(uac::kVersion[1] == '0');
}
