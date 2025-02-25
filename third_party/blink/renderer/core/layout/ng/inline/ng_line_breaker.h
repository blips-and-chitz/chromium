// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef NGLineBreaker_h
#define NGLineBreaker_h

#include "base/optional.h"
#include "third_party/blink/renderer/core/core_export.h"
#include "third_party/blink/renderer/core/layout/ng/exclusions/ng_line_layout_opportunity.h"
#include "third_party/blink/renderer/core/layout/ng/inline/ng_inline_item_result.h"
#include "third_party/blink/renderer/core/layout/ng/inline/ng_inline_node.h"
#include "third_party/blink/renderer/platform/fonts/shaping/harfbuzz_shaper.h"
#include "third_party/blink/renderer/platform/fonts/shaping/shape_result_spacing.h"
#include "third_party/blink/renderer/platform/text/text_break_iterator.h"
#include "third_party/blink/renderer/platform/wtf/allocator.h"

namespace blink {

class Hyphenation;
class NGInlineBreakToken;
class NGInlineItem;

// The line breaker needs to know which mode its in to properly handle floats.
enum class NGLineBreakerMode { kContent, kMinContent, kMaxContent };

// Represents a line breaker.
//
// This class measures each NGInlineItem and determines items to form a line,
// so that NGInlineLayoutAlgorithm can build a line box from the output.
class CORE_EXPORT NGLineBreaker {
  STACK_ALLOCATED();

 public:
  NGLineBreaker(NGInlineNode,
                NGLineBreakerMode,
                const NGConstraintSpace&,
                const NGLineLayoutOpportunity&,
                const NGPositionedFloatVector& leading_floats,
                unsigned handled_leading_floats_index,
                const NGInlineBreakToken*,
                NGExclusionSpace*);
  ~NGLineBreaker();

  // Compute the next line break point and produces NGInlineItemResults for
  // the line.
  inline void NextLine(NGLineInfo* line_info) {
    NextLine(NGSizeIndefinite, nullptr, line_info);
  }

  // During the min/max size calculation we need a special percentage
  // resolution block-size to pass to children/pass to children.
  // TODO(layout-dev): Split into two methods (NextLine/NextLineForMinMax) or,
  // better yet, subclass or templetize the line-breaker for Min/Max computation
  // if we can do that without incurring a performance penalty
  void NextLine(LayoutUnit percentage_resolution_block_size_for_min_max,
                Vector<LayoutObject*>* out_floats_for_min_max,
                NGLineInfo*);

  bool IsFinished() const { return item_index_ >= Items().size(); }

  // Create an NGInlineBreakToken for the last line returned by NextLine().
  scoped_refptr<NGInlineBreakToken> CreateBreakToken(const NGLineInfo&) const;

  // Computing |NGLineBreakerMode::kMinContent| with |MaxSizeCache| caches
  // information that can help computing |kMaxContent|. It is recommended to set
  // this when computing both |kMinContent| and |kMaxContent|.
  using MaxSizeCache = Vector<LayoutUnit, 64>;
  void SetMaxSizeCache(MaxSizeCache* max_size_cache);

  // Compute NGInlineItemResult for an open tag item.
  // Returns true if this item has edge and may have non-zero inline size.
  static bool ComputeOpenTagResult(const NGInlineItem&,
                                   const NGConstraintSpace&,
                                   NGInlineItemResult*);

  // This enum is private, except for |WhitespaceStateForTesting()|. See
  // |whitespace_| member.
  enum class WhitespaceState {
    kLeading,
    kNone,
    kUnknown,
    kCollapsible,
    kCollapsed,
    kPreserved,
  };
  WhitespaceState TrailingWhitespaceForTesting() const {
    return trailing_whitespace_;
  }

 private:
  const String& Text() const { return items_data_.text_content; }
  const Vector<NGInlineItem>& Items() const { return items_data_.items; }

  NGInlineItemResult* AddItem(const NGInlineItem&,
                              unsigned end_offset,
                              NGLineInfo*);
  NGInlineItemResult* AddItem(const NGInlineItem&, NGLineInfo*);
  void SetLineEndFragment(scoped_refptr<const NGPhysicalTextFragment>,
                          NGLineInfo*);

  void BreakLine(LayoutUnit percentage_resolution_block_size_for_min_max,
                 Vector<LayoutObject*>* out_floats_for_min_max,
                 NGLineInfo*);
  void PrepareNextLine(NGLineInfo*);

  void ComputeLineLocation(NGLineInfo*) const;

  enum class LineBreakState {
    // The line breaking is complete.
    kDone,

    // Should complete the line at the earliest possible point.
    // Trailing spaces, <br>, or close tags should be included to the line even
    // when it is overflowing.
    kTrailing,

    // Looking for more items to fit into the current line.
    kContinue,
  };

  inline void HandleText(const NGInlineItem& item, NGLineInfo* line_info) {
    DCHECK(item.TextShapeResult());
    HandleText(item, *item.TextShapeResult(), line_info);
  }
  void HandleText(const NGInlineItem& item, const ShapeResult&, NGLineInfo*);
  void BreakText(NGInlineItemResult*,
                 const NGInlineItem&,
                 const ShapeResult&,
                 LayoutUnit available_width,
                 NGLineInfo*);
  bool HandleTextForFastMinContent(NGInlineItemResult*,
                                   const NGInlineItem&,
                                   const ShapeResult&,
                                   NGLineInfo*);

