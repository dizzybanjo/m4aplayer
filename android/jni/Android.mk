# https://developer.android.com/ndk/guides/android_mk.html

LOCAL_PATH := $(call my-dir)

# ------------------------------------------------------------------------------
# libpd.so
include $(CLEAR_VARS)
LOCAL_MODULE := pd
LOCAL_EXPORT_C_INCLUDES := $(LOCAL_PATH)/libs
LOCAL_SRC_FILES := $(LOCAL_PATH)/libs/$(TARGET_ARCH_ABI)/libpd.so
include $(PREBUILT_SHARED_LIBRARY)

# ------------------------------------------------------------------------------
# libm4aPlayer.so
include $(CLEAR_VARS)
LOCAL_MODULE := m4aPlayer
LOCAL_CFLAGS := -std=c11 -DNDEBUG -O3 -ffast-math
LOCAL_C_INCLUDES := $(LOCAL_PATH)/src
LOCAL_SRC_FILES := \
$(LOCAL_PATH)/src/m4aPlayer.c \
$(LOCAL_PATH)/src/HvLightPipe.c
LOCAL_LDLIBS := -llog -lOpenSLES
LOCAL_SHARED_LIBRARIES = pd
TARGET_PLATFORM := android-9
TARGET_ARCH_ABI := armeabi-v7a x86
LOCAL_ARM_NEON := true
LOCAL_ARM_MODE := arm
include $(BUILD_SHARED_LIBRARY)
