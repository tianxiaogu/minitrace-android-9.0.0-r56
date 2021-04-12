/*
 * Copyright (C) 2011 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef ART_RUNTIME_MINI_TRACE_H_
#define ART_RUNTIME_MINI_TRACE_H_

#include <memory>
#include <ostream>
#include <set>
#include <string>
#include <vector>
#include <queue>
#include <unordered_map>

#include "base/atomic.h"
#include "base/macros.h"
#include "base/os.h"
#include "base/safe_map.h"
#include "base/stringpiece.h"
#include "dex/dex_instruction.h"
#include "globals.h"
#include "instrumentation.h"
#include "class_linker.h"
#include "trace.h"

namespace art {

namespace mirror {
class Class;
}  // namespace mirror

class Thread;
class ArtField;
class ArtMethod;

class MiniTrace : public instrumentation::InstrumentationListener {
 public:
  static void Start()
      REQUIRES(!Locks::mutator_lock_, !Locks::thread_list_lock_, !Locks::thread_suspend_count_lock_,
               !Locks::trace_lock_);
  static void Stop()
      REQUIRES(!Locks::mutator_lock_, !Locks::thread_list_lock_, !Locks::trace_lock_);
  static void Shutdown() REQUIRES(!Locks::trace_lock_);

  static void PostClassPrepare(mirror::Class* klass)
      REQUIRES_SHARED(Locks::mutator_lock_);

  // InstrumentationListener implementation.
  virtual void MethodEntered(Thread* thread,
                             Handle<mirror::Object> this_object,
                             ArtMethod* method,
                             uint32_t dex_pc) REQUIRES_SHARED(Locks::mutator_lock_) OVERRIDE;

  virtual void MethodExited(Thread* thread,
                            Handle<mirror::Object> this_object,
                            ArtMethod* method,
                            uint32_t dex_pc,
                            Handle<mirror::Object> return_value)
      REQUIRES_SHARED(Locks::mutator_lock_) OVERRIDE;

  virtual void MethodExited(Thread* thread,
                            Handle<mirror::Object> this_object,
                            ArtMethod* method,
                            uint32_t dex_pc,
                            const JValue& return_value)
      REQUIRES_SHARED(Locks::mutator_lock_) OVERRIDE;

  virtual void MethodUnwind(Thread* thread,
                            Handle<mirror::Object> this_object,
                            ArtMethod* method,
                            uint32_t dex_pc)
      REQUIRES_SHARED(Locks::mutator_lock_) OVERRIDE;

  virtual void DexPcMoved(Thread* thread,
                          Handle<mirror::Object> this_object,
                          ArtMethod* method,
                          uint32_t new_dex_pc)
      REQUIRES_SHARED(Locks::mutator_lock_) OVERRIDE;

  virtual void FieldRead(Thread* thread,
                         Handle<mirror::Object> this_object,
                         ArtMethod* method,
                         uint32_t dex_pc,
                         ArtField* field)
      REQUIRES_SHARED(Locks::mutator_lock_) OVERRIDE;

  virtual void FieldWritten(Thread* thread,
                            Handle<mirror::Object> this_object,
                            ArtMethod* method,
                            uint32_t dex_pc,
                            ArtField* field,
                            const JValue& field_value)
      REQUIRES_SHARED(Locks::mutator_lock_) OVERRIDE;

  virtual void ExceptionThrown(Thread* thread,
                               Handle<mirror::Throwable> exception_object)
      REQUIRES_SHARED(Locks::mutator_lock_) OVERRIDE;

  virtual void ExceptionHandled(Thread* thread, Handle<mirror::Throwable> exception_object)
      REQUIRES_SHARED(Locks::mutator_lock_) OVERRIDE;

  virtual void Branch(Thread* thread,
                      ArtMethod* method,
                      uint32_t dex_pc,
                      int32_t dex_pc_offset)
      REQUIRES_SHARED(Locks::mutator_lock_) OVERRIDE;

  virtual void InvokeVirtualOrInterface(Thread* thread,
                                        Handle<mirror::Object> this_object,
                                        ArtMethod* caller,
                                        uint32_t dex_pc,
                                        ArtMethod* callee)
      REQUIRES_SHARED(Locks::mutator_lock_) OVERRIDE;

  virtual void WatchedFramePop(Thread* thread ATTRIBUTE_UNUSED,
                               const ShadowFrame& frame ATTRIBUTE_UNUSED)
      REQUIRES_SHARED(Locks::mutator_lock_) OVERRIDE;

  static void DumpCoverageData(std::ostream& os, ArtMethod* method) REQUIRES_SHARED(Locks::mutator_lock_);
  static void DumpCoverageData(bool start = false) REQUIRES(!Locks::mutator_lock_, !Locks::thread_list_lock_, !Locks::thread_suspend_count_lock_);

  static ClassLoadCallback* GetClassLoadCallback() { return &class_load_callback_; }

  static bool IsMiniTraceActive() { return the_trace_ != nullptr; }

 private:
  class MiniTraceClassLoadCallback : public ClassLoadCallback {
   public:
    void ClassLoad(Handle<mirror::Class> klass) OVERRIDE REQUIRES_SHARED(Locks::mutator_lock_);
    void ClassPrepare(Handle<mirror::Class> temp_klass,
                      Handle<mirror::Class> klass) OVERRIDE REQUIRES_SHARED(Locks::mutator_lock_);
  };

  static MiniTraceClassLoadCallback class_load_callback_;

  MiniTrace();

  // Singleton instance of the Trace or NULL when no method tracing is active.
  static MiniTrace* volatile the_trace_;

  DISALLOW_COPY_AND_ASSIGN(MiniTrace);
};


}  // namespace art

#endif  // ART_RUNTIME_MINI_TRACE_H_