  scoped_refptr<ShapeResultView> TruncateLineEndResult(
      const NGLineInfo&,
      const NGInlineItemResult&,
      unsigned end_offset);
  void UpdateShapeResult(const NGLineInfo&, NGInlineItemResult*);
  scoped_refptr<ShapeResult> ShapeText(const NGInlineItem&,
                                       unsigned start,
                                       unsigned end);

  void HandleTrailingSpaces(const NGInlineItem&,
                            const ShapeResult&,
                            NGLineInfo*);
  void RemoveTrailingCollapsibleSpace(NGLineInfo*);
  LayoutUnit TrailingCollapsibleSpaceWidth(NGLineInfo*);
  void ComputeTrailingCollapsibleSpace(NGLineInfo*);

  void HandleControlItem(const NGInlineItem&, NGLineInfo*);
  void HandleBidiControlItem(const NGInlineItem&, NGLineInfo*);
  void HandleAtomicInline(
      const NGInlineItem&,
      LayoutUnit percentage_resolution_block_size_for_min_max,
      NGLineInfo*);
  void HandleFloat(const NGInlineItem&,
                   Vector<LayoutObject*>* out_floats_for_min_max,
                   NGLineInfo*);

  void HandleOpenTag(const NGInlineItem&, NGLineInfo*);
  void HandleCloseTag(const NGInlineItem&, NGLineInfo*);

  void HandleOverflow(NGLineInfo*);
  void Rewind(unsigned new_end, NGLineInfo*);

  const ComputedStyle& ComputeCurrentStyle(unsigned item_result_index,
                                           NGLineInfo*) const;
  void SetCurrentStyle(const ComputedStyle&);

  void MoveToNextOf(const NGInlineItem&);
  void MoveToNextOf(const NGInlineItemResult&);

  void ComputeBaseDirection();

  LayoutUnit AvailableWidth() const {
    return line_opportunity_.AvailableInlineSize();
  }
  LayoutUnit AvailableWidthToFit() const {
    return AvailableWidth().AddEpsilon();
  }

  // Represents the current offset of the input.
  LineBreakState state_;
  unsigned item_index_ = 0;
  unsigned offset_ = 0;

  // |WhitespaceState| of the current end. When a line is broken, this indicates
  // the state of trailing whitespaces.
  WhitespaceState trailing_whitespace_;

  // The current position from inline_start. Unlike NGInlineLayoutAlgorithm
  // that computes position in visual order, this position in logical order.
  LayoutUnit position_;
  NGLineLayoutOpportunity line_opportunity_;

  NGInlineNode node_;

  // True if this line is the "first formatted line".
  // https://www.w3.org/TR/CSS22/selector.html#first-formatted-line
  bool is_first_formatted_line_ = false;

  bool use_first_line_style_ = false;

  // True when current box allows line wrapping.
  bool auto_wrap_ = false;

  // True when current box has 'word-break/word-wrap: break-word'.
  bool break_anywhere_if_overflow_ = false;

  // Force LineBreakType::kBreakCharacter by ignoring the current style if
  // |break_anywhere_if_overflow_| is set. Set to find grapheme cluster
  // boundaries for 'break-word' after overflow.
  bool override_break_anywhere_ = false;

  // True when breaking at soft hyphens (U+00AD) is allowed.
  bool enable_soft_hyphen_ = true;

  // True in quirks mode or limited-quirks mode, which require line-height
  // quirks.
  // https://quirks.spec.whatwg.org/#the-line-height-calculation-quirk
  bool in_line_height_quirks_mode_ = false;

  // True when the line we are breaking has a list marker.
  bool has_list_marker_ = false;

  // Set when the line ended with a forced break. Used to setup the states for
  // the next line.
  bool is_after_forced_break_ = false;

  bool ignore_floats_ = false;

  // Set in quirks mode when we're not supposed to break inside table cells
  // between images, and between text and images.
  bool sticky_images_quirk_ = false;

  const NGInlineItemsData& items_data_;

  NGLineBreakerMode mode_;
  const NGConstraintSpace& constraint_space_;
  NGExclusionSpace* exclusion_space_;
  scoped_refptr<const NGInlineBreakToken> break_token_;
  scoped_refptr<const ComputedStyle> current_style_;

  LazyLineBreakIterator break_iterator_;
  HarfBuzzShaper shaper_;
  ShapeResultSpacing<String> spacing_;
  bool previous_line_had_forced_break_ = false;
  const Hyphenation* hyphenation_ = nullptr;

  // Cache the result of |ComputeTrailingCollapsibleSpace| to avoid shaping
  // multiple times.
  struct TrailingCollapsibleSpace {
    NGInlineItemResult* item_result;
    scoped_refptr<const ShapeResultView> collapsed_shape_result;
  };
  base::Optional<TrailingCollapsibleSpace> trailing_collapsible_space_;

  // Keep track of handled float items. See HandleFloat().
  const NGPositionedFloatVector& leading_floats_;
  unsigned leading_floats_index_ = 0u;
  unsigned handled_leading_floats_index_;

  // Cache for computing |MinMaxSize|. See |MaxSizeCache|.
  MaxSizeCache* max_size_cache_ = nullptr;

  // Keep the last item |HandleTextForFastMinContent()| has handled. This is
  // used to fallback the last word to |HandleText()|.
  const NGInlineItem* fast_min_content_item_ = nullptr;

  // The current base direction for the bidi algorithm.
  // This is copied from NGInlineNode, then updated after each forced line break
  // if 'unicode-bidi: plaintext'.
  TextDirection base_direction_;
};

}  // namespace blink

#endif  // NGLineBreaker_h
