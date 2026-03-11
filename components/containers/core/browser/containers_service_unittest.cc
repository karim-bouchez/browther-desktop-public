// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

#include "brave/components/containers/core/browser/containers_service.h"

#include <memory>
#include <string>
#include <utility>

#include "base/containers/flat_set.h"
#include "base/test/scoped_feature_list.h"
#include "brave/components/containers/core/browser/prefs.h"
#include "brave/components/containers/core/browser/prefs_registration.h"
#include "brave/components/containers/core/common/features.h"
#include "brave/components/containers/core/mojom/containers.mojom.h"
#include "components/sync_preferences/testing_pref_service_syncable.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace containers {

namespace {

mojom::ContainerPtr MakeContainer(std::string id,
                                  std::string name,
                                  mojom::Icon icon,
                                  SkColor color) {
  return mojom::Container::New(std::move(id), std::move(name), icon, color);
}

class FakeContainersServiceDelegate : public ContainersService::Delegate {
 public:
  void StartObserving(base::RepeatingClosure on_references_changed) override {
    on_references_changed_ = std::move(on_references_changed);
  }

  base::flat_set<std::string> GetReferencedContainerIds() const override {
    return referenced_ids_;
  }

  void DeleteContainerStorage(
      const std::string& id,
      base::OnceCallback<void(bool success)> callback) override {
    delete_requests_.push_back(id);
    std::move(callback).Run(delete_result_);
  }

  void SetReferencedIds(base::flat_set<std::string> ids) {
    referenced_ids_ = std::move(ids);
  }

  void NotifyReferencesChanged() {
    ASSERT_TRUE(on_references_changed_);
    on_references_changed_.Run();
  }

  void set_delete_result(bool delete_result) { delete_result_ = delete_result; }
  const std::vector<std::string>& delete_requests() const {
    return delete_requests_;
  }

 private:
  base::flat_set<std::string> referenced_ids_;
  base::RepeatingClosure on_references_changed_;
  bool delete_result_ = true;
  std::vector<std::string> delete_requests_;
};

}  // namespace

class ContainersServiceTest : public testing::Test {
 protected:
  void SetUp() override {
    feature_list_.InitAndEnableFeature(features::kContainers);
    RegisterProfilePrefs(prefs_.registry());

    auto delegate = std::make_unique<FakeContainersServiceDelegate>();
    delegate_ = delegate.get();
    service_ =
        std::make_unique<ContainersService>(&prefs_, std::move(delegate));
    service_->Initialize();
  }

  void SetSyncedContainers(std::vector<mojom::ContainerPtr> containers) {
    SetContainersToPrefs(containers, prefs_);
  }

  base::test::ScopedFeatureList feature_list_;
  sync_preferences::TestingPrefServiceSyncable prefs_;
  std::unique_ptr<ContainersService> service_;
  raw_ptr<FakeContainersServiceDelegate> delegate_ = nullptr;
};

TEST_F(ContainersServiceTest, RuntimeLookupFallsBackToUsedContainer) {
  auto container = MakeContainer("container-id", "Personal",
                                 mojom::Icon::kPersonal, SK_ColorBLUE);
  std::vector<mojom::ContainerPtr> synced_containers;
  synced_containers.push_back(container.Clone());
  SetSyncedContainers(std::move(synced_containers));
  service_->MarkContainerUsed(container.Clone());

  delegate_->SetReferencedIds({"container-id"});
  SetSyncedContainers({});

  auto cached = service_->GetContainerById("container-id");
  ASSERT_TRUE(cached);
  EXPECT_EQ(cached->name, "Personal");
  EXPECT_EQ(cached->icon, mojom::Icon::kPersonal);
  EXPECT_EQ(cached->background_color, SK_ColorBLUE);
}

TEST_F(ContainersServiceTest, CleanupBlockedWhileContainerIsReferenced) {
  auto container =
      MakeContainer("container-id", "Work", mojom::Icon::kWork, SK_ColorGREEN);
  std::vector<mojom::ContainerPtr> synced_containers;
  synced_containers.push_back(container.Clone());
  SetSyncedContainers(std::move(synced_containers));
  service_->MarkContainerUsed(container.Clone());

  delegate_->SetReferencedIds({"container-id"});
  SetSyncedContainers({});

  EXPECT_TRUE(delegate_->delete_requests().empty());
  EXPECT_TRUE(GetUsedContainerFromPrefs(prefs_, "container-id"));
}

TEST_F(ContainersServiceTest, CleanupRunsWhenReferencesDisappear) {
  auto container = MakeContainer("container-id", "Shopping",
                                 mojom::Icon::kShopping, SK_ColorRED);
  std::vector<mojom::ContainerPtr> synced_containers;
  synced_containers.push_back(container.Clone());
  SetSyncedContainers(std::move(synced_containers));
  service_->MarkContainerUsed(container.Clone());

  delegate_->SetReferencedIds({"container-id"});
  SetSyncedContainers({});

  delegate_->SetReferencedIds({});
  delegate_->NotifyReferencesChanged();

  ASSERT_EQ(delegate_->delete_requests().size(), 1u);
  EXPECT_EQ(delegate_->delete_requests()[0], "container-id");
  EXPECT_FALSE(GetUsedContainerFromPrefs(prefs_, "container-id"));
}

TEST_F(ContainersServiceTest, CleanupFailureKeepsUsedSnapshot) {
  auto container = MakeContainer("container-id", "Shopping",
                                 mojom::Icon::kShopping, SK_ColorRED);
  std::vector<mojom::ContainerPtr> synced_containers;
  synced_containers.push_back(container.Clone());
  SetSyncedContainers(std::move(synced_containers));
  service_->MarkContainerUsed(container.Clone());

  delegate_->SetReferencedIds({"container-id"});
  SetSyncedContainers({});

  delegate_->set_delete_result(false);
  delegate_->SetReferencedIds({});
  delegate_->NotifyReferencesChanged();

  ASSERT_EQ(delegate_->delete_requests().size(), 1u);
  EXPECT_TRUE(GetUsedContainerFromPrefs(prefs_, "container-id"));
}

}  // namespace containers
