// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_MODULES_PAYMENTS_PAYMENT_REQUEST_UPDATE_EVENT_H_
#define THIRD_PARTY_BLINK_RENDERER_MODULES_PAYMENTS_PAYMENT_REQUEST_UPDATE_EVENT_H_

#include "third_party/blink/renderer/bindings/core/v8/script_promise.h"
#include "third_party/blink/renderer/core/dom/events/event.h"
#include "third_party/blink/renderer/modules/modules_export.h"
#include "third_party/blink/renderer/modules/payments/payment_request_update_event_init.h"
#include "third_party/blink/renderer/platform/bindings/script_wrappable.h"
#include "third_party/blink/renderer/platform/heap/handle.h"
#include "third_party/blink/renderer/platform/timer.h"

namespace blink {

class ExceptionState;
class ExecutionContext;
class PaymentRequestDelegate;
class ScriptState;

class MODULES_EXPORT PaymentRequestUpdateEvent : public Event,
                                                 public GarbageCollectedMixin {
  DEFINE_WRAPPERTYPEINFO();
  USING_GARBAGE_COLLECTED_MIXIN(PaymentRequestUpdateEvent);

 public:
  PaymentRequestUpdateEvent(ExecutionContext*,
                            const AtomicString& type,
                            const PaymentRequestUpdateEventInit*);
  ~PaymentRequestUpdateEvent() override;

  static PaymentRequestUpdateEvent* Create(
      ExecutionContext*,
      const AtomicString& type,
      const PaymentRequestUpdateEventInit* =
          PaymentRequestUpdateEventInit::Create());

  void SetPaymentRequest(PaymentRequestDelegate* request);

  void updateWith(ScriptState*, ScriptPromise, ExceptionState&);

  void start_waiting_for_update(bool value) { wait_for_update_ = value; }
  bool is_waiting_for_update() const { return wait_for_update_; }

  void Trace(blink::Visitor*) override;

  void OnUpdateEventTimeoutForTesting();

 private:
  // This class is declared here because it requires privileges to access some
  // PaymentRequestUpdateEvent's member fields, such as request_, abort_timer_.
  class UpdatePaymentDetailsFunction;

  void OnUpdateEventTimeout(TimerBase*);

  // True after event.updateWith() was called.
  bool wait_for_update_;

  Member<PaymentRequestDelegate> request_;
  TaskRunnerTimer<PaymentRequestUpdateEvent> abort_timer_;
};

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_MODULES_PAYMENTS_PAYMENT_REQUEST_UPDATE_EVENT_H_
