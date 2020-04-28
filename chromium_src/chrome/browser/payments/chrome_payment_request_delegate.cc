// Copyright (c) 2020 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// you can obtain one at http://mozilla.org/MPL/2.0/.

#include "brave/browser/brave_rewards/checkout_dialog.h"

#define BRAVE_SHOW_BAT_PAYMENT_UI                                    \
  if (request->spec() &&                                             \
      request->spec()->stringified_method_data().count("bat") > 0) { \
    brave_rewards::ShowCheckoutDialog(web_contents_, request);       \
    return;                                                          \
  }
#include "../../../../../chrome/browser/payments/chrome_payment_request_delegate.cc"
#undef BRAVE_SHOW_BAT_PAYMENT_UI
