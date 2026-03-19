// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

#include "brave/browser/ai_chat/pdf_text_extractor.h"

#include <utility>

#include "base/barrier_callback.h"
#include "base/types/fixed_array.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/thread_pool.h"
#include "base/time/time.h"
#include "components/pdf/browser/pdf_document_helper.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/web_contents.h"
#include "net/base/filename_util.h"
#include "services/network/public/cpp/web_sandbox_flags.h"
#include "ui/base/page_transition_types.h"

namespace ai_chat {

namespace {

constexpr base::TimeDelta kExtractionTimeout = base::Seconds(30);

std::optional<base::FilePath> WritePdfToTempFile(
    std::vector<uint8_t> pdf_bytes) {
  base::FilePath temp_dir;
  if (!base::GetTempDir(&temp_dir)) {
    return std::nullopt;
  }
  base::FilePath temp_path;
  if (!base::CreateTemporaryFileInDir(temp_dir, &temp_path)) {
    return std::nullopt;
  }
  // Rename to add .pdf extension so MIME type detection works
  base::FilePath pdf_path = temp_path.AddExtension(FILE_PATH_LITERAL("pdf"));
  if (!base::Move(temp_path, pdf_path)) {
    base::DeleteFile(temp_path);
    return std::nullopt;
  }
  if (!base::WriteFile(pdf_path, pdf_bytes)) {
    base::DeleteFile(pdf_path);
    return std::nullopt;
  }
  return pdf_path;
}

void DeleteTempFile(const base::FilePath& path) {
  if (!path.empty()) {
    base::DeleteFile(path);
  }
}

}  // namespace

PdfTextExtractor::PdfTextExtractor() = default;

PdfTextExtractor::~PdfTextExtractor() {
  Cleanup();
}

void PdfTextExtractor::ExtractText(content::BrowserContext* browser_context,
                                   const base::FilePath& pdf_path,
                                   ExtractTextCallback callback) {
  DCHECK(!callback_) << "ExtractText called while extraction in progress";
  callback_ = std::move(callback);
  DVLOG(0) << "PdfTextExtractor: ExtractText from path: " << pdf_path;
  LoadPdfInWebContents(browser_context, pdf_path);
}

void PdfTextExtractor::ExtractText(content::BrowserContext* browser_context,
                                   std::vector<uint8_t> pdf_bytes,
                                   ExtractTextCallback callback) {
  DCHECK(!callback_) << "ExtractText called while extraction in progress";
  callback_ = std::move(callback);

  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock()},
      base::BindOnce(&WritePdfToTempFile, std::move(pdf_bytes)),
      base::BindOnce(&PdfTextExtractor::OnTempFileWritten,
                     weak_ptr_factory_.GetWeakPtr(), browser_context));
}

// content::WebContentsDelegate:

bool PdfTextExtractor::ShouldSuppressDialogs(content::WebContents* source) {
  return true;
}

void PdfTextExtractor::CanDownload(const GURL& url,
                                   const std::string& request_method,
                                   base::OnceCallback<void(bool)> callback) {
  std::move(callback).Run(false);
}

bool PdfTextExtractor::IsWebContentsCreationOverridden(
    content::RenderFrameHost* opener,
    content::SiteInstance* source_site_instance,
    content::mojom::WindowContainerType window_container_type,
    const GURL& opener_url,
    const std::string& frame_name,
    const GURL& target_url) {
  return true;
}

bool PdfTextExtractor::CanEnterFullscreenModeForTab(
    content::RenderFrameHost* requesting_frame) {
  return false;
}

bool PdfTextExtractor::CanDragEnter(
    content::WebContents* source,
    const content::DropData& data,
    blink::DragOperationsMask operations_allowed) {
  return false;
}

// content::WebContentsObserver:

void PdfTextExtractor::DidFinishLoad(
    content::RenderFrameHost* render_frame_host,
    const GURL& validated_url) {
  DVLOG(0) << "PdfTextExtractor: DidFinishLoad url=" << validated_url;
  TryRegisterForDocumentLoad();
}

void PdfTextExtractor::PrimaryMainFrameRenderProcessGone(
    base::TerminationStatus status) {
  DVLOG(1) << "PdfTextExtractor: Renderer process gone";
  Finish(std::nullopt);
}

void PdfTextExtractor::OnTempFileWritten(
    content::BrowserContext* browser_context,
    std::optional<base::FilePath> temp_path) {
  if (!temp_path || !callback_) {
    Finish(std::nullopt);
    return;
  }
  temp_file_path_ = *temp_path;
  LoadPdfInWebContents(browser_context, temp_file_path_);
}

