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

#include "mini_trace.h"


#include <fstream>
#include <sys/uio.h>
#include <grp.h>
#include <unistd.h>
#include <stdlib.h>

#include "android-base/stringprintf.h"

#include "base/os.h"
#include "base/stl_util.h"
#include "base/unix_file/fd_file.h"
#include "class_linker.h"
#include "common_throws.h"
#include "debugger.h"
#include "dex/dex_file-inl.h"
#include "dex/descriptors_names.h"
#include "instrumentation.h"
#include "art_method-inl.h"
#include "art_field-inl.h"
#include "mirror/class-inl.h"
#include "mirror/dex_cache.h"
#include "mirror/object_array-inl.h"
#include "mirror/object-inl.h"
#include "gc/scoped_gc_critical_section.h"
#include "scoped_thread_state_change.h"
#include "nativehelper/scoped_local_ref.h"
#include "thread.h"
#include "thread_list.h"
#if !defined(ART_USE_PORTABLE_COMPILER)
#include "entrypoints/quick/quick_entrypoints.h"
#endif


namespace art {

using android::base::StringPrintf;

// MiniTrace

static constexpr const char* kMiniTracerInstrumentationKey = "MiniTracer";

MiniTrace* volatile MiniTrace::the_trace_ = nullptr;
MiniTrace::MiniTraceClassLoadCallback MiniTrace::class_load_callback_;

class PostClassPrepareClassVisitor : public ClassVisitor {
 public:
  bool operator()(ObjPtr<mirror::Class> klass) OVERRIDE REQUIRES(Locks::mutator_lock_) {
    MiniTrace::PostClassPrepare(klass.Ptr());
    return true;
  }
};

class DumpCoverageDataClassVisitor: public ClassVisitor {
 public:
  explicit DumpCoverageDataClassVisitor(std::ostream* os)
      : os_(os) {}

  bool operator()(ObjPtr<mirror::Class> klass) OVERRIDE REQUIRES(Locks::mutator_lock_) {
    if (!klass->IsMiniTraceable()) {
      return true;
    }
    ClassLinker* cl = Runtime::Current()->GetClassLinker();
    auto pointer_size = cl->GetImagePointerSize();

    for (ArtMethod& method : klass->GetDeclaredMethods(pointer_size)) {
      MiniTrace::DumpCoverageData(*os_, &method);
    }

    return true;
  }

