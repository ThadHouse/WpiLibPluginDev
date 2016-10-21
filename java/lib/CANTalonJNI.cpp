#include <jni.h>
#include <atomic>
#include <cassert>
#include <condition_variable>
#include <mutex>
#include <sstream>
#include <queue>
#include <thread>
#include <cstring>
#include <string>



#include "com_ctre_CanTalonJNI.h"

#include "CanTalonSRX.h"

#include "HAL/HAL.h"

//using namespace wpi::java;

//
// Globals and load/unload
//

// Used for callback.
static JavaVM *jvm = nullptr;
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
  
static jclass runtimeExCls = nullptr;
static jclass throwableCls = nullptr;
static jclass stackTraceElementCls = nullptr;

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved) {
  jvm = vm;

  JNIEnv *env;
  if (vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) != JNI_OK)
    return JNI_ERR;

  // Cache references to classes
  jclass local;
  
  local = env->FindClass("java/lang/Throwable");
  if (!local) return JNI_ERR;
  throwableCls = static_cast<jclass>(env->NewGlobalRef(local));
  if (!throwableCls) return JNI_ERR;
  env->DeleteLocalRef(local);

  local = env->FindClass("java/lang/StackTraceElement");
  if (!local) return JNI_ERR;
  stackTraceElementCls = static_cast<jclass>(env->NewGlobalRef(local));
  if (!stackTraceElementCls) return JNI_ERR;
  env->DeleteLocalRef(local);

  local = env->FindClass("java/lang/RuntimeException");
  if (!local) return JNI_ERR;
  runtimeExCls = static_cast<jclass>(env->NewGlobalRef(local));
  if (!runtimeExCls) return JNI_ERR;
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
  if (throwableCls) env->DeleteGlobalRef(throwableCls);
  if (stackTraceElementCls) env->DeleteGlobalRef(stackTraceElementCls);
  if (runtimeExCls) env->DeleteGlobalRef(runtimeExCls);
  jvm = nullptr;
}

}  // extern "C"

/*
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
*/


//#include "HALUtil.h"


static void GetStackTrace(JNIEnv *env, std::string &res, std::string &func) {
  // create a throwable
  static jmethodID constructorId = nullptr;
  if (!constructorId)
    constructorId = env->GetMethodID(throwableCls, "<init>", "()V");
  jobject throwable = env->NewObject(throwableCls, constructorId);

  // retrieve information from the exception.
  // get method id
  // getStackTrace returns an array of StackTraceElement
  static jmethodID getStackTraceId = nullptr;
  if (!getStackTraceId)
    getStackTraceId = env->GetMethodID(throwableCls, "getStackTrace",
                                       "()[Ljava/lang/StackTraceElement;");

  // call getStackTrace
  jobjectArray stackTrace =
      static_cast<jobjectArray>(env->CallObjectMethod(throwable,
                                                      getStackTraceId));

  if (!stackTrace) return;

  // get length of the array
  jsize stackTraceLength = env->GetArrayLength(stackTrace);

  // get toString methodId of StackTraceElement class
  static jmethodID toStringId = nullptr;
  if (!toStringId)
    toStringId = env->GetMethodID(stackTraceElementCls, "toString",
                                  "()Ljava/lang/String;");

  bool haveLoc = false;
  for (jsize i = 0; i < stackTraceLength; i++) {
    // add the result of toString method of each element in the result
    jobject curStackTraceElement = env->GetObjectArrayElement(stackTrace, i);

    // call to string on the object
    jstring stackElementString =
        static_cast<jstring>(env->CallObjectMethod(curStackTraceElement,
                                                   toStringId));

    if (!stackElementString) {
      env->DeleteLocalRef(stackTrace);
      env->DeleteLocalRef(curStackTraceElement);
      return;
    }

    // add a line to res
    // res += " at ";
    const char *tmp = env->GetStringUTFChars(stackElementString, nullptr);
    res += tmp;
    res += '\n';

    // func is caller of immediate caller (if there was one)
    // or, if we see it, the first user function
    if (i == 1)
      func = tmp;
    else if (i > 1 && !haveLoc &&
             std::strncmp(tmp, "edu.wpi.first.wpilibj", 21) != 0) {
      func = tmp;
      haveLoc = true;
    }
    env->ReleaseStringUTFChars(stackElementString, tmp);

    env->DeleteLocalRef(curStackTraceElement);
    env->DeleteLocalRef(stackElementString);
  }

  // release java resources
  env->DeleteLocalRef(stackTrace);
}
  
static void ReportError(JNIEnv *env, int32_t status, bool do_throw) {
  if (status == 0) return;
  const char *message = HAL_GetErrorMessage(status);
  if (do_throw && status < 0) {
    char *buf = new char[strlen(message) + 30];
    sprintf(buf, " Code: %d. %s", status, message);
    env->ThrowNew(runtimeExCls, buf);
    delete[] buf;
  } else {
    std::string stack = " at ";
    std::string func;
    GetStackTrace(env, stack, func);
    HAL_SendError(1, status, 0, message, func.c_str(), stack.c_str(), 1);
  }
}
  

inline bool CheckCTRStatus(JNIEnv *env, CTR_Code status) {
  if (status != CTR_OKAY) ReportError(env, (int32_t)status, false);
  return status == CTR_OKAY;
}