void PdfTextExtractor::LoadPdfInWebContents(
    content::BrowserContext* browser_context,
    const base::FilePath& pdf_path) {
  content::WebContents::CreateParams create_params(browser_context);
  create_params.is_never_composited = true;
  // Allow scripts (JS/WASM), origin (Mojo bridge), navigation (subframes),
  // and plugins (required for PDF viewer MimeHandlerView).
  create_params.starting_sandbox_flags =
      network::mojom::WebSandboxFlags::kAll &
      ~network::mojom::WebSandboxFlags::kScripts &
      ~network::mojom::WebSandboxFlags::kOrigin &
      ~network::mojom::WebSandboxFlags::kNavigation &
      ~network::mojom::WebSandboxFlags::kPlugins;
  web_contents_ = content::WebContents::Create(create_params);
  web_contents_->SetOwnerLocationForDebug(FROM_HERE);
  web_contents_->SetDelegate(this);
  Observe(web_contents_.get());

  timeout_timer_.Start(FROM_HERE, kExtractionTimeout,
                       base::BindOnce(&PdfTextExtractor::OnTimeout,
                                      weak_ptr_factory_.GetWeakPtr()));

  GURL file_url = net::FilePathToFileURL(pdf_path);
  DVLOG(0) << "PdfTextExtractor: Loading file_url=" << file_url;
  web_contents_->GetController().LoadURL(file_url, content::Referrer(),
                                         ui::PAGE_TRANSITION_AUTO_TOPLEVEL,
                                         std::string());
}

void PdfTextExtractor::TryRegisterForDocumentLoad() {
  if (registered_for_load_ || !web_contents_) {
    return;
  }
  auto* pdf_helper =
      pdf::PDFDocumentHelper::MaybeGetForWebContents(web_contents_.get());
  if (!pdf_helper) {
    return;
  }
  registered_for_load_ = true;
  pdf_helper->RegisterForDocumentLoadComplete(
      base::BindOnce(&PdfTextExtractor::OnDocumentLoadComplete,
                     weak_ptr_factory_.GetWeakPtr()));
}

void PdfTextExtractor::OnDocumentLoadComplete() {
  auto* pdf_helper =
      pdf::PDFDocumentHelper::MaybeGetForWebContents(web_contents_.get());
  if (!pdf_helper) {
    DVLOG(1) << "PdfTextExtractor: No PDFDocumentHelper found";
    Finish(std::nullopt);
    return;
  }

  pdf_helper->GetPdfBytes(
      /*size_limit=*/0,
      base::BindOnce(
          [](base::WeakPtr<PdfTextExtractor> self,
             pdf::mojom::PdfListener::GetPdfBytesStatus status,
             const std::vector<uint8_t>& bytes, uint32_t page_count) {
            if (!self) {
              return;
            }
            if (status == pdf::mojom::PdfListener::GetPdfBytesStatus::kFailed ||
                page_count == 0) {
              self->Finish(std::nullopt);
              return;
            }
            self->OnGetPDFPageCount(page_count);
          },
          weak_ptr_factory_.GetWeakPtr()));
}

void PdfTextExtractor::OnGetPDFPageCount(uint32_t page_count) {
  auto* pdf_helper =
      pdf::PDFDocumentHelper::MaybeGetForWebContents(web_contents_.get());
  if (!pdf_helper) {
    Finish(std::nullopt);
    return;
  }

  auto barrier_callback = base::BarrierCallback<std::pair<size_t, std::string>>(
      page_count, base::BindOnce(&PdfTextExtractor::OnAllPagesTextReceived,
                                 weak_ptr_factory_.GetWeakPtr()));

  for (uint32_t i = 0; i < page_count; ++i) {
    pdf_helper->GetPageText(
        i, base::BindOnce(
               [](base::OnceCallback<void(std::pair<size_t, std::string>)>
                      barrier_callback,
                  size_t page_index, const std::u16string& page_text) {
                 std::move(barrier_callback)
                     .Run(std::make_pair(page_index,
                                         base::UTF16ToUTF8(page_text)));
               },
               barrier_callback, static_cast<size_t>(i)));
  }
}

void PdfTextExtractor::OnAllPagesTextReceived(
    std::vector<std::pair<size_t, std::string>> page_texts) {
  base::FixedArray<std::string_view> ordered_texts(page_texts.size());
  for (const auto& [index, text] : page_texts) {
    ordered_texts[index] = text;
  }
  Finish(base::JoinString(ordered_texts, "\n"));
}

void PdfTextExtractor::OnTimeout() {
  DVLOG(0) << "PdfTextExtractor: Extraction timed out";
  Finish(std::nullopt);
}

void PdfTextExtractor::Finish(std::optional<std::string> result) {
  DVLOG(0) << "PdfTextExtractor: Finish has_result="
           << result.has_value();
  timeout_timer_.Stop();
  Cleanup();
  if (callback_) {
    std::move(callback_).Run(std::move(result));
  }
}

void PdfTextExtractor::Cleanup() {
  if (web_contents_) {
    Observe(nullptr);
    web_contents_->SetDelegate(nullptr);
    web_contents_.reset();
  }
  if (!temp_file_path_.empty()) {
    base::ThreadPool::PostTask(
        FROM_HERE, {base::MayBlock()},
        base::BindOnce(&DeleteTempFile, std::move(temp_file_path_)));
    temp_file_path_.clear();
  }
}

}  // namespace ai_chat