 private:
  std::ostream* os_;
};

void MiniTrace::DumpCoverageData(std::ostream& os, ArtMethod* method) {
  if (method == NULL) {
    return;
  }
  if (!method->IsMiniTraceable()) {
    return;
  }
  const DexFile::CodeItem* code_item = method->GetCodeItem();
  if (code_item == nullptr) {
    return;
  }
  uint8_t* data = const_cast<uint8_t*>(method->GetCoverageData());
  if (data == nullptr) {
    return;
  }

  uint16_t insns_size = method->DexInstructions().InsnsSizeInCodeUnits();
  if (insns_size == 0) {
    return;
  }

  if (data[0] == 0) {
    int length = (insns_size >> 1);
    uint32_t* p = reinterpret_cast<uint32_t*>(data);
    bool visited = false;
    for (int i = 0; i <= length; i++) {
      if (p[i] != 0) {
        visited = true;
        break;
      }
    }

    if (!visited) {
      return;
    }
  }

  os << StringPrintf("%p\t%s\t%s\t%s\t%s\t%d\t", method,
      PrettyDescriptor(method->GetDeclaringClassDescriptor()).c_str(), method->GetName(),
      method->GetSignature().ToString().c_str(), method->GetDeclaringClassSourceFile(), insns_size);

  for (int i = 0; i < insns_size; i++) {
    if (data[i] != 0) {
      os << 1;
      data[i] = 0;  // clear after dump
    } else {
      os << 0;
    }
  }
  os << '\n';
}

void MiniTrace::DumpCoverageData(bool start) {
  if (!IsMiniTraceActive()) {
    return;
  }

  std::string coverage_data_filename(StringPrintf("/data/mini_trace_%d_coverage.dat",
                                         getuid()));

  std::unique_ptr<File> file;
  if (OS::FileExists(coverage_data_filename.c_str())) {
    file.reset(OS::OpenFileWithFlags(coverage_data_filename.c_str(), O_RDWR | O_APPEND));
  } else {
    file.reset(OS::CreateEmptyFile(coverage_data_filename.c_str()));
  }

  if (file.get() == nullptr) {
    LOG(INFO) << "Failed to open coverage data file " << coverage_data_filename;
    return;
  }


  std::ostringstream os;
  if (start) {
    LOG(INFO) << "MiniTrace: Try to start coverage data";
    os << "Start\t" << getpid() << '\t' << MilliTime() << '\n';
  } else {
    LOG(INFO) << "MiniTrace: Try to dump coverage data";

    os << "Dump\t" << getpid() << '\t' << MilliTime() << '\n';

    ScopedObjectAccess soa(Thread::Current());
    Runtime* runtime = Runtime::Current();
    DumpCoverageDataClassVisitor visitor(&os);
    runtime->GetClassLinker()->VisitClasses(&visitor);
  }

  std::string data(os.str());
  if (!file->WriteFully(data.c_str(), data.length())) {
    LOG(INFO) << "Failed to write coverage data file " << coverage_data_filename;
    file->Erase();
    return;
  }
  if (file->FlushCloseOrErase() != 0) {
    LOG(INFO) << "Failed to flush coverage data file " << coverage_data_filename;
    return;
  }
}

void MiniTrace::Start() {
  LOG(INFO) << "MiniTrace: Try to start";
  Thread* self = Thread::Current();
  {
    MutexLock mu(self, *Locks::trace_lock_);
    if (the_trace_ != nullptr) {
      LOG(ERROR) << "Trace already in progress, ignoring this request";
      return;
    }
  }

  const char* trace_base_filename = "/data/mini_trace_";

  {
    std::ostringstream os;
    os << trace_base_filename << getuid()  << "_config.in";
    std::string trace_config_filename(os.str());

    if (OS::FileExists(trace_config_filename.c_str())) {
      std::ifstream in(trace_config_filename.c_str());
      if (!in) {
        LOG(INFO) << "MiniTrace: config file " << trace_config_filename << " exists but can't be opened";
        return;
      }
    } else {
      LOG(INFO) << "MiniTrace: config file " << trace_config_filename << " does not exist";
      return;
    }
  }

  // Create Trace object.
  {
    // Required since EnableMethodTracing calls ConfigureStubs which visits class linker classes.
    gc::ScopedGCCriticalSection gcs(self,
        gc::kGcCauseInstrumentation,
        gc::kCollectorTypeInstrumentation);
    ScopedSuspendAll ssa(__FUNCTION__);
    MutexLock mu(self, *Locks::trace_lock_);

    if (the_trace_ != nullptr) {
      LOG(ERROR) << "Trace already in progress, ignoring this request";
    } else {
      the_trace_ = new MiniTrace();

      Runtime* runtime = Runtime::Current();
      runtime->GetInstrumentation()->AddListener(the_trace_, 0);
      runtime->GetInstrumentation()->EnableMethodTracing(kMiniTracerInstrumentationKey);

      PostClassPrepareClassVisitor visitor;
      runtime->GetClassLinker()->VisitClasses(&visitor);
    }
  }
  DumpCoverageData(true);
}

void MiniTrace::Stop() {
  Thread* self = Thread::Current();

  Runtime* runtime = Runtime::Current();
  MiniTrace* the_trace = nullptr;
  {
    MutexLock mu(Thread::Current(), *Locks::trace_lock_);
    if (the_trace_ == nullptr) {
      LOG(ERROR) << "Trace stop requested, but no trace currently running";
    } else {
      the_trace = the_trace_;
      the_trace_ = nullptr;
    }
  }
  if (the_trace != nullptr) {
    gc::ScopedGCCriticalSection gcs(self,
                                    gc::kGcCauseInstrumentation,
                                    gc::kCollectorTypeInstrumentation);
    ScopedSuspendAll ssa(__FUNCTION__);

    runtime->GetInstrumentation()->DisableMethodTracing(kMiniTracerInstrumentationKey);
    runtime->GetInstrumentation()->RemoveListener(the_trace, 0);

    delete the_trace;
  }
  DumpCoverageData(false);
}

void MiniTrace::Shutdown() {
  if (IsMiniTraceActive()) {
    Stop();
  }
}

MiniTrace::MiniTrace() {}

void MiniTrace::DexPcMoved(Thread* thread, Handle<mirror::Object> this_object,
                       ArtMethod* method, uint32_t new_dex_pc) {
  UNUSED(thread, this_object, method, new_dex_pc);
}

void MiniTrace::FieldRead(Thread* thread, Handle<mirror::Object> this_object,
                       ArtMethod* method, uint32_t dex_pc, ArtField* field) {
  UNUSED(thread, this_object, method, dex_pc, field);
}

void MiniTrace::FieldWritten(Thread* thread, Handle<mirror::Object> this_object,
                          ArtMethod* method, uint32_t dex_pc, ArtField* field,
                          const JValue& field_value) {
  UNUSED(thread, this_object, method, dex_pc, field, field_value);
}

void MiniTrace::MethodEntered(Thread* thread, Handle<mirror::Object> this_object,
                          ArtMethod* method, uint32_t dex_pc) {
  UNUSED(thread, this_object, method, dex_pc);
}

void MiniTrace::MethodExited(Thread* thread, Handle<mirror::Object> this_object,
                         ArtMethod* method, uint32_t dex_pc,
                         Handle<mirror::Object> return_value) {
  UNUSED(thread, this_object, method, dex_pc, return_value);
}

void MiniTrace::MethodExited(Thread* thread, Handle<mirror::Object> this_object,
                         ArtMethod* method, uint32_t dex_pc,
                         const JValue& return_value) {
  UNUSED(thread, this_object, method, dex_pc, return_value);
}

void MiniTrace::MethodUnwind(Thread* thread, Handle<mirror::Object> this_object,
                         ArtMethod* method, uint32_t dex_pc) {
  UNUSED(thread, this_object, method, dex_pc);
}

void MiniTrace::ExceptionThrown(Thread* thread, Handle<mirror::Throwable> exception_object) {
  UNUSED(thread, exception_object);
}

void MiniTrace::ExceptionHandled(Thread* thread, Handle<mirror::Throwable> exception_object) {
  UNUSED(thread, exception_object);
}

void MiniTrace::Branch(Thread* thread, ArtMethod* method, uint32_t dex_pc, int32_t dex_pc_offset) {
  UNUSED(thread, method, dex_pc, dex_pc_offset);
}

void MiniTrace::InvokeVirtualOrInterface(Thread* thread, Handle<mirror::Object> this_object, ArtMethod* caller, uint32_t dex_pc, ArtMethod* callee) {
  UNUSED(thread, this_object, caller, dex_pc, callee);
}

void MiniTrace::WatchedFramePop(Thread* thread ATTRIBUTE_UNUSED, const ShadowFrame& frame ATTRIBUTE_UNUSED) {}

void MiniTrace::PostClassPrepare(mirror::Class* klass) {
  if (klass->IsArrayClass() || klass->IsInterface() || klass->IsPrimitive() || klass->IsProxyClass()) {
    return;
  }

  const char* prefix = "/system/framework/";
  const char* location = klass->GetDexFile().GetLocation().c_str();
  if (strncmp(location, prefix, strlen(prefix)) == 0) {
    return;
  }

  // Set flags
  klass->SetIsMiniTraceable();
  if (IsMiniTraceActive()) {
    // Install Stubs
    Runtime::Current()->GetInstrumentation()->InstallStubsForClass(klass);
  }
}

void MiniTrace::MiniTraceClassLoadCallback::ClassLoad(Handle<mirror::Class> klass ATTRIBUTE_UNUSED) {
  // Ignore ClassLoad;
}
void MiniTrace::MiniTraceClassLoadCallback::ClassPrepare(Handle<mirror::Class> temp_klass ATTRIBUTE_UNUSED,
                                             Handle<mirror::Class> klass) {
  MiniTrace::PostClassPrepare(klass.Get());
}

}  // namespace art
