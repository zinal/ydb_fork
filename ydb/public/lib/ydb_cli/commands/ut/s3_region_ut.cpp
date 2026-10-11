#include <ydb/public/lib/ydb_cli/common/aws.h>

#include <library/cpp/testing/unittest/registar.h>

using namespace NYdb::NConsoleClient;

Y_UNIT_TEST_SUITE(ResolveS3SigningRegion) {
    Y_UNIT_TEST(ExplicitRegionWins) {
        UNIT_ASSERT_VALUES_EQUAL(ResolveS3SigningRegion("storage.yandexcloud.kz", "custom"), "custom");
        UNIT_ASSERT_VALUES_EQUAL(ResolveS3SigningRegion("https://example.com", "eu-central-1"), "eu-central-1");
    }

    Y_UNIT_TEST(YandexCloudKazakhstan) {
        UNIT_ASSERT_VALUES_EQUAL(ResolveS3SigningRegion("storage.yandexcloud.kz"), "kz1");
        UNIT_ASSERT_VALUES_EQUAL(ResolveS3SigningRegion("https://storage.yandexcloud.kz"), "kz1");
        UNIT_ASSERT_VALUES_EQUAL(ResolveS3SigningRegion("https://storage.yandexcloud.kz/"), "kz1");
        UNIT_ASSERT_VALUES_EQUAL(ResolveS3SigningRegion("https://storage.yandexcloud.kz:443"), "kz1");
        UNIT_ASSERT_VALUES_EQUAL(ResolveS3SigningRegion("HTTP://STORAGE.YANDEXCLOUD.KZ"), "kz1");
    }

    Y_UNIT_TEST(YandexCloudRussia) {
        UNIT_ASSERT_VALUES_EQUAL(ResolveS3SigningRegion("storage.yandexcloud.net"), "ru-central1");
        UNIT_ASSERT_VALUES_EQUAL(ResolveS3SigningRegion("https://storage.yandexcloud.net/bucket"), "ru-central1");
    }

    Y_UNIT_TEST(OtherEndpointsStayUnset) {
        UNIT_ASSERT_VALUES_EQUAL(ResolveS3SigningRegion("https://ydb.s3.local"), "");
        UNIT_ASSERT_VALUES_EQUAL(ResolveS3SigningRegion("localhost:9000"), "");
        UNIT_ASSERT_VALUES_EQUAL(ResolveS3SigningRegion(""), "");
    }
} // Y_UNIT_TEST_SUITE(ResolveS3SigningRegion)
