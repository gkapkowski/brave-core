/* Copyright (c) 2019 The Brave Authors. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "bat/ads/internal/locale_helper.h"

#include <string>

#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"

// npm run test -- brave_unit_tests --filter=BatAds*

namespace ads {

class BatAdsLocaleTest : public ::testing::Test {
 protected:
  BatAdsLocaleTest() {
    // You can do set-up work for each test here
  }

  ~BatAdsLocaleTest() override {
    // You can do clean-up work that doesn't throw exceptions here
  }

  // If the constructor and destructor are not enough for setting up and
  // cleaning up each test, you can use the following methods

  void SetUp() override {
    // Code here will be called immediately after the constructor (right before
    // each test)
  }

  void TearDown() override {
    // Code here will be called immediately after each test (right before the
    // destructor)
  }

  // Objects declared here can be used by all tests in the test case
};

TEST_F(BatAdsLocaleTest,
    LanguageCodeForEnglish) {
  // Arrange
  const std::string locale = "en";

  // Act
  const std::string language_code = helper::Locale::GetLanguageCode(locale);

  // Assert
  const std::string expected_language_code = "en";
  EXPECT_EQ(expected_language_code, language_code);
}

TEST_F(BatAdsLocaleTest,
    RegionCodeForEnglish) {
  // Arrange
  const std::string locale = "en";

  // Act
  const std::string region_code = helper::Locale::GetRegionCode(locale);

  // Assert
  const std::string expected_region_code = "US";
  EXPECT_EQ(expected_region_code, region_code);
}

TEST_F(BatAdsLocaleTest,
    LanguageCodeForLanguageCodeDashRegionCode) {
  // Arrange
  const std::string locale = "en-US";

  // Act
  const std::string language_code = helper::Locale::GetLanguageCode(locale);

  // Assert
  const std::string expected_language_code = "en";
  EXPECT_EQ(expected_language_code, language_code);
}

TEST_F(BatAdsLocaleTest,
    RegionCodeForLanguageCodeDashRegionCode) {
  // Arrange
  const std::string locale = "en-US";

  // Act
  const std::string region_code = helper::Locale::GetRegionCode(locale);

  // Assert
  const std::string expected_region_code = "US";
  EXPECT_EQ(expected_region_code, region_code);
}

TEST_F(BatAdsLocaleTest,
    LanguageCodeForLanguageCodeDashWorld) {
  // Arrange
  const std::string locale = "en-101";

  // Act
  const std::string language_code = helper::Locale::GetLanguageCode(locale);

  // Assert
  const std::string expected_language_code = "en";
  EXPECT_EQ(expected_language_code, language_code);
}

TEST_F(BatAdsLocaleTest,
    RegionCodeForLanguageCodeDashWorld) {
  // Arrange
  const std::string locale = "en-101";

  // Act
  const std::string region_code = helper::Locale::GetRegionCode(locale);

  // Assert
  const std::string expected_region_code = "101";
  EXPECT_EQ(expected_region_code, region_code);
}

TEST_F(BatAdsLocaleTest,
    LanguageCodeForLanguageCodeDashRegionCodeDotEncoding) {
  // Arrange
  const std::string locale = "en-US.UTF-8";

  // Act
  const std::string language_code = helper::Locale::GetLanguageCode(locale);

  // Assert
  const std::string expected_language_code = "en";
  EXPECT_EQ(expected_language_code, language_code);
}

TEST_F(BatAdsLocaleTest,
    RegionCodeForLanguageCodeDashRegionCodeDotEncoding) {
  // Arrange
  const std::string locale = "en-US.UTF-8";

  // Act
  const std::string region_code = helper::Locale::GetRegionCode(locale);

  // Assert
  const std::string expected_region_code = "US";
  EXPECT_EQ(expected_region_code, region_code);
}

TEST_F(BatAdsLocaleTest,
    LanguageCodeForLanguageCodeDashScriptDashRegionCode) {
  // Arrange
  const std::string locale = "az-Latn-AZ";

  // Act
  const std::string language_code = helper::Locale::GetLanguageCode(locale);

  // Assert
  const std::string expected_language_code = "az";
  EXPECT_EQ(expected_language_code, language_code);
}

TEST_F(BatAdsLocaleTest,
    RegionCodeForLanguageCodeDashScriptDashRegionCode) {
  // Arrange
  const std::string locale = "az-Latn-AZ";

  // Act
  const std::string region_code = helper::Locale::GetRegionCode(locale);

  // Assert
  const std::string expected_region_code = "AZ";
  EXPECT_EQ(expected_region_code, region_code);
}

TEST_F(BatAdsLocaleTest,
    LanguageCodeForLanguageCodeUnderscoreRegionCode) {
  // Arrange
  const std::string locale = "en_US";

  // Act
  const std::string language_code = helper::Locale::GetLanguageCode(locale);

  // Assert
  const std::string expected_language_code = "en";
  EXPECT_EQ(expected_language_code, language_code);
}

TEST_F(BatAdsLocaleTest,
    RegionCodeForLanguageCodeUnderscoreRegionCode) {
  // Arrange
  const std::string locale = "en_US";

  // Act
  const std::string region_code = helper::Locale::GetRegionCode(locale);

  // Assert
  const std::string expected_region_code = "US";
  EXPECT_EQ(expected_region_code, region_code);
}

TEST_F(BatAdsLocaleTest,
    LanguageCodeForLanguageCodeUnderscoreRegionCodeDotEncoding) {
  // Arrange
  const std::string locale = "en_US.UTF-8";

  // Act
  const std::string language_code = helper::Locale::GetLanguageCode(locale);

  // Assert
  const std::string expected_language_code = "en";
  EXPECT_EQ(expected_language_code, language_code);
}

TEST_F(BatAdsLocaleTest,
    RegionCodeForLanguageCodeUnderscoreRegionCodeDotEncoding) {
  // Arrange
  const std::string locale = "en_US.UTF-8";

  // Act
  const std::string region_code = helper::Locale::GetRegionCode(locale);

  // Assert
  const std::string expected_region_code = "US";
  EXPECT_EQ(expected_region_code, region_code);
}

TEST_F(BatAdsLocaleTest,
    LanguageCodeForLanguageCodeUnderscoreScriptUnderscoreRegionCode) {
  // Arrange
  const std::string locale = "az_Latn_AZ";

  // Act
  const std::string language_code = helper::Locale::GetLanguageCode(locale);

  // Assert
  const std::string expected_language_code = "az";
  EXPECT_EQ(expected_language_code, language_code);
}

TEST_F(BatAdsLocaleTest,
    RegionCodeForLanguageCodeUnderscoreScriptUnderscoreRegionCode) {
  // Arrange
  const std::string locale = "az_Latn_AZ";

  // Act
  const std::string region_code = helper::Locale::GetRegionCode(locale);

  // Assert
  const std::string expected_region_code = "AZ";
  EXPECT_EQ(expected_region_code, region_code);
}

}  // namespace ads
