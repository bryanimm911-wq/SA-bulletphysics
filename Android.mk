LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)
LOCAL_MODULE    := BulletPhysics64
LOCAL_SRC_FILES := ../main.cpp
LOCAL_C_INCLUDES := $(LOCAL_PATH)/..
LOCAL_CPPFLAGS  := -O2 -std=c++17 -DNDEBUG
LOCAL_LDLIBS    := -llog -ldl
include $(BUILD_SHARED_LIBRARY)