extern "C" {

/*
 * Class:     edu_wpi_first_wpilibj_hal_CanTalonJNI
 * Method:    new_CanTalonSRX
 * Signature: (III)J
 */
JNIEXPORT jlong JNICALL Java_com_ctre_CanTalonJNI_new_1CanTalonSRX__III
  (JNIEnv *env, jclass, jint deviceNumber, jint controlPeriodMs, jint enablePeriodMs)
{
  return (jlong)(new CanTalonSRX((int)deviceNumber, (int)controlPeriodMs, (int)enablePeriodMs));
}

/*
 * Class:     edu_wpi_first_wpilibj_hal_CanTalonJNI
 * Method:    new_CanTalonSRX
 * Signature: (II)J
 */
JNIEXPORT jlong JNICALL Java_com_ctre_CanTalonJNI_new_1CanTalonSRX__II
  (JNIEnv *env, jclass, jint deviceNumber, jint controlPeriodMs)
{
  return (jlong)(new CanTalonSRX((int)deviceNumber, (int)controlPeriodMs));
}

/*
 * Class:     edu_wpi_first_wpilibj_hal_CanTalonJNI
 * Method:    new_CanTalonSRX
 * Signature: (I)J
 */
JNIEXPORT jlong JNICALL Java_com_ctre_CanTalonJNI_new_1CanTalonSRX__I
  (JNIEnv *env, jclass, jint deviceNumber)
{
  return (jlong)(new CanTalonSRX((int)deviceNumber));
}

/*
 * Class:     edu_wpi_first_wpilibj_hal_CanTalonJNI
 * Method:    new_CanTalonSRX
 * Signature: ()J
 */
JNIEXPORT jlong JNICALL Java_com_ctre_CanTalonJNI_new_1CanTalonSRX__
  (JNIEnv *env, jclass)
{
  return (jlong)(new CanTalonSRX);
}

/*
 * Class:     edu_wpi_first_wpilibj_hal_CanTalonJNI
 * Method:    delete_CanTalonSRX
 * Signature: (J)V
 */
JNIEXPORT void JNICALL Java_com_ctre_CanTalonJNI_delete_1CanTalonSRX
  (JNIEnv *env, jclass, jlong handle)
{
  delete (CanTalonSRX*)handle;
}

/*
 * Class:     edu_wpi_first_wpilibj_hal_CanTalonJNI
 * Method:    GetMotionProfileStatus
 * Signature: (JLedu/wpi/first/wpilibj/CANTalon;Ledu/wpi/first/wpilibj/CANTalon/MotionProfileStatus;)V
 */
JNIEXPORT void JNICALL Java_com_ctre_CanTalonJNI_GetMotionProfileStatus
  (JNIEnv *env, jclass, jlong handle, jobject canTalon, jobject motionProfileStatus)
{
  static jmethodID setMotionProfileStatusFromJNI = nullptr;
  if (!setMotionProfileStatusFromJNI) {
    jclass cls = env->GetObjectClass(canTalon);
    setMotionProfileStatusFromJNI = env->GetMethodID(cls, "setMotionProfileStatusFromJNI", "(Ledu/wpi/first/wpilibj/CANTalon$MotionProfileStatus;IIIIIIII)V");
    if (!setMotionProfileStatusFromJNI) return;
  }

  uint32_t flags;
  uint32_t profileSlotSelect;
  int32_t targPos;
  int32_t targVel;
  uint32_t topBufferRem;
  uint32_t topBufferCnt;
  uint32_t btmBufferCnt;
  uint32_t outputEnable;
  CTR_Code status = ((CanTalonSRX*)handle)->GetMotionProfileStatus(flags, profileSlotSelect, targPos, targVel, topBufferRem, topBufferCnt, btmBufferCnt, outputEnable);
  if (!CheckCTRStatus(env, status)) return;

  env->CallVoidMethod(canTalon, setMotionProfileStatusFromJNI, motionProfileStatus, (jint)flags, (jint)profileSlotSelect, (jint)targPos, (jint)targVel, (jint)topBufferRem, (jint)topBufferCnt, (jint)btmBufferCnt, (jint)outputEnable);
}

JNIEXPORT void JNICALL Java_com_ctre_CanTalonJNI_Set
  (JNIEnv *env, jclass, jlong handle, jdouble value)
{
  return ((CanTalonSRX*)handle)->Set((double)value);
}
JNIEXPORT void JNICALL Java_com_ctre_CanTalonJNI_SetParam
  (JNIEnv *env, jclass, jlong handle, jint paramEnum, jdouble value)
{
  CTR_Code status = ((CanTalonSRX*)handle)->SetParam((CanTalonSRX::param_t)paramEnum, (double)value);
  CheckCTRStatus(env, status);
}
JNIEXPORT void JNICALL Java_com_ctre_CanTalonJNI_RequestParam
  (JNIEnv *env, jclass, jlong handle, jint paramEnum)
{
  CTR_Code status = ((CanTalonSRX*)handle)->RequestParam((CanTalonSRX::param_t)paramEnum);
  CheckCTRStatus(env, status);
}
JNIEXPORT jdouble JNICALL Java_com_ctre_CanTalonJNI_GetParamResponse
  (JNIEnv *env, jclass, jlong handle, jint paramEnum)
{
  double value;
  CTR_Code status = ((CanTalonSRX*)handle)->GetParamResponse((CanTalonSRX::param_t)paramEnum, value);
  CheckCTRStatus(env, status);
  return value;
}
JNIEXPORT jint JNICALL Java_com_ctre_CanTalonJNI_GetParamResponseInt32
  (JNIEnv *env, jclass, jlong handle, jint paramEnum)
{
  int value;
  CTR_Code status = ((CanTalonSRX*)handle)->GetParamResponseInt32((CanTalonSRX::param_t)paramEnum, value);
  CheckCTRStatus(env, status);
  return value;
}
JNIEXPORT void JNICALL Java_com_ctre_CanTalonJNI_SetPgain
  (JNIEnv *env, jclass, jlong handle, jint slotIdx, jdouble gain)
{
  CTR_Code status = ((CanTalonSRX*)handle)->SetPgain((unsigned)slotIdx, (double)gain);
  CheckCTRStatus(env, status);
}
JNIEXPORT void JNICALL Java_com_ctre_CanTalonJNI_SetIgain
  (JNIEnv *env, jclass, jlong handle, jint slotIdx, jdouble gain)
{
  CTR_Code status = ((CanTalonSRX*)handle)->SetIgain((unsigned)slotIdx, (double)gain);
  CheckCTRStatus(env, status);
}
JNIEXPORT void JNICALL Java_com_ctre_CanTalonJNI_SetDgain
  (JNIEnv *env, jclass, jlong handle, jint slotIdx, jdouble gain)
{
  CTR_Code status = ((CanTalonSRX*)handle)->SetDgain((unsigned)slotIdx, (double)gain);
  CheckCTRStatus(env, status);
}
JNIEXPORT void JNICALL Java_com_ctre_CanTalonJNI_SetFgain
  (JNIEnv *env, jclass, jlong handle, jint slotIdx, jdouble gain)
{
  CTR_Code status = ((CanTalonSRX*)handle)->SetFgain((unsigned)slotIdx, (double)gain);
  CheckCTRStatus(env, status);
}
JNIEXPORT void JNICALL Java_com_ctre_CanTalonJNI_SetIzone
  (JNIEnv *env, jclass, jlong handle, jint slotIdx, jint zone)
{
  CTR_Code status = ((CanTalonSRX*)handle)->SetIzone((unsigned)slotIdx, (int)zone);
  CheckCTRStatus(env, status);
}
JNIEXPORT void JNICALL Java_com_ctre_CanTalonJNI_SetCloseLoopRampRate
  (JNIEnv *env, jclass, jlong handle, jint slotIdx, jint closeLoopRampRate)
{
  CTR_Code status = ((CanTalonSRX*)handle)->SetCloseLoopRampRate((unsigned)slotIdx, (int)closeLoopRampRate);
  CheckCTRStatus(env, status);
}
JNIEXPORT void JNICALL Java_com_ctre_CanTalonJNI_SetVoltageCompensationRate
  (JNIEnv *env, jclass, jlong handle, jdouble voltagePerMs)
{
  CTR_Code status = ((CanTalonSRX*)handle)->SetVoltageCompensationRate((double)voltagePerMs);
  CheckCTRStatus(env, status);
}
JNIEXPORT void JNICALL Java_com_ctre_CanTalonJNI_SetSensorPosition
  (JNIEnv *env, jclass, jlong handle, jint pos)
{
  CTR_Code status = ((CanTalonSRX*)handle)->SetSensorPosition((int)pos);
  CheckCTRStatus(env, status);
}
JNIEXPORT void JNICALL Java_com_ctre_CanTalonJNI_SetForwardSoftLimit
  (JNIEnv *env, jclass, jlong handle, jint forwardLimit)
{
  CTR_Code status = ((CanTalonSRX*)handle)->SetForwardSoftLimit((int)forwardLimit);
  CheckCTRStatus(env, status);
}
JNIEXPORT void JNICALL Java_com_ctre_CanTalonJNI_SetReverseSoftLimit
  (JNIEnv *env, jclass, jlong handle, jint reverseLimit)
{
  CTR_Code status = ((CanTalonSRX*)handle)->SetReverseSoftLimit((int)reverseLimit);
  CheckCTRStatus(env, status);
}
JNIEXPORT void JNICALL Java_com_ctre_CanTalonJNI_SetForwardSoftEnable
  (JNIEnv *env, jclass, jlong handle, jint enable)
{
  CTR_Code status = ((CanTalonSRX*)handle)->SetForwardSoftEnable((int)enable);
  CheckCTRStatus(env, status);
}
JNIEXPORT void JNICALL Java_com_ctre_CanTalonJNI_SetReverseSoftEnable
  (JNIEnv *env, jclass, jlong handle, jint enable)
{
  CTR_Code status = ((CanTalonSRX*)handle)->SetReverseSoftEnable((int)enable);
  CheckCTRStatus(env, status);
}
JNIEXPORT jdouble JNICALL Java_com_ctre_CanTalonJNI_GetPgain
  (JNIEnv *env, jclass, jlong handle, jint slotIdx)
{
  double gain;
  CTR_Code status = ((CanTalonSRX*)handle)->GetPgain((unsigned)slotIdx, gain);
  CheckCTRStatus(env, status);
  return gain;
}
JNIEXPORT jdouble JNICALL Java_com_ctre_CanTalonJNI_GetIgain
  (JNIEnv *env, jclass, jlong handle, jint slotIdx)
{
  double gain;
  CTR_Code status = ((CanTalonSRX*)handle)->GetIgain((unsigned)slotIdx, gain);
  CheckCTRStatus(env, status);
  return gain;
}
JNIEXPORT jdouble JNICALL Java_com_ctre_CanTalonJNI_GetDgain
  (JNIEnv *env, jclass, jlong handle, jint slotIdx)
{
  double gain;
  CTR_Code status = ((CanTalonSRX*)handle)->GetDgain((unsigned)slotIdx, gain);
  CheckCTRStatus(env, status);
  return gain;
}
JNIEXPORT jdouble JNICALL Java_com_ctre_CanTalonJNI_GetFgain
  (JNIEnv *env, jclass, jlong handle, jint slotIdx)
{
  double gain;
  CTR_Code status = ((CanTalonSRX*)handle)->GetFgain((unsigned)slotIdx, gain);
  CheckCTRStatus(env, status);
  return gain;
}
JNIEXPORT jint JNICALL Java_com_ctre_CanTalonJNI_GetIzone
  (JNIEnv *env, jclass, jlong handle, jint slotIdx)
{
  int zone;
  CTR_Code status = ((CanTalonSRX*)handle)->GetIzone((unsigned)slotIdx, zone);
  CheckCTRStatus(env, status);
  return zone;
}
JNIEXPORT jint JNICALL Java_com_ctre_CanTalonJNI_GetCloseLoopRampRate
  (JNIEnv *env, jclass, jlong handle, jint slotIdx)
{
  int closeLoopRampRate;
  CTR_Code status = ((CanTalonSRX*)handle)->GetCloseLoopRampRate((unsigned)slotIdx, closeLoopRampRate);
  CheckCTRStatus(env, status);
  return closeLoopRampRate;
}
JNIEXPORT jdouble JNICALL Java_com_ctre_CanTalonJNI_GetVoltageCompensationRate
  (JNIEnv *env, jclass, jlong handle)
{
  double voltagePerMs;
  CTR_Code status = ((CanTalonSRX*)handle)->GetVoltageCompensationRate(voltagePerMs);
  CheckCTRStatus(env, status);
  return voltagePerMs;
}
JNIEXPORT jint JNICALL Java_com_ctre_CanTalonJNI_GetForwardSoftLimit
  (JNIEnv *env, jclass, jlong handle)
{
  int forwardLimit;
  CTR_Code status = ((CanTalonSRX*)handle)->GetForwardSoftLimit(forwardLimit);
  CheckCTRStatus(env, status);
  return forwardLimit;
}
JNIEXPORT jint JNICALL Java_com_ctre_CanTalonJNI_GetReverseSoftLimit
  (JNIEnv *env, jclass, jlong handle)
{
  int reverseLimit;
  CTR_Code status = ((CanTalonSRX*)handle)->GetReverseSoftLimit(reverseLimit);
  CheckCTRStatus(env, status);
  return reverseLimit;
}
JNIEXPORT jint JNICALL Java_com_ctre_CanTalonJNI_GetForwardSoftEnable
  (JNIEnv *env, jclass, jlong handle)
{
  int enable;
  CTR_Code status = ((CanTalonSRX*)handle)->GetForwardSoftEnable(enable);
  CheckCTRStatus(env, status);
  return enable;
}
JNIEXPORT jint JNICALL Java_com_ctre_CanTalonJNI_GetReverseSoftEnable
  (JNIEnv *env, jclass, jlong handle)
{
  int enable;
  CTR_Code status = ((CanTalonSRX*)handle)->GetReverseSoftEnable(enable);
  CheckCTRStatus(env, status);
  return enable;
}
JNIEXPORT jint JNICALL Java_com_ctre_CanTalonJNI_GetPulseWidthRiseToFallUs
  (JNIEnv *env, jclass, jlong handle)
{
  int param;
  CTR_Code status = ((CanTalonSRX*)handle)->GetPulseWidthRiseToFallUs(param);
  CheckCTRStatus(env, status);
  return param;
}
JNIEXPORT jint JNICALL Java_com_ctre_CanTalonJNI_IsPulseWidthSensorPresent
  (JNIEnv *env, jclass, jlong handle)
{
  int param;
  CTR_Code status = ((CanTalonSRX*)handle)->IsPulseWidthSensorPresent(param);
  CheckCTRStatus(env, status);
  return param;
}
JNIEXPORT void JNICALL Java_com_ctre_CanTalonJNI_SetModeSelect2
  (JNIEnv *env, jclass, jlong handle, jint modeSelect, jint demand)
{
  CTR_Code status = ((CanTalonSRX*)handle)->SetModeSelect((int)modeSelect, (int)demand);
  CheckCTRStatus(env, status);
}
JNIEXPORT void JNICALL Java_com_ctre_CanTalonJNI_SetStatusFrameRate
  (JNIEnv *env, jclass, jlong handle, jint frameEnum, jint periodMs)
{
  CTR_Code status = ((CanTalonSRX*)handle)->SetStatusFrameRate((unsigned)frameEnum, (unsigned)periodMs);
  CheckCTRStatus(env, status);
}
JNIEXPORT void JNICALL Java_com_ctre_CanTalonJNI_ClearStickyFaults
  (JNIEnv *env, jclass, jlong handle)
{
  CTR_Code status = ((CanTalonSRX*)handle)->ClearStickyFaults();
  CheckCTRStatus(env, status);
}
JNIEXPORT void JNICALL Java_com_ctre_CanTalonJNI_ChangeMotionControlFramePeriod
  (JNIEnv *env, jclass, jlong handle, jint periodMs)
{
  return ((CanTalonSRX*)handle)->ChangeMotionControlFramePeriod((uint32_t)periodMs);
}
JNIEXPORT void JNICALL Java_com_ctre_CanTalonJNI_ClearMotionProfileTrajectories
  (JNIEnv *env, jclass, jlong handle)
{
  return ((CanTalonSRX*)handle)->ClearMotionProfileTrajectories();
}
JNIEXPORT jint JNICALL Java_com_ctre_CanTalonJNI_GetMotionProfileTopLevelBufferCount
  (JNIEnv *env, jclass, jlong handle)
{
  return ((CanTalonSRX*)handle)->GetMotionProfileTopLevelBufferCount();
}
JNIEXPORT jboolean JNICALL Java_com_ctre_CanTalonJNI_IsMotionProfileTopLevelBufferFull
  (JNIEnv *env, jclass, jlong handle)
{
  return ((CanTalonSRX*)handle)->IsMotionProfileTopLevelBufferFull();
}
JNIEXPORT void JNICALL Java_com_ctre_CanTalonJNI_PushMotionProfileTrajectory
  (JNIEnv *env, jclass, jlong handle, jint targPos, jint targVel, jint profileSlotSelect, jint timeDurMs, jint velOnly, jint isLastPoint, jint zeroPos)
{
  CTR_Code status = ((CanTalonSRX*)handle)->PushMotionProfileTrajectory((int)targPos, (int)targVel, (int)profileSlotSelect, (int)timeDurMs, (int)velOnly, (int)isLastPoint, (int)zeroPos);
  CheckCTRStatus(env, status);
}
JNIEXPORT void JNICALL Java_com_ctre_CanTalonJNI_ProcessMotionProfileBuffer
  (JNIEnv *env, jclass, jlong handle)
{
  return ((CanTalonSRX*)handle)->ProcessMotionProfileBuffer();
}
JNIEXPORT jint JNICALL Java_com_ctre_CanTalonJNI_GetFault_1OverTemp
  (JNIEnv * env, jclass, jlong handle)
{
  int retval;
  CTR_Code status = ((CanTalonSRX*)handle)->GetFault_OverTemp(retval);
  CheckCTRStatus(env, status);
  return retval;
}
JNIEXPORT jint JNICALL Java_com_ctre_CanTalonJNI_GetFault_1UnderVoltage
  (JNIEnv * env, jclass, jlong handle)
{
  int retval;
  CTR_Code status = ((CanTalonSRX*)handle)->GetFault_UnderVoltage(retval);
  CheckCTRStatus(env, status);
  return retval;
}
JNIEXPORT jint JNICALL Java_com_ctre_CanTalonJNI_GetFault_1ForLim
  (JNIEnv * env, jclass, jlong handle)
{
  int retval;
  CTR_Code status = ((CanTalonSRX*)handle)->GetFault_ForLim(retval);
  CheckCTRStatus(env, status);
  return retval;
}
JNIEXPORT jint JNICALL Java_com_ctre_CanTalonJNI_GetFault_1RevLim
  (JNIEnv * env, jclass, jlong handle)
{
  int retval;
  CTR_Code status = ((CanTalonSRX*)handle)->GetFault_RevLim(retval);
  CheckCTRStatus(env, status);
  return retval;
}
JNIEXPORT jint JNICALL Java_com_ctre_CanTalonJNI_GetFault_1HardwareFailure
  (JNIEnv * env, jclass, jlong handle)
{
  int retval;
  CTR_Code status = ((CanTalonSRX*)handle)->GetFault_HardwareFailure(retval);
  CheckCTRStatus(env, status);
  return retval;
}
JNIEXPORT jint JNICALL Java_com_ctre_CanTalonJNI_GetFault_1ForSoftLim
  (JNIEnv * env, jclass, jlong handle)
{
  int retval;
  CTR_Code status = ((CanTalonSRX*)handle)->GetFault_ForSoftLim(retval);
  CheckCTRStatus(env, status);
  return retval;
}
JNIEXPORT jint JNICALL Java_com_ctre_CanTalonJNI_GetFault_1RevSoftLim
  (JNIEnv * env, jclass, jlong handle)
{
  int retval;
  CTR_Code status = ((CanTalonSRX*)handle)->GetFault_RevSoftLim(retval);
  CheckCTRStatus(env, status);
  return retval;
}
JNIEXPORT jint JNICALL Java_com_ctre_CanTalonJNI_GetStckyFault_1OverTemp
  (JNIEnv * env, jclass, jlong handle)
{
  int retval;
  CTR_Code status = ((CanTalonSRX*)handle)->GetStckyFault_OverTemp(retval);
  CheckCTRStatus(env, status);
  return retval;
}
JNIEXPORT jint JNICALL Java_com_ctre_CanTalonJNI_GetStckyFault_1UnderVoltage
  (JNIEnv * env, jclass, jlong handle)
{
  int retval;
  CTR_Code status = ((CanTalonSRX*)handle)->GetStckyFault_UnderVoltage(retval);
  CheckCTRStatus(env, status);
  return retval;
}
JNIEXPORT jint JNICALL Java_com_ctre_CanTalonJNI_GetStckyFault_1ForLim
  (JNIEnv * env, jclass, jlong handle)
{
  int retval;
  CTR_Code status = ((CanTalonSRX*)handle)->GetStckyFault_ForLim(retval);
  CheckCTRStatus(env, status);
  return retval;
}
JNIEXPORT jint JNICALL Java_com_ctre_CanTalonJNI_GetStckyFault_1RevLim
  (JNIEnv * env, jclass, jlong handle)
{
  int retval;
  CTR_Code status = ((CanTalonSRX*)handle)->GetStckyFault_RevLim(retval);
  CheckCTRStatus(env, status);
  return retval;
}
JNIEXPORT jint JNICALL Java_com_ctre_CanTalonJNI_GetStckyFault_1ForSoftLim
  (JNIEnv * env, jclass, jlong handle)
{
  int retval;
  CTR_Code status = ((CanTalonSRX*)handle)->GetStckyFault_ForSoftLim(retval);
  CheckCTRStatus(env, status);
  return retval;
}
JNIEXPORT jint JNICALL Java_com_ctre_CanTalonJNI_GetStckyFault_1RevSoftLim
  (JNIEnv * env, jclass, jlong handle)
{
  int retval;
  CTR_Code status = ((CanTalonSRX*)handle)->GetStckyFault_RevSoftLim(retval);
  CheckCTRStatus(env, status);
  return retval;
}
JNIEXPORT jint JNICALL Java_com_ctre_CanTalonJNI_GetAppliedThrottle
  (JNIEnv * env, jclass, jlong handle)
{
  int retval;
  CTR_Code status = ((CanTalonSRX*)handle)->GetAppliedThrottle(retval);
  CheckCTRStatus(env, status);
  return retval;
}
JNIEXPORT jint JNICALL Java_com_ctre_CanTalonJNI_GetCloseLoopErr
  (JNIEnv * env, jclass, jlong handle)
{
  int retval;
  CTR_Code status = ((CanTalonSRX*)handle)->GetCloseLoopErr(retval);
  CheckCTRStatus(env, status);
  return retval;
}
JNIEXPORT jint JNICALL Java_com_ctre_CanTalonJNI_GetFeedbackDeviceSelect
  (JNIEnv * env, jclass, jlong handle)
{
  int retval;
  CTR_Code status = ((CanTalonSRX*)handle)->GetFeedbackDeviceSelect(retval);
  CheckCTRStatus(env, status);
  return retval;
}
JNIEXPORT jint JNICALL Java_com_ctre_CanTalonJNI_GetModeSelect
  (JNIEnv * env, jclass, jlong handle)
{
  int retval;
  CTR_Code status = ((CanTalonSRX*)handle)->GetModeSelect(retval);
  CheckCTRStatus(env, status);
  return retval;
}
JNIEXPORT jint JNICALL Java_com_ctre_CanTalonJNI_GetLimitSwitchEn
  (JNIEnv * env, jclass, jlong handle)
{
  int retval;
  CTR_Code status = ((CanTalonSRX*)handle)->GetLimitSwitchEn(retval);
  CheckCTRStatus(env, status);
  return retval;
}
JNIEXPORT jint JNICALL Java_com_ctre_CanTalonJNI_GetLimitSwitchClosedFor
  (JNIEnv * env, jclass, jlong handle)
{
  int retval;
  CTR_Code status = ((CanTalonSRX*)handle)->GetLimitSwitchClosedFor(retval);
  CheckCTRStatus(env, status);
  return retval;
}
JNIEXPORT jint JNICALL Java_com_ctre_CanTalonJNI_GetLimitSwitchClosedRev
  (JNIEnv * env, jclass, jlong handle)
{
  int retval;
  CTR_Code status = ((CanTalonSRX*)handle)->GetLimitSwitchClosedRev(retval);
  CheckCTRStatus(env, status);
  return retval;
}
JNIEXPORT jint JNICALL Java_com_ctre_CanTalonJNI_GetSensorPosition
  (JNIEnv * env, jclass, jlong handle)
{
  int retval;
  CTR_Code status = ((CanTalonSRX*)handle)->GetSensorPosition(retval);
  CheckCTRStatus(env, status);
  return retval;
}
JNIEXPORT jint JNICALL Java_com_ctre_CanTalonJNI_GetSensorVelocity
  (JNIEnv * env, jclass, jlong handle)
{
  int retval;
  CTR_Code status = ((CanTalonSRX*)handle)->GetSensorVelocity(retval);
  CheckCTRStatus(env, status);
  return retval;
}
JNIEXPORT jdouble JNICALL Java_com_ctre_CanTalonJNI_GetCurrent
  (JNIEnv * env, jclass, jlong handle)
{
  double retval;
  CTR_Code status = ((CanTalonSRX*)handle)->GetCurrent(retval);
  CheckCTRStatus(env, status);
  return retval;
}
JNIEXPORT jint JNICALL Java_com_ctre_CanTalonJNI_GetBrakeIsEnabled
  (JNIEnv * env, jclass, jlong handle)
{
  int retval;
  CTR_Code status = ((CanTalonSRX*)handle)->GetBrakeIsEnabled(retval);
  CheckCTRStatus(env, status);
  return retval;
}
JNIEXPORT jint JNICALL Java_com_ctre_CanTalonJNI_GetEncPosition
  (JNIEnv * env, jclass, jlong handle)
{
  int retval;
  CTR_Code status = ((CanTalonSRX*)handle)->GetEncPosition(retval);
  CheckCTRStatus(env, status);
  return retval;
}
JNIEXPORT jint JNICALL Java_com_ctre_CanTalonJNI_GetEncVel
  (JNIEnv * env, jclass, jlong handle)
{
  int retval;
  CTR_Code status = ((CanTalonSRX*)handle)->GetEncVel(retval);
  CheckCTRStatus(env, status);
  return retval;
}
JNIEXPORT jint JNICALL Java_com_ctre_CanTalonJNI_GetEncIndexRiseEvents
  (JNIEnv * env, jclass, jlong handle)
{
  int retval;
  CTR_Code status = ((CanTalonSRX*)handle)->GetEncIndexRiseEvents(retval);
  CheckCTRStatus(env, status);
  return retval;
}
JNIEXPORT jint JNICALL Java_com_ctre_CanTalonJNI_GetQuadApin
  (JNIEnv * env, jclass, jlong handle)
{
  int retval;
  CTR_Code status = ((CanTalonSRX*)handle)->GetQuadApin(retval);
  CheckCTRStatus(env, status);
  return retval;
}
JNIEXPORT jint JNICALL Java_com_ctre_CanTalonJNI_GetQuadBpin
  (JNIEnv * env, jclass, jlong handle)
{
  int retval;
  CTR_Code status = ((CanTalonSRX*)handle)->GetQuadBpin(retval);
  CheckCTRStatus(env, status);
  return retval;
}
JNIEXPORT jint JNICALL Java_com_ctre_CanTalonJNI_GetQuadIdxpin
  (JNIEnv * env, jclass, jlong handle)
{
  int retval;
  CTR_Code status = ((CanTalonSRX*)handle)->GetQuadIdxpin(retval);
  CheckCTRStatus(env, status);
  return retval;
}
JNIEXPORT jint JNICALL Java_com_ctre_CanTalonJNI_GetAnalogInWithOv
  (JNIEnv * env, jclass, jlong handle)
{
  int retval;
  CTR_Code status = ((CanTalonSRX*)handle)->GetAnalogInWithOv(retval);
  CheckCTRStatus(env, status);
  return retval;
}
JNIEXPORT jint JNICALL Java_com_ctre_CanTalonJNI_GetAnalogInVel
  (JNIEnv * env, jclass, jlong handle)
{
  int retval;
  CTR_Code status = ((CanTalonSRX*)handle)->GetAnalogInVel(retval);
  CheckCTRStatus(env, status);
  return retval;
}
JNIEXPORT jdouble JNICALL Java_com_ctre_CanTalonJNI_GetTemp
  (JNIEnv * env, jclass, jlong handle)
{
  double retval;
  CTR_Code status = ((CanTalonSRX*)handle)->GetTemp(retval);
  CheckCTRStatus(env, status);
  return retval;
}
JNIEXPORT jdouble JNICALL Java_com_ctre_CanTalonJNI_GetBatteryV
  (JNIEnv * env, jclass, jlong handle)
{
  double retval;
  CTR_Code status = ((CanTalonSRX*)handle)->GetBatteryV(retval);
  CheckCTRStatus(env, status);
  return retval;
}
JNIEXPORT jint JNICALL Java_com_ctre_CanTalonJNI_GetResetCount
  (JNIEnv * env, jclass, jlong handle)
{
  int retval;
  CTR_Code status = ((CanTalonSRX*)handle)->GetResetCount(retval);
  CheckCTRStatus(env, status);
  return retval;
}
JNIEXPORT jint JNICALL Java_com_ctre_CanTalonJNI_GetResetFlags
  (JNIEnv * env, jclass, jlong handle)
{
  int retval;
  CTR_Code status = ((CanTalonSRX*)handle)->GetResetFlags(retval);
  CheckCTRStatus(env, status);
  return retval;
}
JNIEXPORT jint JNICALL Java_com_ctre_CanTalonJNI_GetFirmVers
  (JNIEnv * env, jclass, jlong handle)
{
  int retval;
  CTR_Code status = ((CanTalonSRX*)handle)->GetFirmVers(retval);
  CheckCTRStatus(env, status);
  return retval;
}
JNIEXPORT jint JNICALL Java_com_ctre_CanTalonJNI_GetPulseWidthPosition
  (JNIEnv * env, jclass, jlong handle)
{
  int retval;
  CTR_Code status = ((CanTalonSRX*)handle)->GetPulseWidthPosition(retval);
  CheckCTRStatus(env, status);
  return retval;
}
JNIEXPORT jint JNICALL Java_com_ctre_CanTalonJNI_GetPulseWidthVelocity
  (JNIEnv * env, jclass, jlong handle)
{
  int retval;
  CTR_Code status = ((CanTalonSRX*)handle)->GetPulseWidthVelocity(retval);
  CheckCTRStatus(env, status);
  return retval;
}
JNIEXPORT jint JNICALL Java_com_ctre_CanTalonJNI_GetPulseWidthRiseToRiseUs
  (JNIEnv * env, jclass, jlong handle)
{
  int retval;
  CTR_Code status = ((CanTalonSRX*)handle)->GetPulseWidthRiseToRiseUs(retval);
  CheckCTRStatus(env, status);
  return retval;
}
JNIEXPORT jint JNICALL Java_com_ctre_CanTalonJNI_GetActTraj_1IsValid
  (JNIEnv * env, jclass, jlong handle)
{
  int retval;
  CTR_Code status = ((CanTalonSRX*)handle)->GetActTraj_IsValid(retval);
  CheckCTRStatus(env, status);
  return retval;
}
JNIEXPORT jint JNICALL Java_com_ctre_CanTalonJNI_GetActTraj_1ProfileSlotSelect
  (JNIEnv * env, jclass, jlong handle)
{
  int retval;
  CTR_Code status = ((CanTalonSRX*)handle)->GetActTraj_ProfileSlotSelect(retval);
  CheckCTRStatus(env, status);
  return retval;
}
JNIEXPORT jint JNICALL Java_com_ctre_CanTalonJNI_GetActTraj_1VelOnly
  (JNIEnv * env, jclass, jlong handle)
{
  int retval;
  CTR_Code status = ((CanTalonSRX*)handle)->GetActTraj_VelOnly(retval);
  CheckCTRStatus(env, status);
  return retval;
}
JNIEXPORT jint JNICALL Java_com_ctre_CanTalonJNI_GetActTraj_1IsLast
  (JNIEnv * env, jclass, jlong handle)
{
  int retval;
  CTR_Code status = ((CanTalonSRX*)handle)->GetActTraj_IsLast(retval);
  CheckCTRStatus(env, status);
  return retval;
}
JNIEXPORT jint JNICALL Java_com_ctre_CanTalonJNI_GetOutputType
  (JNIEnv * env, jclass, jlong handle)
{
  int retval;
  CTR_Code status = ((CanTalonSRX*)handle)->GetOutputType(retval);
  CheckCTRStatus(env, status);
  return retval;
}
JNIEXPORT jint JNICALL Java_com_ctre_CanTalonJNI_GetHasUnderrun
  (JNIEnv * env, jclass, jlong handle)
{
  int retval;
  CTR_Code status = ((CanTalonSRX*)handle)->GetHasUnderrun(retval);
  CheckCTRStatus(env, status);
  return retval;
}
JNIEXPORT jint JNICALL Java_com_ctre_CanTalonJNI_GetIsUnderrun
  (JNIEnv * env, jclass, jlong handle)
{
  int retval;
  CTR_Code status = ((CanTalonSRX*)handle)->GetIsUnderrun(retval);
  CheckCTRStatus(env, status);
  return retval;
}
JNIEXPORT jint JNICALL Java_com_ctre_CanTalonJNI_GetNextID
  (JNIEnv * env, jclass, jlong handle)
{
  int retval;
  CTR_Code status = ((CanTalonSRX*)handle)->GetNextID(retval);
  CheckCTRStatus(env, status);
  return retval;
}
JNIEXPORT jint JNICALL Java_com_ctre_CanTalonJNI_GetBufferIsFull
  (JNIEnv * env, jclass, jlong handle)
{
  int retval;
  CTR_Code status = ((CanTalonSRX*)handle)->GetBufferIsFull(retval);
  CheckCTRStatus(env, status);
  return retval;
}
JNIEXPORT jint JNICALL Java_com_ctre_CanTalonJNI_GetCount
  (JNIEnv * env, jclass, jlong handle)
{
  int retval;
  CTR_Code status = ((CanTalonSRX*)handle)->GetCount(retval);
  CheckCTRStatus(env, status);
  return retval;
}
JNIEXPORT jint JNICALL Java_com_ctre_CanTalonJNI_GetActTraj_1Velocity
  (JNIEnv * env, jclass, jlong handle)
{
  int retval;
  CTR_Code status = ((CanTalonSRX*)handle)->GetActTraj_Velocity(retval);
  CheckCTRStatus(env, status);
  return retval;
}
JNIEXPORT jint JNICALL Java_com_ctre_CanTalonJNI_GetActTraj_1Position
  (JNIEnv * env, jclass, jlong handle)
{
  int retval;
  CTR_Code status = ((CanTalonSRX*)handle)->GetActTraj_Position(retval);
  CheckCTRStatus(env, status);
  return retval;
}
JNIEXPORT void JNICALL Java_com_ctre_CanTalonJNI_SetDemand
  (JNIEnv * env, jclass, jlong handle, jint param)
{
  CTR_Code status = ((CanTalonSRX*)handle)->SetDemand(param);
  CheckCTRStatus(env, status);
}
JNIEXPORT void JNICALL Java_com_ctre_CanTalonJNI_SetOverrideLimitSwitchEn
  (JNIEnv * env, jclass, jlong handle, jint param)
{
  CTR_Code status = ((CanTalonSRX*)handle)->SetOverrideLimitSwitchEn(param);
  CheckCTRStatus(env, status);
}
JNIEXPORT void JNICALL Java_com_ctre_CanTalonJNI_SetFeedbackDeviceSelect
  (JNIEnv * env, jclass, jlong handle, jint param)
{
  CTR_Code status = ((CanTalonSRX*)handle)->SetFeedbackDeviceSelect(param);
  CheckCTRStatus(env, status);
}
JNIEXPORT void JNICALL Java_com_ctre_CanTalonJNI_SetRevMotDuringCloseLoopEn
  (JNIEnv * env, jclass, jlong handle, jint param)
{
  CTR_Code status = ((CanTalonSRX*)handle)->SetRevMotDuringCloseLoopEn(param);
  CheckCTRStatus(env, status);
}
JNIEXPORT void JNICALL Java_com_ctre_CanTalonJNI_SetOverrideBrakeType
  (JNIEnv * env, jclass, jlong handle, jint param)
{
  CTR_Code status = ((CanTalonSRX*)handle)->SetOverrideBrakeType(param);
  CheckCTRStatus(env, status);
}
JNIEXPORT void JNICALL Java_com_ctre_CanTalonJNI_SetModeSelect
  (JNIEnv * env, jclass, jlong handle, jint param)
{
  CTR_Code status = ((CanTalonSRX*)handle)->SetModeSelect(param);
  CheckCTRStatus(env, status);
}
JNIEXPORT void JNICALL Java_com_ctre_CanTalonJNI_SetProfileSlotSelect
  (JNIEnv * env, jclass, jlong handle, jint param)
{
  CTR_Code status = ((CanTalonSRX*)handle)->SetProfileSlotSelect(param);
  CheckCTRStatus(env, status);
}
JNIEXPORT void JNICALL Java_com_ctre_CanTalonJNI_SetRampThrottle
  (JNIEnv * env, jclass, jlong handle, jint param)
{
  CTR_Code status = ((CanTalonSRX*)handle)->SetRampThrottle(param);
  CheckCTRStatus(env, status);
}
JNIEXPORT void JNICALL Java_com_ctre_CanTalonJNI_SetRevFeedbackSensor
  (JNIEnv * env, jclass, jlong handle, jint param)
{
  CTR_Code status = ((CanTalonSRX*)handle)->SetRevFeedbackSensor(param);
  CheckCTRStatus(env, status);
}
}  // extern "C"
