// Copyright (c) 2023 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

#include "brave/components/ai_chat/renderer/ai_chat_render_frame_observer.h"

#include <string>
#include <utility>

#include "base/containers/contains.h"
#include "base/containers/fixed_flat_set.h"
#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/strings/string_piece_forward.h"
#include "base/values.h"
#include "brave/components/ai_chat/page_content_extractor.mojom-shared.h"
#include "brave/components/ai_chat/page_content_extractor.mojom.h"
#include "brave/components/ai_chat/renderer/page_text_distilling.h"
#include "content/public/renderer/render_frame.h"
#include "content/public/renderer/render_frame_observer.h"
#include "net/base/url_util.h"
#include "third_party/abseil-cpp/absl/types/optional.h"
#include "third_party/blink/public/platform/web_string.h"
#include "third_party/blink/public/web/web_local_frame.h"
#include "third_party/blink/public/web/web_script_source.h"
#include "url/gurl.h"
#include "url/url_constants.h"
#include "v8/include/v8-isolate.h"

namespace {

const char16_t kYoutubeTranscriptUrlExtractionScript[] =
    uR"JS(
      (function() {
        return ytplayer?.config?.args?.raw_player_response?.captions?.playerCaptionsTracklistRenderer?.captionTracks?.[0]?.baseUrl
      })()
    )JS";

const char16_t kVideoTrackTranscriptUrlExtractionScript[] =
    // TODO(petemill): Make more informed srclang choice.
    uR"JS(
      (function() {
        const nodes = document.querySelectorAll('video track')
        if (nodes.length) {
          let selectedNode = nodes[0]
          for (const node of nodes) {
            if (node.srclang.toLowerCase() === 'en') {
              selectedNode = node
            }
          }
          return node.src
        }
      })()
    )JS";

constexpr auto kYouTubeHosts = base::MakeFixedFlatSet<base::StringPiece>(
    {"www.youtube.com", "m.youtube.com"});

// TODO(petemill): Use heuristics to determine if page's main focus is
// a video, and not a hard-coded list of Url hosts.
constexpr auto kVideoTrackHosts =
    base::MakeFixedFlatSet<base::StringPiece>({"www.ted.com"});

}  // namespace

