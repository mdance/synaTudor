#include "internal.h"

typedef ULONG __winfnc DEVICE_NOTIFY_CALLBACK_ROUTINE(void *Context, ULONG Type, void *setting);

typedef struct {
    DEVICE_NOTIFY_CALLBACK_ROUTINE *Callback;
    void *Context;
} DEVICE_NOTIFY_SUBSCRIBE_PARAMETERS;

__winfnc DWORD PowerRegisterSuspendResumeNotification(DWORD flags, DEVICE_NOTIFY_SUBSCRIBE_PARAMETERS *params, HANDLE *out) {
    //TODO
    *out = NULL;
    return ERROR_SUCCESS;
}
WINAPI(PowerRegisterSuspendResumeNotification)

__winfnc DWORD PowerUnregisterSuspendResumeNotification(HANDLE handle) {
    //TODO
    return ERROR_SUCCESS;
}
WINAPI(PowerUnregisterSuspendResumeNotification)

__winfnc DWORD PowerSettingRegisterNotification(const GUID *setting_guid, DWORD flags, HANDLE recipient, HANDLE *out) {
    if(out) *out = NULL;
    return ERROR_SUCCESS;
}
WINAPI(PowerSettingRegisterNotification)

__winfnc DWORD PowerSettingUnregisterNotification(HANDLE handle) {
    return ERROR_SUCCESS;
}
WINAPI(PowerSettingUnregisterNotification)