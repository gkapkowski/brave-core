/* Copyright (c) 2020 The Brave Authors. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "chrome/install_static/install_modes.h"

namespace install_static {

namespace {

std::wstring GetClientsKeyPathForApp(const wchar_t* app_guid) {
  return std::wstring(L"Software\\BraveSoftware\\Update\\Clients\\")
      .append(app_guid);
}

std::wstring GetClientStateKeyPathForApp(const wchar_t* app_guid) {
  return std::wstring(L"Software\\BraveSoftware\\Update\\ClientState\\")
      .append(app_guid);
}

std::wstring GetClientStateMediumKeyPathForApp(const wchar_t* app_guid) {
  return std::wstring(L"Software\\BraveSoftware\\Update\\ClientStateMedium\\")
      .append(app_guid);
}

}  // namespace

std::wstring GetClientsKeyPath(const wchar_t* app_guid) {
  return GetClientsKeyPathForApp(app_guid);
}

std::wstring GetClientStateKeyPath(const wchar_t* app_guid) {
  return GetClientStateKeyPathForApp(app_guid);
}

std::wstring GetBinariesClientsKeyPath() {
  return GetClientsKeyPathForApp(kBinariesAppGuid);
}

std::wstring GetBinariesClientStateKeyPath() {
  return GetClientStateKeyPathForApp(kBinariesAppGuid);
}

std::wstring GetClientStateMediumKeyPath(const wchar_t* app_guid) {
  return GetClientStateMediumKeyPathForApp(app_guid);
}

std::wstring GetBinariesClientStateMediumKeyPath() {
  return GetClientStateMediumKeyPathForApp(kBinariesAppGuid);
}

}  // namespace install_static