namespace ai_chat {

AIRenderFrameObserver::AIRenderFrameObserver(
    content::RenderFrame* render_frame,
    service_manager::BinderRegistry* registry,
    int32_t global_world_id,
    int32_t isolated_world_id)
    : content::RenderFrameObserver(render_frame),
      global_world_id_(global_world_id),
      isolated_world_id_(isolated_world_id),
      weak_ptr_factory_(this) {
  if (!render_frame->IsMainFrame()) {
    return;
  }
  // Bind mojom API to allow browser to communicate with this class
  // Being a RenderFrameObserver, this object is scoped to the RenderFrame.
  // Unretained is safe here because `registry` is also scoped to the
  // RenderFrame.
  registry->AddInterface(base::BindRepeating(
      &AIRenderFrameObserver::BindReceiver, base::Unretained(this)));
}

AIRenderFrameObserver::~AIRenderFrameObserver() = default;

void AIRenderFrameObserver::OnDestruct() {
  delete this;
}

void AIRenderFrameObserver::ExtractPageContent(
    mojom::PageContentExtractor::ExtractPageContentCallback callback) {
  VLOG(1) << "AI Chat renderer has been asked for page content.";
  blink::WebLocalFrame* main_frame = render_frame()->GetWebFrame();
  GURL origin =
      url::Origin(((const blink::WebFrame*)main_frame)->GetSecurityOrigin())
          .GetURL();

  // Decide which technique to use to extract content from page...
  // 1) Video - YouTube's custom link to transcript
  // 2) Video - <track> element specifying text location
  // 3) Text - find the "main" text of the page

  if (origin.is_valid()) {
    std::string host = origin.host();
    if (base::Contains(kYouTubeHosts, host)) {
      VLOG(1) << "YouTube transcript type";
      // Do Youtube extraction
      v8::HandleScope handle_scope(v8::Isolate::GetCurrent());
      blink::WebScriptSource source = blink::WebScriptSource(
          blink::WebString::FromUTF16(kYoutubeTranscriptUrlExtractionScript));
      auto script_callback = base::BindOnce(
          &AIRenderFrameObserver::OnJSTranscriptUrlResult,
          weak_ptr_factory_.GetWeakPtr(), std::move(callback),
          ai_chat::mojom::PageContentType::VideoTranscriptYouTube);
      // Main world so that we can access a global variable
      main_frame->RequestExecuteScript(
          global_world_id_, base::make_span(&source, 1u),
          blink::mojom::UserActivationOption::kDoNotActivate,
          blink::mojom::EvaluationTiming::kAsynchronous,
          blink::mojom::LoadEventBlockingOption::kDoNotBlock,
          std::move(script_callback), blink::BackForwardCacheAware::kAllow,
          blink::mojom::WantResultOption::kWantResult,
          blink::mojom::PromiseResultOption::kAwait);
      return;
    } else if (base::Contains(kVideoTrackHosts, host)) {
      VLOG(1) << "Video track transcript type";
      // Video <track> extraction
      // TODO(petemill): Use heuristics to determine if page's main focus is
      // a video, and not a hard-coded list of Url hosts.
      v8::HandleScope handle_scope(v8::Isolate::GetCurrent());
      blink::WebScriptSource source =
          blink::WebScriptSource(blink::WebString::FromUTF16(
              kVideoTrackTranscriptUrlExtractionScript));
      auto script_callback = base::BindOnce(
          &AIRenderFrameObserver::OnJSTranscriptUrlResult,
          weak_ptr_factory_.GetWeakPtr(), std::move(callback),
          ai_chat::mojom::PageContentType::VideoTranscriptYouTube);
      // Main world so that we can access a global variable
      main_frame->RequestExecuteScript(
          isolated_world_id_, base::make_span(&source, 1u),
          blink::mojom::UserActivationOption::kDoNotActivate,
          blink::mojom::EvaluationTiming::kAsynchronous,
          blink::mojom::LoadEventBlockingOption::kDoNotBlock,
          std::move(script_callback), blink::BackForwardCacheAware::kAllow,
          blink::mojom::WantResultOption::kWantResult,
          blink::mojom::PromiseResultOption::kAwait);
      return;
    }
  }
  VLOG(1) << "Text transcript type";
  // Do text extraction
  DistillPageText(
      render_frame(),
      base::BindOnce(&AIRenderFrameObserver::OnDistillResult,
                     weak_ptr_factory_.GetWeakPtr(), std::move(callback)));
}

void AIRenderFrameObserver::BindReceiver(
    mojo::PendingReceiver<mojom::PageContentExtractor> receiver) {
  VLOG(1) << "AIChat AIRenderFrameObserver handler bound.";
  receiver_.reset();
  receiver_.Bind(std::move(receiver));
}

void AIRenderFrameObserver::OnDistillResult(
    mojom::PageContentExtractor::ExtractPageContentCallback callback,
    const absl::optional<std::string>& content) {
  // Validate
  if (!content.has_value()) {
    VLOG(1) << "null content";
    std::move(callback).Run({});
    return;
  }
  if (content->empty()) {
    VLOG(1) << "Empty string";
    std::move(callback).Run({});
    return;
  }
  VLOG(1) << "Got a distill result of character length: " << content->length();
  // Successful text extraction
  auto result = mojom::PageContent::New();
  result->type = std::move(mojom::PageContentType::Text);
  result->content = mojom::PageContentData::NewContent(content.value());
  std::move(callback).Run(std::move(result));
}

void AIRenderFrameObserver::OnJSTranscriptUrlResult(
    ai_chat::mojom::PageContentExtractor::ExtractPageContentCallback callback,
    ai_chat::mojom::PageContentType type,
    absl::optional<base::Value> value,
    base::TimeTicks start_time) {
  DVLOG(2) << "Video transcript Url extraction script completed and took"
           << (base::TimeTicks::Now() - start_time).InMillisecondsF() << "ms"
           << "\nResult: " << (value ? value->DebugString() : "[undefined]");
  // Handle no result from script
  if (!value.has_value() || !value->is_string()) {
    std::move(callback).Run({});
    return;
  }
  // Handle invalid url
  GURL transcript_url =
      render_frame()->GetWebFrame()->GetDocument().CompleteURL(
          blink::WebString::FromASCII(value->GetString()));
  if (!transcript_url.is_valid() ||
      !transcript_url.SchemeIs(url::kHttpsScheme)) {
    DVLOG(1) << "Invalid Url for transcript: " << transcript_url.spec();
    std::move(callback).Run({});
    return;
  }
  // Handle success. Url will be fetched in (browser process) caller.
  auto result = ai_chat::mojom::PageContent::New();
  result->content = mojom::PageContentData::NewContentUrl(transcript_url);
  result->type = std::move(type);
  std::move(callback).Run(std::move(result));
}

}  // namespace ai_chat
