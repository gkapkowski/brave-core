/* Copyright (c) 2020 The Brave Authors. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef BAT_CONFIRMATIONS_INTERNAL_SECURITY_UTILS_H_
#define BAT_CONFIRMATIONS_INTERNAL_SECURITY_UTILS_H_

#include <stdint.h>

#include <string>
#include <vector>
#include <map>

#include "wrapper.hpp"

namespace confirmations {

using challenge_bypass_ristretto::Token;
using challenge_bypass_ristretto::BlindedToken;

std::string Sign(
    const std::map<std::string, std::string>& headers,
    const std::string& key_id,
    const std::vector<uint8_t>& private_key);

std::vector<Token> GenerateTokens(const int count);

std::vector<BlindedToken> BlindTokens(
    const std::vector<Token>& tokens);

std::vector<uint8_t> Sha256(
    const std::string& string);

std::string Base64Encode(
    const std::vector<uint8_t>& data);

}  // namespace confirmations

#endif  // BAT_CONFIRMATIONS_INTERNAL_SECURITY_UTILS_H_
