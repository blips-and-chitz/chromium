// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/browser/portal/portal.h"

#include <unordered_map>
#include <utility>

#include "base/feature_list.h"
#include "base/memory/ptr_util.h"
#include "content/browser/child_process_security_policy_impl.h"
#include "content/browser/frame_host/render_frame_host_impl.h"
#include "content/browser/frame_host/render_frame_host_manager.h"
#include "content/browser/frame_host/render_frame_proxy_host.h"
#include "content/browser/web_contents/web_contents_impl.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/web_contents_delegate.h"
#include "content/public/common/content_switches.h"
#include "mojo/public/cpp/bindings/strong_binding.h"
#include "services/service_manager/public/mojom/interface_provider.mojom.h"
#include "third_party/blink/public/common/features.h"

namespace content {

namespace {
using PortalTokenMap = std::
    unordered_map<base::UnguessableToken, Portal*, base::UnguessableTokenHash>;
base::LazyInstance<PortalTokenMap>::Leaky g_portal_token_map =
    LAZY_INSTANCE_INITIALIZER;
}  // namespace

Portal::Portal(RenderFrameHostImpl* owner_render_frame_host)
    : WebContentsObserver(
          WebContents::FromRenderFrameHost(owner_render_frame_host)),
      owner_render_frame_host_(owner_render_frame_host),
      portal_token_(base::UnguessableToken::Create()) {
  auto pair = g_portal_token_map.Get().emplace(portal_token_, this);
  DCHECK(pair.second);
}

Portal::~Portal() {
  g_portal_token_map.Get().erase(portal_token_);
}

// static
bool Portal::IsEnabled() {
  return base::FeatureList::IsEnabled(blink::features::kPortals) ||
         base::CommandLine::ForCurrentProcess()->HasSwitch(
             switches::kEnableExperimentalWebPlatformFeatures);
}

// static
Portal* Portal::FromToken(const base::UnguessableToken& portal_token) {
  PortalTokenMap& portals = g_portal_token_map.Get();
  auto it = portals.find(portal_token);
  return it == portals.end() ? nullptr : it->second;
}

// static
Portal* Portal::Create(RenderFrameHostImpl* owner_render_frame_host,
                       blink::mojom::PortalAssociatedRequest request) {
  auto portal_ptr = base::WrapUnique(new Portal(owner_render_frame_host));
  Portal* portal = portal_ptr.get();
  portal->binding_ = mojo::MakeStrongAssociatedBinding(std::move(portal_ptr),
                                                       std::move(request));
  return portal;
}

// static
std::unique_ptr<Portal> Portal::CreateForTesting(
    RenderFrameHostImpl* owner_render_frame_host) {
  return base::WrapUnique(new Portal(owner_render_frame_host));
}

RenderFrameProxyHost* Portal::CreateProxyAndAttachPortal() {
  WebContentsImpl* outer_contents_impl = static_cast<WebContentsImpl*>(
      WebContents::FromRenderFrameHost(owner_render_frame_host_));

  service_manager::mojom::InterfaceProviderPtr interface_provider;
  auto interface_provider_request(mojo::MakeRequest(&interface_provider));

  blink::mojom::DocumentInterfaceBrokerPtrInfo
      document_interface_broker_content;
  blink::mojom::DocumentInterfaceBrokerPtrInfo document_interface_broker_blink;

  // Create a FrameTreeNode in the outer WebContents to host the portal, in
  // response to the creation of a portal in the renderer process.
  FrameTreeNode* outer_node = outer_contents_impl->GetFrameTree()->AddFrame(
      owner_render_frame_host_->frame_tree_node(),
      owner_render_frame_host_->GetProcess()->GetID(),
      owner_render_frame_host_->GetProcess()->GetNextRoutingID(),
      std::move(interface_provider_request),
      mojo::MakeRequest(&document_interface_broker_content),
      mojo::MakeRequest(&document_interface_broker_blink),
      blink::WebTreeScopeType::kDocument, "", "", true,
      base::UnguessableToken::Create(), blink::FramePolicy(),
      FrameOwnerProperties(), false, blink::FrameOwnerElementType::kPortal);

  bool web_contents_created = false;
  if (!portal_contents_) {
    // Create the Portal WebContents.
    WebContents::CreateParams params(outer_contents_impl->GetBrowserContext());
    portal_contents_ = WebContents::Create(params);
    web_contents_created = true;
  }
  portal_contents_impl_ = static_cast<WebContentsImpl*>(portal_contents_.get());
  portal_contents_impl_->set_portal(this);
  portal_contents_impl_->SetDelegate(this);

  outer_contents_impl->AttachInnerWebContents(std::move(portal_contents_),
                                              outer_node->current_frame_host());

  FrameTreeNode* frame_tree_node =
      portal_contents_impl_->GetMainFrame()->frame_tree_node();
  RenderFrameProxyHost* proxy_host =
      frame_tree_node->render_manager()->GetProxyToOuterDelegate();
  proxy_host->set_render_frame_proxy_created(true);
  portal_contents_impl_->ReattachToOuterWebContentsFrame();

  if (web_contents_created)
    PortalWebContentsCreated(portal_contents_impl_);

  return proxy_host;
}

void Portal::Navigate(const GURL& url) {
  NavigationController::LoadURLParams load_url_params(url);
  portal_contents_impl_->GetController().LoadURLWithParams(load_url_params);
}

void Portal::Activate(blink::TransferableMessage data,
                      base::OnceCallback<void()> callback) {
  WebContentsImpl* outer_contents = static_cast<WebContentsImpl*>(
      WebContents::FromRenderFrameHost(owner_render_frame_host_));

  if (outer_contents->portal()) {
    mojo::ReportBadMessage("Portal::Activate called on nested portal");
    return;
  }

  WebContentsDelegate* delegate = outer_contents->GetDelegate();
  bool is_loading = portal_contents_impl_->IsLoading();
  std::unique_ptr<WebContents> portal_contents =
      portal_contents_impl_->DetachFromOuterWebContents();

  static_cast<RenderWidgetHostViewBase*>(
      outer_contents->GetMainFrame()->GetView())
      ->Destroy();
  std::unique_ptr<WebContents> contents = delegate->SwapWebContents(
      outer_contents, std::move(portal_contents), true, is_loading);
  CHECK_EQ(contents.get(), outer_contents);
  portal_contents_impl_->set_portal(nullptr);
  blink::mojom::PortalAssociatedPtr portal_ptr;
  Portal* portal = Create(portal_contents_impl_->GetMainFrame(),
                          mojo::MakeRequest(&portal_ptr));
  portal_contents_impl_->GetMainFrame()->OnPortalActivated(
      portal->portal_token_, portal_ptr.PassInterface(), std::move(data));
  portal->portal_contents_ = std::move(contents);
  std::move(callback).Run();
}

void Portal::PostMessage(blink::TransferableMessage message,
                         const base::Optional<url::Origin>& target_origin) {
  portal_contents_impl_->GetMainFrame()->ForwardMessageToPortalHost(
      std::move(message), owner_render_frame_host_->GetLastCommittedOrigin(),
      target_origin);
}

void Portal::RenderFrameDeleted(RenderFrameHost* render_frame_host) {
  if (render_frame_host == owner_render_frame_host_)
    binding_->Close();  // Also deletes |this|.
}

void Portal::WebContentsDestroyed() {
  binding_->Close();  // Also deletes |this|.
}

void Portal::PortalWebContentsCreated(WebContents* portal_web_contents) {
  WebContentsImpl* outer_contents = static_cast<WebContentsImpl*>(
      WebContents::FromRenderFrameHost(owner_render_frame_host_));
  DCHECK(outer_contents->GetDelegate());
  outer_contents->GetDelegate()->PortalWebContentsCreated(portal_web_contents);
}

WebContentsImpl* Portal::GetPortalContents() {
  return portal_contents_impl_;
}

void Portal::SetBindingForTesting(
    mojo::StrongAssociatedBindingPtr<blink::mojom::Portal> binding) {
  binding_ = binding;
}

}  // namespace content
