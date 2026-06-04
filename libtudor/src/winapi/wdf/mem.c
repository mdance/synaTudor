#include "internal.h"

static void mem_destr(struct wdf_memory *mem) {
    //Free memory
    wdf_cleanup_obj(&mem->object);
    if(mem->owns_data) free(mem->data);
    free(mem);
}
__winfnc NTSTATUS WdfMemoryCreatePreallocated(WDF_DRIVER_GLOBALS *globals, WDF_OBJECT_ATTRIBUTES *obj_attrs, void *buffer, size_t buffer_size, WDFOBJECT *out) {
    //Create the memory object
    struct wdf_memory *mem = (struct wdf_memory*) malloc(sizeof(struct wdf_memory));
    if(!mem) return winerr_from_errno();

    wdf_create_obj((struct wdf_object*) winwdf_get_driver(globals), &mem->object, (wdf_obj_destr_fnc*) mem_destr, obj_attrs);

    mem->data = buffer;
    mem->data_size = buffer_size;
    mem->owns_data = false;

    *out = &mem->object;
    return STATUS_SUCCESS;
}
WDFFUNC(WdfMemoryCreatePreallocated, 118)

//Internal helper: create a WDFMEMORY owning a copy of the given data, parented
//to `parent` (freed when the parent is destroyed). Returns NULL on OOM.
WDFOBJECT wdf_create_memory(struct wdf_object *parent, const void *data, size_t size) {
    struct wdf_memory *mem = (struct wdf_memory*) malloc(sizeof(struct wdf_memory));
    if(!mem) return NULL;

    wdf_create_obj(parent, &mem->object, (wdf_obj_destr_fnc*) mem_destr, NULL);

    mem->data = size ? malloc(size) : NULL;
    if(size) memcpy(mem->data, data, size);
    mem->data_size = size;
    mem->owns_data = true;

    return &mem->object;
}

__winfnc void *WdfMemoryGetBuffer(WDF_DRIVER_GLOBALS *globals, WDFOBJECT memory_obj, size_t *buffer_size) {
    struct wdf_memory *mem = (struct wdf_memory*) memory_obj;
    if(buffer_size) *buffer_size = mem->data_size;
    return mem->data;
}
WDFFUNC(WdfMemoryGetBuffer, 119)