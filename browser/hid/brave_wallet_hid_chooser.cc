/* Copyright (c) 2025 The Brave Authors. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "brave/browser/hid/brave_wallet_hid_chooser.h"

#include <utility>

#include "base/functional/bind.h"
#include "base/task/sequenced_task_runner.h"
#include "brave/components/constants/webui_url_constants.h"
#include "chrome/browser/ui/hid/hid_chooser_controller.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/common/url_constants.h"

namespace {

constexpr uint16_t kLedgerVendorId = 0x2c97;

// Matches `filters: [{ vendorId: LEDGER_VENDOR_ID }]` — vendor only, no
// product id, no usage filter, no exclusion filters, exactly one filter.
bool IsExactlyLedgerVendorOnlyRequest(
    const std::vector<blink::mojom::HidDeviceFilterPtr>& filters,
    const std::vector<blink::mojom::HidDeviceFilterPtr>& exclusion_filters) {
  if (!exclusion_filters.empty() || filters.size() != 1u) {
    return false;
  }
  const auto& f = filters[0];
  return f->device_ids && f->device_ids->is_vendor() &&
         f->device_ids->get_vendor() == kLedgerVendorId && !f->usage;
}

}  // namespace

// static
bool BraveWalletHidChooser::IsBraveWalletMainFrameOrigin(
    content::RenderFrameHost* rfh) {
  if (!rfh) {
    return false;
  }

  const auto& origin = rfh->GetMainFrame()->GetLastCommittedOrigin();
  return origin.scheme() == content::kChromeUIScheme &&
         (origin.host() == kWalletPageHost ||
          origin.host() == kWalletPanelHost);
}

BraveWalletHidChooser::BraveWalletHidChooser(
    content::RenderFrameHost* render_frame_host,
    std::vector<blink::mojom::HidDeviceFilterPtr> filters,
    std::vector<blink::mojom::HidDeviceFilterPtr> exclusion_filters,
    content::HidChooser::Callback callback)
    : is_valid_ledger_request_(
          IsExactlyLedgerVendorOnlyRequest(filters, exclusion_filters)),
      controller_(
          std::make_unique<HidChooserController>(render_frame_host,
                                                 std::move(filters),
                                                 std::move(exclusion_filters),
                                                 std::move(callback))) {
  controller_->set_view(this);
}

BraveWalletHidChooser::~BraveWalletHidChooser() {
  if (controller_) {
    controller_->set_view(nullptr);
  }
}

void BraveWalletHidChooser::OnOptionsInitialized() {
  if (!is_valid_ledger_request_ || controller_->NumOptions() == 0) {
    // Can't destroy the controller synchronously here — we're inside its
    // OnGotDevices call stack.  Post a task to tear it down; the controller's
    // destructor invokes the callback with an empty device list.
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(&BraveWalletHidChooser::RejectAndClose,
                                  weak_factory_.GetWeakPtr()));
    return;
  }

  controller_->Select({0});
}

void BraveWalletHidChooser::OnOptionAdded(size_t index) {}
void BraveWalletHidChooser::OnOptionRemoved(size_t index) {}
void BraveWalletHidChooser::OnOptionUpdated(size_t index) {}
void BraveWalletHidChooser::OnAdapterEnabledChanged(bool enabled) {}
void BraveWalletHidChooser::OnRefreshStateChanged(bool refreshing) {}

void BraveWalletHidChooser::RejectAndClose() {
  if (controller_) {
    controller_->set_view(nullptr);
    controller_->Close();
    controller_.reset();
  }
}
