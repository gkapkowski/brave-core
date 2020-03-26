/* Copyright 2020 The Brave Authors. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "brave/browser/brave_rewards/checkout_dialog_controller.h"

#include <utility>

namespace brave_rewards {

CheckoutDialogController::CheckoutDialogController() = default;

CheckoutDialogController::~CheckoutDialogController() = default;

void CheckoutDialogController::NotifyPaymentAborted() {
  for (auto& observer : observers_) {
    observer.OnPaymentAborted();
  }
}

void CheckoutDialogController::NotifyPaymentFulfilled() {
  payment_fulfilled_ = true;
  for (auto& observer : observers_) {
    observer.OnPaymentFulfilled();
  }
}

void CheckoutDialogController::SetOnDialogClosedCallback(
    OnDialogClosedCallback callback) {
  dialog_closed_callback_ = std::move(callback);
}

void CheckoutDialogController::SetOnPaymentReadyCallback(
    OnPaymentReadyCallback callback) {
  payment_ready_callback_ = std::move(callback);
}

void CheckoutDialogController::AddObserver(Observer* observer) {
  observers_.AddObserver(observer);
}

void CheckoutDialogController::RemoveObserver(Observer* observer) {
  observers_.RemoveObserver(observer);
}

void CheckoutDialogController::NotifyDialogClosed() {
  if (dialog_closed_callback_) {
    std::move(dialog_closed_callback_).Run(payment_fulfilled_);
  }
}

void CheckoutDialogController::NotifyPaymentReady() {
  if (payment_ready_callback_) {
    std::move(payment_ready_callback_).Run();
  }
}

}  // namespace brave_rewards
