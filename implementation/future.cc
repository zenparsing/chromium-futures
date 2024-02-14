// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/futures/future.h"

namespace base {

Future<void> MakeReadyFuture() {
  Promise<void> promise;
  promise.SetValue();
  return promise.GetFuture();
}

}  // namespace base
