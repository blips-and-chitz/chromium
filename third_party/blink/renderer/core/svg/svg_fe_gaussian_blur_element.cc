/*
 * Copyright (C) 2004, 2005, 2007 Nikolas Zimmermann <zimmermann@kde.org>
 * Copyright (C) 2004, 2005, 2006 Rob Buis <buis@kde.org>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public License
 * along with this library; see the file COPYING.LIB.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include "third_party/blink/renderer/core/svg/svg_fe_gaussian_blur_element.h"

#include "third_party/blink/renderer/core/svg/graphics/filters/svg_filter_builder.h"
#include "third_party/blink/renderer/core/svg_names.h"
#include "third_party/blink/renderer/platform/graphics/filters/fe_gaussian_blur.h"
#include "third_party/blink/renderer/platform/heap/heap.h"

namespace blink {

inline SVGFEGaussianBlurElement::SVGFEGaussianBlurElement(Document& document)
    : SVGFilterPrimitiveStandardAttributes(svg_names::kFEGaussianBlurTag,
                                           document),
      std_deviation_(
          SVGAnimatedNumberOptionalNumber::Create(this,
                                                  svg_names::kStdDeviationAttr,
                                                  0.0f)),
      in1_(SVGAnimatedString::Create(this, svg_names::kInAttr)) {
  AddToPropertyMap(std_deviation_);
  AddToPropertyMap(in1_);
}

void SVGFEGaussianBlurElement::Trace(blink::Visitor* visitor) {
  visitor->Trace(std_deviation_);
  visitor->Trace(in1_);
  SVGFilterPrimitiveStandardAttributes::Trace(visitor);
}

DEFINE_NODE_FACTORY(SVGFEGaussianBlurElement)

void SVGFEGaussianBlurElement::setStdDeviation(float x, float y) {
  stdDeviationX()->BaseValue()->SetValue(x);
  stdDeviationY()->BaseValue()->SetValue(y);
  Invalidate();
}

void SVGFEGaussianBlurElement::SvgAttributeChanged(
    const QualifiedName& attr_name) {
  if (attr_name == svg_names::kInAttr ||
      attr_name == svg_names::kStdDeviationAttr) {
    SVGElement::InvalidationGuard invalidation_guard(this);
    Invalidate();
    return;
  }

  SVGFilterPrimitiveStandardAttributes::SvgAttributeChanged(attr_name);
}

FilterEffect* SVGFEGaussianBlurElement::Build(SVGFilterBuilder* filter_builder,
                                              Filter* filter) {
  FilterEffect* input1 = filter_builder->GetEffectById(
      AtomicString(in1_->CurrentValue()->Value()));
  DCHECK(input1);

  // "A negative value or a value of zero disables the effect of the given
  // filter primitive (i.e., the result is the filter input image)."
  // (https://drafts.fxtf.org/filter-effects/#element-attrdef-fegaussianblur-stddeviation)
  //
  // => Clamp to non-negative.
  float std_dev_x = std::max(0.0f, stdDeviationX()->CurrentValue()->Value());
  float std_dev_y = std::max(0.0f, stdDeviationY()->CurrentValue()->Value());
  auto* effect =
      MakeGarbageCollected<FEGaussianBlur>(filter, std_dev_x, std_dev_y);
  effect->InputEffects().push_back(input1);
  return effect;
}

}  // namespace blink
