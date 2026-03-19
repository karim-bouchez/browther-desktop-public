// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

#include "brave/browser/ai_chat/pdf_text_extractor.h"

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/test/test_future.h"
#include "chrome/test/base/testing_profile.h"
#include "content/public/test/browser_task_environment.h"
#include "content/public/test/test_renderer_host.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace ai_chat {

class PdfTextExtractorTest : public content::RenderViewHostTestHarness {
 public:
  PdfTextExtractorTest()
      : content::RenderViewHostTestHarness(
            base::test::TaskEnvironment::TimeSource::MOCK_TIME) {}

  std::unique_ptr<content::BrowserContext> CreateBrowserContext() override {
    return std::make_unique<TestingProfile>();
  }
};

// Without a real PDF viewer extension, the hidden WebContents will never
// produce a PDFDocumentHelper. The extraction should time out and return
// nullopt.
TEST_F(PdfTextExtractorTest, BytesOverload_TimeoutReturnsNullopt) {
  auto extractor = std::make_unique<PdfTextExtractor>();
  base::test::TestFuture<std::optional<std::string>> future;

  std::vector<uint8_t> dummy_pdf = {0x25, 0x50, 0x44, 0x46};  // %PDF
  extractor->ExtractText(browser_context(), std::move(dummy_pdf),
                         future.GetCallback());

  // Let the ThreadPool temp-file write complete.
  task_environment()->RunUntilIdle();

  // Fast-forward past the 30s extraction timeout.
  task_environment()->FastForwardBy(base::Seconds(31));

  auto result = future.Take();
  EXPECT_FALSE(result.has_value());
}

// Same timeout test but using the file-path overload (no temp file).
TEST_F(PdfTextExtractorTest, PathOverload_TimeoutReturnsNullopt) {
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  base::FilePath pdf_path = temp_dir.GetPath().AppendASCII("test.pdf");
  ASSERT_TRUE(base::WriteFile(pdf_path, "%PDF-1.4 dummy content for test"));

  auto extractor = std::make_unique<PdfTextExtractor>();
  base::test::TestFuture<std::optional<std::string>> future;

  extractor->ExtractText(browser_context(), pdf_path, future.GetCallback());

  task_environment()->FastForwardBy(base::Seconds(31));

  auto result = future.Take();
  EXPECT_FALSE(result.has_value());
}

// Destroying the extractor while an extraction is in-flight must not crash
// or leak.
TEST_F(PdfTextExtractorTest, DestroyDuringExtraction_NoCrash) {
  auto extractor = std::make_unique<PdfTextExtractor>();

  bool callback_called = false;
  std::vector<uint8_t> dummy_pdf = {0x25, 0x50, 0x44, 0x46};
  extractor->ExtractText(
      browser_context(), std::move(dummy_pdf),
      base::BindOnce(
          [](bool* called, std::optional<std::string>) { *called = true; },
          &callback_called));

  // Let the temp file write complete so the WebContents is created.
  task_environment()->RunUntilIdle();

  // Destroy while extraction is in progress — should not crash.
  extractor.reset();

  // Drain any weak-pointer cancellations or cleanup tasks.
  task_environment()->RunUntilIdle();
}

// The bytes overload writes to a temp file. After the extraction completes
// (via timeout), the temp file should be deleted.
TEST_F(PdfTextExtractorTest, BytesOverload_TempFileDeletedAfterTimeout) {
  auto extractor = std::make_unique<PdfTextExtractor>();
  base::test::TestFuture<std::optional<std::string>> future;

  std::vector<uint8_t> dummy_pdf = {0x25, 0x50, 0x44, 0x46};
  extractor->ExtractText(browser_context(), std::move(dummy_pdf),
                         future.GetCallback());

  // Let the temp file write complete.
  task_environment()->RunUntilIdle();

  // Fast-forward past timeout to trigger cleanup.
  task_environment()->FastForwardBy(base::Seconds(31));

  ASSERT_TRUE(future.Wait());

  // Let the DeleteTempFile task on the ThreadPool run.
  task_environment()->RunUntilIdle();
}

// The file-path overload should NOT delete the original file after extraction.
TEST_F(PdfTextExtractorTest, PathOverload_OriginalFileNotDeleted) {
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  base::FilePath pdf_path = temp_dir.GetPath().AppendASCII("keep_me.pdf");
  ASSERT_TRUE(base::WriteFile(pdf_path, "%PDF-1.4 dummy content for test"));

  auto extractor = std::make_unique<PdfTextExtractor>();
  base::test::TestFuture<std::optional<std::string>> future;

  extractor->ExtractText(browser_context(), pdf_path, future.GetCallback());

  task_environment()->FastForwardBy(base::Seconds(31));

  ASSERT_TRUE(future.Wait());
  task_environment()->RunUntilIdle();

  // The original file must still exist — only temp files are cleaned up.
  EXPECT_TRUE(base::PathExists(pdf_path));
}

}  // namespace ai_chat
