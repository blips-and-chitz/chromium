// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cc/layers/surface_layer_impl.h"

#include <stdint.h>

#include "base/stl_util.h"
#include "base/trace_event/traced_value.h"
#include "cc/debug/debug_colors.h"
#include "cc/layers/append_quads_data.h"
#include "cc/trees/layer_tree_impl.h"
#include "cc/trees/occlusion.h"
#include "components/viz/common/quads/solid_color_draw_quad.h"
#include "components/viz/common/quads/surface_draw_quad.h"

namespace cc {

SurfaceLayerImpl::SurfaceLayerImpl(
    LayerTreeImpl* tree_impl,
    int id,
    UpdateSubmissionStateCB update_submission_state_callback)
    : LayerImpl(tree_impl, id),
      update_submission_state_callback_(
          std::move(update_submission_state_callback)) {}

SurfaceLayerImpl::~SurfaceLayerImpl() {
  if (update_submission_state_callback_)
    update_submission_state_callback_.Run(false);
}

std::unique_ptr<LayerImpl> SurfaceLayerImpl::CreateLayerImpl(
    LayerTreeImpl* tree_impl) {
  return SurfaceLayerImpl::Create(tree_impl, id(),
                                  std::move(update_submission_state_callback_));
}

void SurfaceLayerImpl::SetRange(const viz::SurfaceRange& surface_range,
                                base::Optional<uint32_t> deadline_in_frames) {
  if (surface_range_ == surface_range &&
      deadline_in_frames_ == deadline_in_frames) {
    return;
  }

  if (surface_range_.end() != surface_range.end() &&
      surface_range.end().local_surface_id().is_valid()) {
    TRACE_EVENT_WITH_FLOW2(
        TRACE_DISABLED_BY_DEFAULT("viz.surface_id_flow"),
        "LocalSurfaceId.Embed.Flow",
        TRACE_ID_GLOBAL(
            surface_range.end().local_surface_id().embed_trace_id()),
        TRACE_EVENT_FLAG_FLOW_IN | TRACE_EVENT_FLAG_FLOW_OUT, "step",
        "ImplSetSurfaceId", "surface_id", surface_range.end().ToString());
  }

  if (surface_range.start() &&
      surface_range_.start() != surface_range.start() &&
      surface_range.start()->local_surface_id().is_valid()) {
    TRACE_EVENT_WITH_FLOW2(
        TRACE_DISABLED_BY_DEFAULT("viz.surface_id_flow"),
        "LocalSurfaceId.Submission.Flow",
        TRACE_ID_GLOBAL(
            surface_range.start()->local_surface_id().submission_trace_id()),
        TRACE_EVENT_FLAG_FLOW_IN | TRACE_EVENT_FLAG_FLOW_OUT, "step",
        "ImplSetOldestAcceptableFallback", "surface_id",
        surface_range.start()->ToString());
  }

  surface_range_ = surface_range;
  deadline_in_frames_ = deadline_in_frames;
  NoteLayerPropertyChanged();
}

void SurfaceLayerImpl::SetStretchContentToFillBounds(bool stretch_content) {
  if (stretch_content_to_fill_bounds_ == stretch_content)
    return;

  stretch_content_to_fill_bounds_ = stretch_content;
  NoteLayerPropertyChanged();
}

void SurfaceLayerImpl::SetSurfaceHitTestable(bool surface_hit_testable) {
  if (surface_hit_testable_ == surface_hit_testable)
    return;

  surface_hit_testable_ = surface_hit_testable;
  NoteLayerPropertyChanged();
}

void SurfaceLayerImpl::SetHasPointerEventsNone(bool has_pointer_events_none) {
  if (has_pointer_events_none_ == has_pointer_events_none)
    return;

  has_pointer_events_none_ = has_pointer_events_none;
  NoteLayerPropertyChanged();
}

void SurfaceLayerImpl::PushPropertiesTo(LayerImpl* layer) {
  LayerImpl::PushPropertiesTo(layer);
  SurfaceLayerImpl* layer_impl = static_cast<SurfaceLayerImpl*>(layer);
  layer_impl->SetRange(surface_range_, std::move(deadline_in_frames_));
  // Unless the client explicitly specifies otherwise, don't block on
  // |surface_range_| more than once.
  deadline_in_frames_ = 0u;
  layer_impl->SetStretchContentToFillBounds(stretch_content_to_fill_bounds_);
  layer_impl->SetSurfaceHitTestable(surface_hit_testable_);
  layer_impl->SetHasPointerEventsNone(has_pointer_events_none_);
}

bool SurfaceLayerImpl::WillDraw(
    DrawMode draw_mode,
    viz::ClientResourceProvider* resource_provider) {
  bool will_draw = LayerImpl::WillDraw(draw_mode, resource_provider);
  // If we have a change in WillDraw (meaning that visibility has changed), we
  // want to inform the VideoFrameSubmitter to start or stop submitting
  // compositor frames.
  if (will_draw_ != will_draw) {
    will_draw_ = will_draw;
    if (update_submission_state_callback_)
      update_submission_state_callback_.Run(will_draw);
  }

  return surface_range_.IsValid() && will_draw;
}

void SurfaceLayerImpl::AppendQuads(viz::RenderPass* render_pass,
                                   AppendQuadsData* append_quads_data) {
  AppendRainbowDebugBorder(render_pass);
  if (!surface_range_.IsValid())
    return;

  auto* primary = CreateSurfaceDrawQuad(render_pass, surface_range_);
  if (primary) {
    // Add the primary surface ID as a dependency.
    append_quads_data->activation_dependencies.push_back(surface_range_.end());
    if (deadline_in_frames_) {
      if (!append_quads_data->deadline_in_frames)
        append_quads_data->deadline_in_frames = 0u;
      append_quads_data->deadline_in_frames = std::max(
          *append_quads_data->deadline_in_frames, *deadline_in_frames_);
    } else {
      append_quads_data->use_default_lower_bound_deadline = true;
    }
  }
  // Unless the client explicitly specifies otherwise, don't block on
  // |surface_range_| more than once.
  deadline_in_frames_ = 0u;
}

bool SurfaceLayerImpl::is_surface_layer() const {
  return true;
}

gfx::Rect SurfaceLayerImpl::GetEnclosingRectInTargetSpace() const {
  return GetScaledEnclosingRectInTargetSpace(
      layer_tree_impl()->device_scale_factor());
}

viz::SurfaceDrawQuad* SurfaceLayerImpl::CreateSurfaceDrawQuad(
    viz::RenderPass* render_pass,
    const viz::SurfaceRange& surface_range) {
  DCHECK(surface_range.end().is_valid());

  float device_scale_factor = layer_tree_impl()->device_scale_factor();

  gfx::Rect quad_rect(gfx::ScaleToEnclosingRect(
      gfx::Rect(bounds()), device_scale_factor, device_scale_factor));
  gfx::Rect visible_quad_rect =
      draw_properties().occlusion_in_content_space.GetUnoccludedContentRect(
          gfx::Rect(bounds()));

  visible_quad_rect = gfx::ScaleToEnclosingRect(
      visible_quad_rect, device_scale_factor, device_scale_factor);
  visible_quad_rect = gfx::IntersectRects(quad_rect, visible_quad_rect);

  if (visible_quad_rect.IsEmpty())
    return nullptr;

  viz::SharedQuadState* shared_quad_state =
      render_pass->CreateAndAppendSharedQuadState();

  PopulateScaledSharedQuadState(shared_quad_state, device_scale_factor,
                                device_scale_factor, contents_opaque());

  auto* surface_draw_quad =
      render_pass->CreateAndAppendDrawQuad<viz::SurfaceDrawQuad>();
  surface_draw_quad->SetNew(shared_quad_state, quad_rect, visible_quad_rect,
                            surface_range, background_color(),
                            stretch_content_to_fill_bounds_,
                            has_pointer_events_none_);

  return surface_draw_quad;
}

void SurfaceLayerImpl::GetDebugBorderProperties(SkColor* color,
                                                float* width) const {
  *color = DebugColors::SurfaceLayerBorderColor();
  *width = DebugColors::SurfaceLayerBorderWidth(
      layer_tree_impl() ? layer_tree_impl()->device_scale_factor() : 1);
}

void SurfaceLayerImpl::AppendRainbowDebugBorder(viz::RenderPass* render_pass) {
  if (!ShowDebugBorders(DebugBorderType::SURFACE))
    return;

  viz::SharedQuadState* shared_quad_state =
      render_pass->CreateAndAppendSharedQuadState();
  PopulateSharedQuadState(shared_quad_state, contents_opaque());

  SkColor color;
  float border_width;
  GetDebugBorderProperties(&color, &border_width);

  SkColor colors[] = {
      0x80ff0000,  // Red.
      0x80ffa500,  // Orange.
      0x80ffff00,  // Yellow.
      0x80008000,  // Green.
      0x800000ff,  // Blue.
      0x80ee82ee,  // Violet.
  };
  const int kNumColors = base::size(colors);

  const int kStripeWidth = 300;
  const int kStripeHeight = 300;

  for (int i = 0;; ++i) {
    // For horizontal lines.
    int x = kStripeWidth * i;
    int width = std::min(kStripeWidth, bounds().width() - x - 1);

    // For vertical lines.
    int y = kStripeHeight * i;
    int height = std::min(kStripeHeight, bounds().height() - y - 1);

    gfx::Rect top(x, 0, width, border_width);
    gfx::Rect bottom(x, bounds().height() - border_width, width, border_width);
    gfx::Rect left(0, y, border_width, height);
    gfx::Rect right(bounds().width() - border_width, y, border_width, height);

    if (top.IsEmpty() && left.IsEmpty())
      break;

    if (!top.IsEmpty()) {
      bool force_anti_aliasing_off = false;
      auto* top_quad =
          render_pass->CreateAndAppendDrawQuad<viz::SolidColorDrawQuad>();
      top_quad->SetNew(shared_quad_state, top, top, colors[i % kNumColors],
                       force_anti_aliasing_off);

      auto* bottom_quad =
          render_pass->CreateAndAppendDrawQuad<viz::SolidColorDrawQuad>();
      bottom_quad->SetNew(shared_quad_state, bottom, bottom,
                          colors[kNumColors - 1 - (i % kNumColors)],
                          force_anti_aliasing_off);

      if (contents_opaque()) {
        // Draws a stripe filling the layer vertically with the same color and
        // width as the horizontal stipes along the layer's top border.
        auto* solid_quad =
            render_pass->CreateAndAppendDrawQuad<viz::SolidColorDrawQuad>();
        // The inner fill is more transparent then the border.
        static const float kFillOpacity = 0.1f;
        SkColor fill_color = SkColorSetA(
            colors[i % kNumColors],
            static_cast<uint8_t>(SkColorGetA(colors[i % kNumColors]) *
                                 kFillOpacity));
        gfx::Rect fill_rect(x, 0, width, bounds().height());
        solid_quad->SetNew(shared_quad_state, fill_rect, fill_rect, fill_color,
                           force_anti_aliasing_off);
      }
    }
    if (!left.IsEmpty()) {
      bool force_anti_aliasing_off = false;
      auto* left_quad =
          render_pass->CreateAndAppendDrawQuad<viz::SolidColorDrawQuad>();
      left_quad->SetNew(shared_quad_state, left, left,
                        colors[kNumColors - 1 - (i % kNumColors)],
                        force_anti_aliasing_off);

      auto* right_quad =
          render_pass->CreateAndAppendDrawQuad<viz::SolidColorDrawQuad>();
      right_quad->SetNew(shared_quad_state, right, right,
                         colors[i % kNumColors], force_anti_aliasing_off);
    }
  }
}

void SurfaceLayerImpl::AsValueInto(base::trace_event::TracedValue* dict) const {
  LayerImpl::AsValueInto(dict);
  dict->SetString("surface_range", surface_range_.ToString());
}

const char* SurfaceLayerImpl::LayerTypeAsString() const {
  return "cc::SurfaceLayerImpl";
}

}  // namespace cc
