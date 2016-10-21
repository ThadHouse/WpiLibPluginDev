#include <jni.h>
#include <atomic>
#include <cassert>
#include <condition_variable>
#include <mutex>
#include <sstream>
#include <queue>
#include <thread>

#include "edu_wpi_first_wpilibj_networktables_NetworkTablesJNI.h"
#include "support/atomic_static.h"
#include "support/jni_util.h"
#include "support/SafeThread.h"
#include "llvm/ConvertUTF.h"
#include "llvm/SmallString.h"
#include "llvm/SmallVector.h"

using namespace wpi::java;

//
// Globals and load/unload
//

// Used for callback.
static JavaVM *jvm = nullptr;
static jclass booleanCls = nullptr;
static jclass doubleCls = nullptr;
static jclass connectionInfoCls = nullptr;
static jclass entryInfoCls = nullptr;
static jclass keyNotDefinedEx = nullptr;
static jclass persistentEx = nullptr;
static jclass illegalArgEx = nullptr;
// Thread-attached environment for listener callbacks.
//static JNIEnv *listenerEnv = nullptr;

/*
static void ListenerOnStart() {
  if (!jvm) return;
  JNIEnv *env;
  JavaVMAttachArgs args;
  args.version = JNI_VERSION_1_2;
  args.name = const_cast<char*>("NTListener");
  args.group = nullptr;
  if (jvm->AttachCurrentThreadAsDaemon(reinterpret_cast<void **>(&env),
                                       &args) != JNI_OK)
    return;
  if (!env || !env->functions) return;
  listenerEnv = env;
}

static void ListenerOnExit() {
  listenerEnv = nullptr;
  if (!jvm) return;
  jvm->DetachCurrentThread();
}
*/

extern "C" {

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved) {
  jvm = vm;

  JNIEnv *env;
  if (vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) != JNI_OK)
    return JNI_ERR;

  // Cache references to classes
  jclass local;

  local = env->FindClass("java/lang/Boolean");
  if (!local) return JNI_ERR;
  booleanCls = static_cast<jclass>(env->NewGlobalRef(local));
  if (!booleanCls) return JNI_ERR;
  env->DeleteLocalRef(local);

  local = env->FindClass("java/lang/Double");
  if (!local) return JNI_ERR;
  doubleCls = static_cast<jclass>(env->NewGlobalRef(local));
  if (!doubleCls) return JNI_ERR;
  env->DeleteLocalRef(local);

  local = env->FindClass("edu/wpi/first/wpilibj/networktables/ConnectionInfo");
  if (!local) return JNI_ERR;
  connectionInfoCls = static_cast<jclass>(env->NewGlobalRef(local));
  if (!connectionInfoCls) return JNI_ERR;
  env->DeleteLocalRef(local);

  local = env->FindClass("edu/wpi/first/wpilibj/networktables/EntryInfo");
  if (!local) return JNI_ERR;
  entryInfoCls = static_cast<jclass>(env->NewGlobalRef(local));
  if (!entryInfoCls) return JNI_ERR;
  env->DeleteLocalRef(local);

  local =
      env->FindClass("edu/wpi/first/wpilibj/networktables/NetworkTableKeyNotDefined");
  keyNotDefinedEx = static_cast<jclass>(env->NewGlobalRef(local));
  if (!keyNotDefinedEx) return JNI_ERR;
  env->DeleteLocalRef(local);

  local =
      env->FindClass("edu/wpi/first/wpilibj/networktables/PersistentException");
  persistentEx = static_cast<jclass>(env->NewGlobalRef(local));
  if (!persistentEx) return JNI_ERR;
  env->DeleteLocalRef(local);

  local = env->FindClass("java/lang/IllegalArgumentException");
  if (!local) return JNI_ERR;
  illegalArgEx = static_cast<jclass>(env->NewGlobalRef(local));
  if (!illegalArgEx) return JNI_ERR;
  env->DeleteLocalRef(local);

  /*
  // Initial configuration of listener start/exit
  nt::SetListenerOnStart(ListenerOnStart);
  nt::SetListenerOnExit(ListenerOnExit);
  */

  return JNI_VERSION_1_6;
}

JNIEXPORT void JNICALL JNI_OnUnload(JavaVM *vm, void *reserved) {
  JNIEnv *env;
  if (vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) != JNI_OK)
    return;
  // Delete global references
  if (booleanCls) env->DeleteGlobalRef(booleanCls);
  if (doubleCls) env->DeleteGlobalRef(doubleCls);
  if (connectionInfoCls) env->DeleteGlobalRef(connectionInfoCls);
  if (entryInfoCls) env->DeleteGlobalRef(entryInfoCls);
  if (keyNotDefinedEx) env->DeleteGlobalRef(keyNotDefinedEx);
  if (persistentEx) env->DeleteGlobalRef(persistentEx);
  if (illegalArgEx) env->DeleteGlobalRef(illegalArgEx);
  jvm = nullptr;
}

}  // extern "C"

//
// Helper class to create and clean up a global reference
//
template <typename T>
class JGlobal {
 public:
  JGlobal(JNIEnv *env, T obj)
      : m_obj(static_cast<T>(env->NewGlobalRef(obj))) {}
  ~JGlobal() {
    if (!jvm) return;
    JNIEnv *env;
    bool attached = false;
    // don't attach and de-attach if already attached to a thread.
    if (jvm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) ==
        JNI_EDETACHED) {
      if (jvm->AttachCurrentThread(reinterpret_cast<void **>(&env), nullptr) !=
          JNI_OK)
        return;
      attached = true;
    }
    if (!env || !env->functions) return;
    env->DeleteGlobalRef(m_obj);
    if (attached) jvm->DetachCurrentThread();
  }
  operator T() { return m_obj; }
  T obj() { return m_obj; }

 private:
  T m_obj;
};

//
// Helper class to create and clean up a weak global reference
//
template <typename T>
class JWeakGlobal {
 public:
  JWeakGlobal(JNIEnv *env, T obj)
      : m_obj(static_cast<T>(env->NewWeakGlobalRef(obj))) {}
  ~JWeakGlobal() {
    if (!jvm) return;
    JNIEnv *env;
    bool attached = false;
    // don't attach and de-attach if already attached to a thread.
    if (jvm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) ==
        JNI_EDETACHED) {
      if (jvm->AttachCurrentThread(reinterpret_cast<void **>(&env), nullptr) !=
          JNI_OK)
        return;
      attached = true;
    }
    if (!env || !env->functions) return;
    env->DeleteWeakGlobalRef(m_obj);
    if (attached) jvm->DetachCurrentThread();
  }
  JLocal<T> obj(JNIEnv *env) {
    return JLocal<T>{env, env->NewLocalRef(m_obj)};
  }

 private:
  T m_obj;
};
