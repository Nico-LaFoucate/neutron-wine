/*
 * amfrt64.dll: Neutron's AMD AMF runtime, the Windows-facing half.
 *
 * On Windows with an AMD GPU, Premiere Pro, Media Encoder and After Effects encode H.264 and HEVC
 * through AMD's AMF runtime, amfrt64.dll. This winelib DLL answers the AMF calls those apps make and
 * hands the frames to vkenc (vkenc.h), which encodes them on the host through Mesa's Vulkan Video
 * driver or, where that hides its encoder, Mesa's VA-API driver.
 *
 * The AMF interfaces are C++ classes and Adobe's code is built with MSVC, so every object here is laid
 * out the way MSVC lays out those classes: a vtable pointer first, then the slots in declaration order,
 * base class first. Two MSVC rules differ from the SDK's C vtables:
 *   - overloaded virtuals are placed in reverse declaration order, so
 *     AMFPropertyStorageEx::GetPropertyInfo(name) is at +0x70 and GetPropertyInfo(index) at +0x78;
 *   - a virtual destructor takes one slot, so AMFPropertyInfo has a vptr and its maxValue.int64Value
 *     is at +0x58.
 * Every slot Adobe calls is checked with a C_ASSERT next to its vtable. An AMFVariantStruct passed by
 * value (SetProperty, ValidateProperty) arrives as a pointer to the caller's copy (MSVC x64 ABI).
 *
 * Copyright 2026 the Neutron project. LGPL-2.1-or-later.
 */

#include <dirent.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "windef.h"
#include "winbase.h"
#include "objbase.h"
#include "wine/debug.h"

#include "vkenc_backend.h"

WINE_DEFAULT_DEBUG_CHANNEL(amf);

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#endif

/* -------------------------------------------------------------------------------------------------
 * AMF constants (values from the AMF SDK headers, MIT)
 */

#define AMF_MAKE_VERSION(major, minor, release, build) \
    (((UINT64)(major) << 48) | ((UINT64)(minor) << 32) | ((UINT64)(release) << 16) | (UINT64)(build))
#define AMF_VERSION_MAJOR(v) ((unsigned int)((v) >> 48) & 0xffff)
#define AMF_VERSION_MINOR(v) ((unsigned int)((v) >> 32) & 0xffff)
#define AMF_VERSION_RELEASE(v) ((unsigned int)((v) >> 16) & 0xffff)
#define AMF_VERSION_BUILD(v) ((unsigned int)(v) & 0xffff)

/* The SDK whose layouts this file implements. */
#define AMF_RUNTIME_VERSION AMF_MAKE_VERSION(1, 5, 2, 0)

typedef INT32 AMF_RESULT;

#define AMF_RESULT_LIST(X) \
    X(AMF_OK) X(AMF_FAIL) X(AMF_UNEXPECTED) X(AMF_ACCESS_DENIED) X(AMF_INVALID_ARG) X(AMF_OUT_OF_RANGE) \
    X(AMF_OUT_OF_MEMORY) X(AMF_INVALID_POINTER) X(AMF_NO_INTERFACE) X(AMF_NOT_IMPLEMENTED) \
    X(AMF_NOT_SUPPORTED) X(AMF_NOT_FOUND) X(AMF_ALREADY_INITIALIZED) X(AMF_NOT_INITIALIZED) \
    X(AMF_INVALID_FORMAT) X(AMF_WRONG_STATE) X(AMF_FILE_NOT_OPEN) X(AMF_NO_DEVICE) X(AMF_DIRECTX_FAILED) \
    X(AMF_OPENCL_FAILED) X(AMF_GLX_FAILED) X(AMF_XV_FAILED) X(AMF_ALSA_FAILED) X(AMF_EOF) X(AMF_REPEAT) \
    X(AMF_INPUT_FULL) X(AMF_RESOLUTION_CHANGED) X(AMF_RESOLUTION_UPDATED) X(AMF_INVALID_DATA_TYPE) \
    X(AMF_INVALID_RESOLUTION) X(AMF_CODEC_NOT_SUPPORTED) X(AMF_SURFACE_FORMAT_NOT_SUPPORTED) \
    X(AMF_SURFACE_MUST_BE_SHARED) X(AMF_DECODER_NOT_PRESENT) X(AMF_DECODER_SURFACE_ALLOCATION_FAILED) \
    X(AMF_DECODER_NO_FREE_SURFACES) X(AMF_ENCODER_NOT_PRESENT) X(AMF_DEM_ERROR) X(AMF_DEM_PROPERTY_READONLY) \
    X(AMF_DEM_REMOTE_DISPLAY_CREATE_FAILED) X(AMF_DEM_START_ENCODING_FAILED) X(AMF_DEM_QUERY_OUTPUT_FAILED) \
    X(AMF_TAN_CLIPPING_WAS_REQUIRED) X(AMF_TAN_UNSUPPORTED_VERSION) X(AMF_NEED_MORE_INPUT) \
    X(AMF_VULKAN_FAILED)

enum amf_result_code
{
#define X(name) name,
    AMF_RESULT_LIST(X)
#undef X
    AMF_RESULT_COUNT
};
C_ASSERT(AMF_EOF == 23);
C_ASSERT(AMF_INPUT_FULL == 25);
C_ASSERT(AMF_ENCODER_NOT_PRESENT == 36);
C_ASSERT(AMF_NEED_MORE_INPUT == 44);

static const WCHAR *const result_text[] =
{
#define X(name) L"" #name,
    AMF_RESULT_LIST(X)
#undef X
};

enum
{
    AMF_VARIANT_EMPTY = 0, AMF_VARIANT_BOOL = 1, AMF_VARIANT_INT64 = 2, AMF_VARIANT_DOUBLE = 3,
    AMF_VARIANT_RECT = 4, AMF_VARIANT_SIZE = 5, AMF_VARIANT_POINT = 6, AMF_VARIANT_RATE = 7,
    AMF_VARIANT_RATIO = 8, AMF_VARIANT_COLOR = 9, AMF_VARIANT_STRING = 10, AMF_VARIANT_WSTRING = 11,
    AMF_VARIANT_INTERFACE = 12, AMF_VARIANT_FLOAT = 13,
};

enum
{
    AMF_MEMORY_UNKNOWN = 0, AMF_MEMORY_HOST = 1, AMF_MEMORY_DX9 = 2, AMF_MEMORY_DX11 = 3,
    AMF_MEMORY_OPENCL = 4, AMF_MEMORY_OPENGL = 5, AMF_MEMORY_XV = 6, AMF_MEMORY_GRALLOC = 7,
    AMF_MEMORY_COMPUTE_FOR_DX9 = 8, AMF_MEMORY_COMPUTE_FOR_DX11 = 9, AMF_MEMORY_VULKAN = 10,
    AMF_MEMORY_DX12 = 11, AMF_MEMORY_LAST
};

enum { AMF_DATA_BUFFER = 0, AMF_DATA_SURFACE = 1 };
enum { AMF_SURFACE_UNKNOWN = 0, AMF_SURFACE_NV12 = 1, AMF_SURFACE_P010 = 10 };
enum { AMF_PLANE_Y = 2, AMF_PLANE_UV = 3 };
enum { AMF_FRAME_PROGRESSIVE = 0 };
enum { AMF_ACCEL_HARDWARE = 0 };
enum { AMF_PROPERTY_CONTENT_DEFAULT = 0 };
enum { AMF_PROPERTY_ACCESS_FULL = 0xff };

/* Picture types for ForcePictureType / HevcForcePictureType, and OutputDataType values. */
enum { AMF_PICTURE_TYPE_IDR = 2 };
enum { AMF_OUTPUT_DATA_TYPE_IDR = 0, AMF_OUTPUT_DATA_TYPE_P = 2 };

/* AMF_VIDEO_CONVERTER_COLOR_PROFILE_ENUM */
enum
{
    AMF_COLOR_PROFILE_UNKNOWN = -1, AMF_COLOR_PROFILE_601 = 0, AMF_COLOR_PROFILE_709 = 1,
    AMF_COLOR_PROFILE_2020 = 2, AMF_COLOR_PROFILE_FULL_601 = 3, AMF_COLOR_PROFILE_FULL_709 = 7,
    AMF_COLOR_PROFILE_FULL_2020 = 8,
};

static const GUID amf_iid_interface = {0x9d872f34, 0x90dc, 0x4b93, {0xb6, 0xb2, 0x6c, 0xa3, 0x7c, 0x85, 0x25, 0xdb}};
static const GUID amf_iid_property_storage = {0xc7cec05b, 0xcfb9, 0x48af, {0xac, 0xe3, 0xf6, 0x8d, 0xf8, 0x39, 0x5f, 0xe3}};
static const GUID amf_iid_property_storage_ex = {0x16b8958d, 0xe943, 0x4a33, {0xa3, 0x5a, 0x88, 0x5a, 0xd8, 0x28, 0xf2, 0x67}};
static const GUID amf_iid_data = {0xa1159bf6, 0x9104, 0x4107, {0x8e, 0xaa, 0xc5, 0x3d, 0x5d, 0xba, 0xc5, 0x11}};
static const GUID amf_iid_buffer = {0xb04b7248, 0xb6f0, 0x4321, {0xb6, 0x91, 0xba, 0xa4, 0x74, 0x0f, 0x9f, 0xcb}};
static const GUID amf_iid_surface = {0x3075dbe3, 0x8718, 0x4cfa, {0x86, 0xfb, 0x21, 0x14, 0xc0, 0xa5, 0xa4, 0x51}};
static const GUID amf_iid_plane = {0xbede1aa6, 0xd8fa, 0x4625, {0x94, 0x65, 0x6c, 0x82, 0xc4, 0x37, 0x71, 0x2e}};
static const GUID amf_iid_context = {0xa76a13f0, 0xd80e, 0x4fcc, {0xb5, 0x08, 0x65, 0xd0, 0xb5, 0x2e, 0xd9, 0xee}};
static const GUID amf_iid_component = {0x8b51e5e4, 0x455d, 0x4034, {0xa7, 0x46, 0xde, 0x1b, 0xed, 0xc3, 0xc4, 0x06}};

/* -------------------------------------------------------------------------------------------------
 * AMF structures and vtable slot layouts (MSVC x64)
 */

typedef struct amf_variant
{
    INT32 type;
    union
    {
        BYTE boolValue;
        INT64 int64Value;
        double doubleValue;
        float floatValue;
        char *stringValue;
        WCHAR *wstringValue;
        void *pInterface;
        struct { INT32 a, b; } pair;    /* AMFSize, AMFPoint */
        struct { UINT32 num, den; } rate;  /* AMFRate, AMFRatio */
        BYTE raw[16];
    } u;
} amf_variant;
C_ASSERT(sizeof(amf_variant) == 24);
C_ASSERT(offsetof(amf_variant, u) == 8);

typedef struct amf_property_info amf_property_info;

struct property_info_vtbl
{
    /* MSVC: one slot for the virtual destructor (the scalar deleting destructor) */
    void *(WINAPI *destructor)(amf_property_info *info, unsigned int flags);
};

/* The C++ AMFPropertyInfo, which has a virtual destructor. */
struct amf_property_info
{
    const struct property_info_vtbl *vtbl;
    const WCHAR *name;
    const WCHAR *desc;
    INT32 type;
    INT32 content_type;
    amf_variant default_value;
    amf_variant min_value;
    amf_variant max_value;
    INT32 access_type;
    const void *enum_description;
};
C_ASSERT(offsetof(amf_property_info, name) == 0x08);
C_ASSERT(offsetof(amf_property_info, max_value) == 0x50);
C_ASSERT(offsetof(amf_property_info, max_value.u.int64Value) == 0x58);

#define AMF_INTERFACE_SLOTS \
    INT32      (WINAPI *Acquire)(void *iface); \
    INT32      (WINAPI *Release)(void *iface); \
    AMF_RESULT (WINAPI *QueryInterface)(void *iface, const GUID *iid, void **out);

#define AMF_PROPERTY_STORAGE_SLOTS \
    AMF_RESULT (WINAPI *SetProperty)(void *iface, const WCHAR *name, const amf_variant *value); \
    AMF_RESULT (WINAPI *GetProperty)(void *iface, const WCHAR *name, amf_variant *value); \
    BYTE       (WINAPI *HasProperty)(void *iface, const WCHAR *name); \
    SIZE_T     (WINAPI *GetPropertyCount)(void *iface); \
    AMF_RESULT (WINAPI *GetPropertyAt)(void *iface, SIZE_T index, WCHAR *name, SIZE_T name_size, \
                                       amf_variant *value); \
    AMF_RESULT (WINAPI *Clear)(void *iface); \
    AMF_RESULT (WINAPI *AddTo)(void *iface, void *dest, BYTE overwrite, BYTE deep); \
    AMF_RESULT (WINAPI *CopyTo)(void *iface, void *dest, BYTE deep); \
    void       (WINAPI *AddObserver)(void *iface, void *observer); \
    void       (WINAPI *RemoveObserver)(void *iface, void *observer);

#define AMF_DATA_SLOTS \
    INT32      (WINAPI *GetMemoryType)(void *iface); \
    AMF_RESULT (WINAPI *Duplicate)(void *iface, INT32 type, void **out); \
    AMF_RESULT (WINAPI *Convert)(void *iface, INT32 type); \
    AMF_RESULT (WINAPI *Interop)(void *iface, INT32 type); \
    INT32      (WINAPI *GetDataType)(void *iface); \
    BYTE       (WINAPI *IsReusable)(void *iface); \
    void       (WINAPI *SetPts)(void *iface, INT64 pts); \
    INT64      (WINAPI *GetPts)(void *iface); \
    void       (WINAPI *SetDuration)(void *iface, INT64 duration); \
    INT64      (WINAPI *GetDuration)(void *iface);

struct amf_interface_vtbl
{
    AMF_INTERFACE_SLOTS
};

struct amf_interface
{
    const struct amf_interface_vtbl *vtbl;
};

struct property_storage_vtbl
{
    AMF_INTERFACE_SLOTS
    AMF_PROPERTY_STORAGE_SLOTS
};
C_ASSERT(offsetof(struct property_storage_vtbl, SetProperty) == 0x18);
C_ASSERT(sizeof(struct property_storage_vtbl) == 0x68);

struct property_storage
{
    const struct property_storage_vtbl *vtbl;
};

/* Observers are C++ objects without AMFInterface: the callback is slot 0. */
struct property_observer_vtbl
{
    void (WINAPI *OnPropertyChanged)(void *iface, const WCHAR *name);
};

struct surface_observer_vtbl
{
    void (WINAPI *OnSurfaceDataRelease)(void *iface, void *surface);
};

struct buffer_observer_vtbl
{
    void (WINAPI *OnBufferDataRelease)(void *iface, void *buffer);
};

struct amf_observer
{
    const void *vtbl;
};

/* -------------------------------------------------------------------------------------------------
 * Helpers
 */

static int amf_wcscmp(const WCHAR *a, const WCHAR *b)
{
    while (*a && *a == *b)
    {
        a++;
        b++;
    }
    return (int)*a - (int)*b;
}

static SIZE_T amf_wcslen(const WCHAR *s)
{
    const WCHAR *p = s;

    while (*p) p++;
    return p - s;
}

static WCHAR *amf_wcsdup_internal(const WCHAR *s)
{
    SIZE_T size = (amf_wcslen(s) + 1) * sizeof(WCHAR);
    WCHAR *ret;

    if ((ret = malloc(size))) memcpy(ret, s, size);
    return ret;
}

static void interface_acquire(void *iface)
{
    if (iface) ((struct amf_interface *)iface)->vtbl->Acquire(iface);
}

static void interface_release(void *iface)
{
    if (iface) ((struct amf_interface *)iface)->vtbl->Release(iface);
}

static const char *debugstr_amf_variant(const amf_variant *v)
{
    if (!v) return "(null)";
    switch (v->type)
    {
    case AMF_VARIANT_EMPTY: return "EMPTY";
    case AMF_VARIANT_BOOL: return wine_dbg_sprintf("BOOL %u", v->u.boolValue);
    case AMF_VARIANT_INT64: return wine_dbg_sprintf("INT64 %lld", (long long)v->u.int64Value);
    case AMF_VARIANT_DOUBLE: return wine_dbg_sprintf("DOUBLE %g", v->u.doubleValue);
    case AMF_VARIANT_FLOAT: return wine_dbg_sprintf("FLOAT %g", (double)v->u.floatValue);
    case AMF_VARIANT_SIZE: return wine_dbg_sprintf("SIZE {%d, %d}", v->u.pair.a, v->u.pair.b);
    case AMF_VARIANT_POINT: return wine_dbg_sprintf("POINT {%d, %d}", v->u.pair.a, v->u.pair.b);
    case AMF_VARIANT_RATE: return wine_dbg_sprintf("RATE {%u, %u}", v->u.rate.num, v->u.rate.den);
    case AMF_VARIANT_RATIO: return wine_dbg_sprintf("RATIO {%u, %u}", v->u.rate.num, v->u.rate.den);
    case AMF_VARIANT_STRING: return wine_dbg_sprintf("STRING %s", debugstr_a(v->u.stringValue));
    case AMF_VARIANT_WSTRING: return wine_dbg_sprintf("WSTRING %s", debugstr_w(v->u.wstringValue));
    case AMF_VARIANT_INTERFACE: return wine_dbg_sprintf("INTERFACE %p", v->u.pInterface);
    default: return wine_dbg_sprintf("type %d", v->type);
    }
}

static void variant_clear(amf_variant *v)
{
    switch (v->type)
    {
    case AMF_VARIANT_STRING: CoTaskMemFree(v->u.stringValue); break;
    case AMF_VARIANT_WSTRING: CoTaskMemFree(v->u.wstringValue); break;
    case AMF_VARIANT_INTERFACE: interface_release(v->u.pInterface); break;
    }
    memset(v, 0, sizeof(*v));
}

/* A deep copy, as the SDK's AMFVariantCopy: strings are duplicated with CoTaskMemAlloc (so a caller can
 * free what GetProperty returns with AMFVariantClear) and interfaces are acquired. */
static AMF_RESULT variant_copy(amf_variant *dst, const amf_variant *src)
{
    *dst = *src;
    switch (src->type)
    {
    case AMF_VARIANT_STRING:
        if (src->u.stringValue)
        {
            SIZE_T size = strlen(src->u.stringValue) + 1;

            if (!(dst->u.stringValue = CoTaskMemAlloc(size)))
            {
                dst->type = AMF_VARIANT_EMPTY;
                return AMF_OUT_OF_MEMORY;
            }
            memcpy(dst->u.stringValue, src->u.stringValue, size);
        }
        break;
    case AMF_VARIANT_WSTRING:
        if (src->u.wstringValue)
        {
            SIZE_T size = (amf_wcslen(src->u.wstringValue) + 1) * sizeof(WCHAR);

            if (!(dst->u.wstringValue = CoTaskMemAlloc(size)))
            {
                dst->type = AMF_VARIANT_EMPTY;
                return AMF_OUT_OF_MEMORY;
            }
            memcpy(dst->u.wstringValue, src->u.wstringValue, size);
        }
        break;
    case AMF_VARIANT_INTERFACE:
        interface_acquire(src->u.pInterface);
        break;
    }
    return AMF_OK;
}

/* -------------------------------------------------------------------------------------------------
 * Property storage, shared by every object that is an AMFPropertyStorage
 */

struct property
{
    WCHAR *name;
    amf_variant value;
};

struct observer_list
{
    void *items[8];
    unsigned int count;
};

struct property_store
{
    SRWLOCK lock;   /* also guards the object's other observer lists */
    struct property *props;
    SIZE_T count, size;
    struct observer_list observers;     /* AMFPropertyStorageObserver */
};

static BOOL observer_add(struct observer_list *list, void *observer)
{
    unsigned int i;

    for (i = 0; i < list->count; i++)
        if (list->items[i] == observer) return TRUE;
    if (list->count == ARRAY_SIZE(list->items)) return FALSE;
    list->items[list->count++] = observer;
    return TRUE;
}

static void observer_remove(struct observer_list *list, void *observer)
{
    unsigned int i;

    for (i = 0; i < list->count; i++)
    {
        if (list->items[i] != observer) continue;
        memmove(&list->items[i], &list->items[i + 1], (list->count - i - 1) * sizeof(list->items[0]));
        list->count--;
        return;
    }
}

static struct property *store_find(struct property_store *store, const WCHAR *name)
{
    SIZE_T i;

    for (i = 0; i < store->count; i++)
        if (!amf_wcscmp(store->props[i].name, name)) return &store->props[i];
    return NULL;
}

static AMF_RESULT store_set(struct property_store *store, const WCHAR *name, const amf_variant *value)
{
    struct observer_list observers;
    struct property *prop;
    amf_variant copy;
    AMF_RESULT ret;
    unsigned int i;

    if ((ret = variant_copy(&copy, value))) return ret;

    AcquireSRWLockExclusive(&store->lock);
    if ((prop = store_find(store, name)))
    {
        variant_clear(&prop->value);
        prop->value = copy;
    }
    else
    {
        if (store->count == store->size)
        {
            SIZE_T size = store->size ? store->size * 2 : 16;
            struct property *props = realloc(store->props, size * sizeof(*props));

            if (!props) goto oom;
            store->props = props;
            store->size = size;
        }
        if (!(store->props[store->count].name = amf_wcsdup_internal(name))) goto oom;
        store->props[store->count++].value = copy;
    }
    observers = store->observers;
    ReleaseSRWLockExclusive(&store->lock);

    for (i = 0; i < observers.count; i++)
    {
        struct amf_observer *observer = observers.items[i];
        ((const struct property_observer_vtbl *)observer->vtbl)->OnPropertyChanged(observer, name);
    }
    return AMF_OK;

oom:
    ReleaseSRWLockExclusive(&store->lock);
    variant_clear(&copy);
    return AMF_OUT_OF_MEMORY;
}

static AMF_RESULT store_get(struct property_store *store, const WCHAR *name, amf_variant *value)
{
    struct property *prop;
    AMF_RESULT ret;

    AcquireSRWLockExclusive(&store->lock);
    if ((prop = store_find(store, name))) ret = variant_copy(value, &prop->value);
    else ret = AMF_NOT_FOUND;
    ReleaseSRWLockExclusive(&store->lock);
    return ret;
}

/* For the bridge's own reads of scalar properties: a shallow copy, never a string or interface. */
static BOOL store_get_scalar(struct property_store *store, const WCHAR *name, amf_variant *value)
{
    struct property *prop;
    BOOL ret = FALSE;

    if (!name) return FALSE;
    AcquireSRWLockExclusive(&store->lock);
    if ((prop = store_find(store, name)) && prop->value.type != AMF_VARIANT_STRING
            && prop->value.type != AMF_VARIANT_WSTRING && prop->value.type != AMF_VARIANT_INTERFACE)
    {
        *value = prop->value;
        ret = TRUE;
    }
    ReleaseSRWLockExclusive(&store->lock);
    return ret;
}

static BOOL store_get_int(struct property_store *store, const WCHAR *name, INT64 *out)
{
    amf_variant v;

    if (!store_get_scalar(store, name, &v)) return FALSE;
    switch (v.type)
    {
    case AMF_VARIANT_INT64: *out = v.u.int64Value; return TRUE;
    case AMF_VARIANT_BOOL: *out = !!v.u.boolValue; return TRUE;
    case AMF_VARIANT_DOUBLE: *out = (INT64)v.u.doubleValue; return TRUE;
    case AMF_VARIANT_FLOAT: *out = (INT64)v.u.floatValue; return TRUE;
    default:
        WARN("%s has type %d, expected a number\n", debugstr_w(name), v.type);
        return FALSE;
    }
}

static INT64 store_int(struct property_store *store, const WCHAR *name, INT64 def)
{
    INT64 value;

    return store_get_int(store, name, &value) ? value : def;
}

static BOOL store_get_pair(struct property_store *store, const WCHAR *name, UINT32 *a, UINT32 *b)
{
    amf_variant v;

    if (!store_get_scalar(store, name, &v)) return FALSE;
    if (v.type != AMF_VARIANT_SIZE && v.type != AMF_VARIANT_POINT && v.type != AMF_VARIANT_RATE
            && v.type != AMF_VARIANT_RATIO)
    {
        WARN("%s has type %d, expected a pair\n", debugstr_w(name), v.type);
        return FALSE;
    }
    *a = v.u.rate.num;
    *b = v.u.rate.den;
    return TRUE;
}

static void store_set_int(struct property_store *store, const WCHAR *name, INT64 value)
{
    amf_variant v = {AMF_VARIANT_INT64};

    v.u.int64Value = value;
    store_set(store, name, &v);
}

static void store_set_bool(struct property_store *store, const WCHAR *name, BOOL value)
{
    amf_variant v = {AMF_VARIANT_BOOL};

    v.u.boolValue = !!value;
    store_set(store, name, &v);
}

static void snapshot_free(struct property *props, SIZE_T count)
{
    SIZE_T i;

    for (i = 0; i < count; i++)
    {
        free(props[i].name);
        variant_clear(&props[i].value);
    }
    free(props);
}

/* A deep copy of every property, so a destination storage can be called without holding our lock. */
static AMF_RESULT store_snapshot(struct property_store *store, struct property **out, SIZE_T *count)
{
    struct property *props = NULL;
    AMF_RESULT ret = AMF_OK;
    SIZE_T i, n = 0;

    AcquireSRWLockExclusive(&store->lock);
    if (store->count && !(props = calloc(store->count, sizeof(*props)))) ret = AMF_OUT_OF_MEMORY;
    for (i = 0; !ret && i < store->count; i++, n++)
    {
        if (!(props[i].name = amf_wcsdup_internal(store->props[i].name))) ret = AMF_OUT_OF_MEMORY;
        else if ((ret = variant_copy(&props[i].value, &store->props[i].value))) free(props[i].name);
    }
    ReleaseSRWLockExclusive(&store->lock);

    if (ret)
    {
        snapshot_free(props, n);
        return ret;
    }
    *out = props;
    *count = n;
    return AMF_OK;
}

static void store_clear(struct property_store *store)
{
    struct property *props;
    SIZE_T count;

    AcquireSRWLockExclusive(&store->lock);
    props = store->props;
    count = store->count;
    store->props = NULL;
    store->count = store->size = 0;
    ReleaseSRWLockExclusive(&store->lock);
    snapshot_free(props, count);
}

/* -------------------------------------------------------------------------------------------------
 * Generic object: AMFInterface + AMFPropertyStorage
 */

struct amf_object;

struct object_ops
{
    const char *name;
    const GUID *const *iids;    /* NULL-terminated */
    void (*destroy)(struct amf_object *object);
};

struct amf_object
{
    const void *vtbl;
    LONG refcount;
    const struct object_ops *ops;
    struct property_store store;
};

static void object_init(struct amf_object *object, const void *vtbl, const struct object_ops *ops)
{
    object->vtbl = vtbl;
    object->refcount = 1;
    object->ops = ops;
    InitializeSRWLock(&object->store.lock);
}

static INT32 WINAPI object_Acquire(void *iface)
{
    struct amf_object *object = iface;
    LONG refcount = InterlockedIncrement(&object->refcount);

    TRACE("%s %p, refcount %d\n", object->ops->name, object, (int)refcount);
    return refcount;
}

static INT32 WINAPI object_Release(void *iface)
{
    struct amf_object *object = iface;
    LONG refcount = InterlockedDecrement(&object->refcount);

    TRACE("%s %p, refcount %d\n", object->ops->name, object, (int)refcount);
    if (!refcount)
    {
        if (object->ops->destroy) object->ops->destroy(object);
        store_clear(&object->store);
        free(object);
    }
    return refcount;
}

static AMF_RESULT WINAPI object_QueryInterface(void *iface, const GUID *iid, void **out)
{
    struct amf_object *object = iface;
    const GUID *const *p;

    TRACE("%s %p, iid %s, out %p\n", object->ops->name, object, debugstr_guid(iid), out);
    if (!out) return AMF_INVALID_POINTER;
    *out = NULL;
    if (!iid) return AMF_INVALID_POINTER;
    for (p = object->ops->iids; *p; p++)
    {
        if (!IsEqualGUID(*p, iid)) continue;
        object_Acquire(object);
        *out = object;
        return AMF_OK;
    }
    WARN("%s %p: no interface %s\n", object->ops->name, object, debugstr_guid(iid));
    return AMF_NO_INTERFACE;
}

static AMF_RESULT WINAPI object_SetProperty(void *iface, const WCHAR *name, const amf_variant *value)
{
    struct amf_object *object = iface;

    TRACE("%s %p, %s = %s\n", object->ops->name, object, debugstr_w(name), debugstr_amf_variant(value));
    if (!name || !value) return AMF_INVALID_POINTER;
    return store_set(&object->store, name, value);
}

static AMF_RESULT WINAPI object_GetProperty(void *iface, const WCHAR *name, amf_variant *value)
{
    struct amf_object *object = iface;
    AMF_RESULT ret;

    if (!name || !value) return AMF_INVALID_POINTER;
    ret = store_get(&object->store, name, value);
    TRACE("%s %p, %s -> %d %s\n", object->ops->name, object, debugstr_w(name), ret,
          ret ? "" : debugstr_amf_variant(value));
    return ret;
}

static BYTE WINAPI object_HasProperty(void *iface, const WCHAR *name)
{
    struct amf_object *object = iface;
    BOOL ret;

    if (!name) return FALSE;
    AcquireSRWLockExclusive(&object->store.lock);
    ret = store_find(&object->store, name) != NULL;
    ReleaseSRWLockExclusive(&object->store.lock);
    TRACE("%s %p, %s -> %d\n", object->ops->name, object, debugstr_w(name), ret);
    return ret;
}

static SIZE_T WINAPI object_GetPropertyCount(void *iface)
{
    struct amf_object *object = iface;
    SIZE_T count;

    AcquireSRWLockExclusive(&object->store.lock);
    count = object->store.count;
    ReleaseSRWLockExclusive(&object->store.lock);
    TRACE("%s %p -> %u\n", object->ops->name, object, (unsigned int)count);
    return count;
}

static AMF_RESULT WINAPI object_GetPropertyAt(void *iface, SIZE_T index, WCHAR *name, SIZE_T name_size,
                                             amf_variant *value)
{
    struct amf_object *object = iface;
    AMF_RESULT ret = AMF_OK;

    TRACE("%s %p, index %u, name %p, size %u, value %p\n", object->ops->name, object, (unsigned int)index,
          name, (unsigned int)name_size, value);
    if (!name || !value) return AMF_INVALID_POINTER;
    AcquireSRWLockExclusive(&object->store.lock);
    if (index >= object->store.count) ret = AMF_INVALID_ARG;
    else
    {
        const struct property *prop = &object->store.props[index];
        SIZE_T len = amf_wcslen(prop->name);

        if (len >= name_size) ret = AMF_INVALID_ARG;
        else
        {
            memcpy(name, prop->name, (len + 1) * sizeof(WCHAR));
            ret = variant_copy(value, &prop->value);
        }
    }
    ReleaseSRWLockExclusive(&object->store.lock);
    return ret;
}

static AMF_RESULT WINAPI object_Clear(void *iface)
{
    struct amf_object *object = iface;

    TRACE("%s %p\n", object->ops->name, object);
    store_clear(&object->store);
    return AMF_OK;
}

static AMF_RESULT WINAPI object_AddTo(void *iface, void *dest, BYTE overwrite, BYTE deep)
{
    struct amf_object *object = iface;
    struct property_storage *target = dest;
    struct property *props;
    AMF_RESULT ret;
    SIZE_T i, count;

    TRACE("%s %p, dest %p, overwrite %u, deep %u\n", object->ops->name, object, dest, overwrite, deep);
    if (!dest) return AMF_INVALID_POINTER;
    if (dest == iface) return AMF_OK;
    if ((ret = store_snapshot(&object->store, &props, &count))) return ret;
    for (i = 0; i < count && !ret; i++)
    {
        if (!(overwrite & 1) && target->vtbl->HasProperty(dest, props[i].name)) continue;
        ret = target->vtbl->SetProperty(dest, props[i].name, &props[i].value);
    }
    snapshot_free(props, count);
    return ret;
}

static AMF_RESULT WINAPI object_CopyTo(void *iface, void *dest, BYTE deep)
{
    struct property_storage *target = dest;
    AMF_RESULT ret;

    TRACE("%p, dest %p, deep %u\n", iface, dest, deep);
    if (!dest) return AMF_INVALID_POINTER;
    if (dest == iface) return AMF_OK;
    if ((ret = target->vtbl->Clear(dest))) return ret;
    return object_AddTo(iface, dest, TRUE, deep);
}

static void WINAPI object_AddObserver(void *iface, void *observer)
{
    struct amf_object *object = iface;

    TRACE("%s %p, observer %p\n", object->ops->name, object, observer);
    if (!observer) return;
    AcquireSRWLockExclusive(&object->store.lock);
    if (!observer_add(&object->store.observers, observer)) FIXME("too many property observers\n");
    ReleaseSRWLockExclusive(&object->store.lock);
}

static void WINAPI object_RemoveObserver(void *iface, void *observer)
{
    struct amf_object *object = iface;

    TRACE("%s %p, observer %p\n", object->ops->name, object, observer);
    AcquireSRWLockExclusive(&object->store.lock);
    observer_remove(&object->store.observers, observer);
    ReleaseSRWLockExclusive(&object->store.lock);
}

#define AMF_INTERFACE_IMPL \
    .Acquire = object_Acquire, \
    .Release = object_Release, \
    .QueryInterface = object_QueryInterface,

#define AMF_PROPERTY_STORAGE_IMPL \
    .GetProperty = object_GetProperty, \
    .HasProperty = object_HasProperty, \
    .GetPropertyCount = object_GetPropertyCount, \
    .GetPropertyAt = object_GetPropertyAt, \
    .Clear = object_Clear, \
    .AddTo = object_AddTo, \
    .CopyTo = object_CopyTo, \
    .AddObserver = object_AddObserver, \
    .RemoveObserver = object_RemoveObserver,

/* -------------------------------------------------------------------------------------------------
 * AMFSurface and AMFPlane: host-memory NV12 / P010 frames
 */

struct amf_surface;

struct amf_plane
{
    const struct amf_plane_vtbl *vtbl;
    struct amf_surface *surface;
    unsigned int index;     /* 0 luma, 1 interleaved chroma */
};

struct amf_surface
{
    struct amf_object obj;
    struct amf_plane planes[2];
    INT32 format, width, height;
    INT32 memory_type;
    const BYTE *host;       /* the caller's memory, until a Convert copies it */
    INT32 host_pitch, host_vpitch;
    BYTE *own;              /* our copy, chroma at own + own_pitch * height */
    INT32 own_pitch;
    INT64 pts, duration;
    INT32 frame_type;
    INT32 crop_x, crop_y, crop_width, crop_height;
    struct observer_list observers;     /* AMFSurfaceObserver, guarded by obj.store.lock */
};

struct amf_plane_vtbl
{
    AMF_INTERFACE_SLOTS
    INT32 (WINAPI *GetType)(void *iface);
    void *(WINAPI *GetNative)(void *iface);
    INT32 (WINAPI *GetPixelSizeInBytes)(void *iface);
    INT32 (WINAPI *GetOffsetX)(void *iface);
    INT32 (WINAPI *GetOffsetY)(void *iface);
    INT32 (WINAPI *GetWidth)(void *iface);
    INT32 (WINAPI *GetHeight)(void *iface);
    INT32 (WINAPI *GetHPitch)(void *iface);
    INT32 (WINAPI *GetVPitch)(void *iface);
    BYTE  (WINAPI *IsTiled)(void *iface);
};
C_ASSERT(offsetof(struct amf_plane_vtbl, GetNative) == 0x20);
C_ASSERT(offsetof(struct amf_plane_vtbl, GetHeight) == 0x48);
C_ASSERT(offsetof(struct amf_plane_vtbl, GetHPitch) == 0x50);

struct amf_surface_vtbl
{
    AMF_INTERFACE_SLOTS
    AMF_PROPERTY_STORAGE_SLOTS
    AMF_DATA_SLOTS
    INT32      (WINAPI *GetFormat)(void *iface);
    SIZE_T     (WINAPI *GetPlanesCount)(void *iface);
    void *     (WINAPI *GetPlaneAt)(void *iface, SIZE_T index);
    void *     (WINAPI *GetPlane)(void *iface, INT32 type);
    INT32      (WINAPI *GetFrameType)(void *iface);
    void       (WINAPI *SetFrameType)(void *iface, INT32 type);
    AMF_RESULT (WINAPI *SetCrop)(void *iface, INT32 x, INT32 y, INT32 width, INT32 height);
    AMF_RESULT (WINAPI *CopySurfaceRegion)(void *iface, void *dest, INT32 dst_x, INT32 dst_y, INT32 src_x,
                                           INT32 src_y, INT32 width, INT32 height);
    void       (WINAPI *AddSurfaceObserver)(void *iface, void *observer);
    void       (WINAPI *RemoveSurfaceObserver)(void *iface, void *observer);
};
C_ASSERT(offsetof(struct amf_surface_vtbl, Convert) == 0x78);
C_ASSERT(offsetof(struct amf_surface_vtbl, GetPlanesCount) == 0xc0);
C_ASSERT(offsetof(struct amf_surface_vtbl, GetPlaneAt) == 0xc8);
C_ASSERT(sizeof(struct amf_surface_vtbl) == 0x108);

static unsigned int format_bytes_per_sample(INT32 format)
{
    return format == AMF_SURFACE_P010 ? 2 : 1;
}

/* Copies the width x height region at (x, y) of an NV12/P010 image whose chroma plane starts at
 * src + src_pitch * src_vpitch into a new top-down image whose chroma plane starts at
 * dst + pitch * height. Keeps the source pitch when it can, so that each plane is one memcpy; never
 * reads past the last pixel of either source plane. */
static BYTE *copy_frame(const BYTE *src, INT32 src_pitch, INT32 src_vpitch, INT32 x, INT32 y, INT32 width,
                        INT32 height, INT32 format, INT32 *pitch)
{
    SIZE_T bps = format_bytes_per_sample(format);
    SIZE_T luma_row = (SIZE_T)width * bps, chroma_row = (SIZE_T)((width + 1) / 2) * 2 * bps;
    SIZE_T row = luma_row > chroma_row ? luma_row : chroma_row;
    INT32 chroma_height = (height + 1) / 2, i;
    const BYTE *src_luma = src + (SIZE_T)y * src_pitch + (SIZE_T)x * bps;
    const BYTE *src_chroma = src + (SIZE_T)src_pitch * src_vpitch + (SIZE_T)(y / 2) * src_pitch
            + (SIZE_T)(x / 2) * 2 * bps;
    SIZE_T dst_pitch = (!x && (SIZE_T)src_pitch >= row) ? (SIZE_T)src_pitch : (row + 63) & ~(SIZE_T)63;
    BYTE *dst, *dst_chroma;

    if (!(dst = malloc(dst_pitch * (height + chroma_height)))) return NULL;
    dst_chroma = dst + dst_pitch * height;
    if (dst_pitch == (SIZE_T)src_pitch)
    {
        memcpy(dst, src_luma, dst_pitch * (height - 1) + luma_row);
        memcpy(dst_chroma, src_chroma, dst_pitch * (chroma_height - 1) + chroma_row);
    }
    else
    {
        for (i = 0; i < height; i++)
            memcpy(dst + i * dst_pitch, src_luma + (SIZE_T)i * src_pitch, luma_row);
        for (i = 0; i < chroma_height; i++)
            memcpy(dst_chroma + i * dst_pitch, src_chroma + (SIZE_T)i * src_pitch, chroma_row);
    }
    *pitch = (INT32)dst_pitch;
    return dst;
}

/* The surface's current pixels: luma at the returned pointer, chroma at that + *pitch * *vpitch. */
static const BYTE *surface_pixels(struct amf_surface *surface, INT32 *pitch, INT32 *vpitch)
{
    if (surface->own)
    {
        *pitch = surface->own_pitch;
        *vpitch = surface->height;
        return surface->own;
    }
    *pitch = surface->host_pitch;
    *vpitch = surface->host_vpitch;
    return surface->host;
}

static AMF_RESULT surface_take_copy(struct amf_surface *surface)
{
    if (surface->own) return AMF_OK;
    if (!(surface->own = copy_frame(surface->host, surface->host_pitch, surface->host_vpitch, 0, 0,
                                    surface->width, surface->height, surface->format, &surface->own_pitch)))
        return AMF_OUT_OF_MEMORY;
    /* From here on the surface never reads the caller's memory again. */
    surface->host = NULL;
    return AMF_OK;
}

/* Pixels for vkenc_submit: a width x height frame with chroma at data + pitch * height. Points into
 * the surface when its layout already is that, otherwise into *temp, which the caller frees. */
static const BYTE *surface_frame(struct amf_surface *surface, INT32 width, INT32 height, INT32 *pitch,
                                 BYTE **temp)
{
    INT32 src_pitch, src_vpitch;
    const BYTE *src = surface_pixels(surface, &src_pitch, &src_vpitch);

    *temp = NULL;
    if (!surface->crop_x && !surface->crop_y && src_vpitch == height)
    {
        *pitch = src_pitch;
        return src;
    }
    *temp = copy_frame(src, src_pitch, src_vpitch, surface->crop_x, surface->crop_y, width, height,
                       surface->format, pitch);
    return *temp;
}

static struct amf_surface *surface_from_plane(void *iface)
{
    return ((struct amf_plane *)iface)->surface;
}

static INT32 WINAPI plane_Acquire(void *iface)
{
    return object_Acquire(surface_from_plane(iface));
}

static INT32 WINAPI plane_Release(void *iface)
{
    return object_Release(surface_from_plane(iface));
}

static AMF_RESULT WINAPI plane_QueryInterface(void *iface, const GUID *iid, void **out)
{
    TRACE("plane %p, iid %s, out %p\n", iface, debugstr_guid(iid), out);
    if (!out) return AMF_INVALID_POINTER;
    *out = NULL;
    if (!iid) return AMF_INVALID_POINTER;
    if (!IsEqualGUID(iid, &amf_iid_interface) && !IsEqualGUID(iid, &amf_iid_plane))
    {
        WARN("plane %p: no interface %s\n", iface, debugstr_guid(iid));
        return AMF_NO_INTERFACE;
    }
    plane_Acquire(iface);
    *out = iface;
    return AMF_OK;
}

static INT32 WINAPI plane_GetType(void *iface)
{
    return ((struct amf_plane *)iface)->index ? AMF_PLANE_UV : AMF_PLANE_Y;
}

static void * WINAPI plane_GetNative(void *iface)
{
    struct amf_plane *plane = iface;
    INT32 pitch, vpitch;
    const BYTE *data = surface_pixels(plane->surface, &pitch, &vpitch);

    TRACE("plane %p\n", plane);
    if (!data) return NULL;
    return (void *)(plane->index ? data + (SIZE_T)pitch * vpitch : data);
}

static INT32 WINAPI plane_GetPixelSizeInBytes(void *iface)
{
    struct amf_plane *plane = iface;

    return format_bytes_per_sample(plane->surface->format) * (plane->index ? 2 : 1);
}

static INT32 WINAPI plane_GetOffsetX(void *iface)
{
    struct amf_plane *plane = iface;

    return plane->index ? plane->surface->crop_x / 2 : plane->surface->crop_x;
}

static INT32 WINAPI plane_GetOffsetY(void *iface)
{
    struct amf_plane *plane = iface;

    return plane->index ? plane->surface->crop_y / 2 : plane->surface->crop_y;
}

static INT32 WINAPI plane_GetWidth(void *iface)
{
    struct amf_plane *plane = iface;
    INT32 width = plane->surface->crop_width;

    return plane->index ? (width + 1) / 2 : width;
}

static INT32 WINAPI plane_GetHeight(void *iface)
{
    struct amf_plane *plane = iface;
    INT32 height = plane->surface->crop_height;

    return plane->index ? (height + 1) / 2 : height;
}

static INT32 WINAPI plane_GetHPitch(void *iface)
{
    struct amf_plane *plane = iface;
    INT32 pitch, vpitch;

    surface_pixels(plane->surface, &pitch, &vpitch);
    return pitch;
}

static INT32 WINAPI plane_GetVPitch(void *iface)
{
    struct amf_plane *plane = iface;
    INT32 pitch, vpitch;

    surface_pixels(plane->surface, &pitch, &vpitch);
    return plane->index ? (vpitch + 1) / 2 : vpitch;
}

static BYTE WINAPI plane_IsTiled(void *iface)
{
    return FALSE;
}

static const struct amf_plane_vtbl plane_vtbl =
{
    .Acquire = plane_Acquire,
    .Release = plane_Release,
    .QueryInterface = plane_QueryInterface,
    .GetType = plane_GetType,
    .GetNative = plane_GetNative,
    .GetPixelSizeInBytes = plane_GetPixelSizeInBytes,
    .GetOffsetX = plane_GetOffsetX,
    .GetOffsetY = plane_GetOffsetY,
    .GetWidth = plane_GetWidth,
    .GetHeight = plane_GetHeight,
    .GetHPitch = plane_GetHPitch,
    .GetVPitch = plane_GetVPitch,
    .IsTiled = plane_IsTiled,
};

static INT32 WINAPI surface_GetMemoryType(void *iface)
{
    return ((struct amf_surface *)iface)->memory_type;
}

static AMF_RESULT WINAPI surface_Duplicate(void *iface, INT32 type, void **out)
{
    FIXME("surface %p, type %d, out %p: not implemented\n", iface, type, out);
    if (out) *out = NULL;
    return AMF_NOT_IMPLEMENTED;
}

static AMF_RESULT WINAPI surface_Convert(void *iface, INT32 type)
{
    struct amf_surface *surface = iface;

    TRACE("surface %p, type %d\n", surface, type);
    switch (type)
    {
    case AMF_MEMORY_HOST:
        /* The pixels are in host memory either way. */
        surface->memory_type = type;
        return AMF_OK;
    case AMF_MEMORY_DX9:
    case AMF_MEMORY_DX11:
        /* On Windows this uploads the frame to a GPU texture, after which the surface no longer reads
         * the caller's memory; Adobe frees that memory as soon as SubmitInput and Release return. The
         * equivalent here is our own copy. */
        if (surface_take_copy(surface))
        {
            ERR("surface %p: out of memory copying a %dx%d frame\n", surface, surface->width, surface->height);
            return AMF_OUT_OF_MEMORY;
        }
        surface->memory_type = type;
        return AMF_OK;
    default:
        FIXME("surface %p: memory type %d not supported\n", surface, type);
        return AMF_NOT_SUPPORTED;
    }
}

static AMF_RESULT WINAPI surface_Interop(void *iface, INT32 type)
{
    TRACE("surface %p, type %d\n", iface, type);
    /* Interop never copies through host memory, so only the host memory type works. */
    if (type == AMF_MEMORY_HOST) return surface_Convert(iface, type);
    return AMF_NOT_SUPPORTED;
}

static INT32 WINAPI surface_GetDataType(void *iface)
{
    return AMF_DATA_SURFACE;
}

static BYTE WINAPI surface_IsReusable(void *iface)
{
    return FALSE;
}

static void WINAPI surface_SetPts(void *iface, INT64 pts)
{
    ((struct amf_surface *)iface)->pts = pts;
}

static INT64 WINAPI surface_GetPts(void *iface)
{
    return ((struct amf_surface *)iface)->pts;
}

static void WINAPI surface_SetDuration(void *iface, INT64 duration)
{
    ((struct amf_surface *)iface)->duration = duration;
}

static INT64 WINAPI surface_GetDuration(void *iface)
{
    return ((struct amf_surface *)iface)->duration;
}

static INT32 WINAPI surface_GetFormat(void *iface)
{
    return ((struct amf_surface *)iface)->format;
}

static SIZE_T WINAPI surface_GetPlanesCount(void *iface)
{
    return 2;
}

static void * WINAPI surface_GetPlaneAt(void *iface, SIZE_T index)
{
    struct amf_surface *surface = iface;

    TRACE("surface %p, index %u\n", surface, (unsigned int)index);
    return index < 2 ? &surface->planes[index] : NULL;
}

static void * WINAPI surface_GetPlane(void *iface, INT32 type)
{
    struct amf_surface *surface = iface;

    TRACE("surface %p, type %d\n", surface, type);
    if (type == AMF_PLANE_Y) return &surface->planes[0];
    if (type == AMF_PLANE_UV) return &surface->planes[1];
    return NULL;
}

static INT32 WINAPI surface_GetFrameType(void *iface)
{
    return ((struct amf_surface *)iface)->frame_type;
}

static void WINAPI surface_SetFrameType(void *iface, INT32 type)
{
    TRACE("surface %p, type %#x\n", iface, type);
    ((struct amf_surface *)iface)->frame_type = type;
}

static AMF_RESULT WINAPI surface_SetCrop(void *iface, INT32 x, INT32 y, INT32 width, INT32 height)
{
    struct amf_surface *surface = iface;

    TRACE("surface %p, %d,%d %dx%d\n", surface, x, y, width, height);
    if (x < 0 || y < 0 || width <= 0 || height <= 0 || x > surface->width - width
            || y > surface->height - height)
        return AMF_INVALID_ARG;
    surface->crop_x = x & ~1;
    surface->crop_y = y & ~1;
    surface->crop_width = width;
    surface->crop_height = height;
    return AMF_OK;
}

static AMF_RESULT WINAPI surface_CopySurfaceRegion(void *iface, void *dest, INT32 dst_x, INT32 dst_y,
                                                   INT32 src_x, INT32 src_y, INT32 width, INT32 height)
{
    FIXME("surface %p, dest %p: not implemented\n", iface, dest);
    return AMF_NOT_IMPLEMENTED;
}

static void WINAPI surface_AddSurfaceObserver(void *iface, void *observer)
{
    struct amf_surface *surface = iface;

    TRACE("surface %p, observer %p\n", surface, observer);
    if (!observer) return;
    AcquireSRWLockExclusive(&surface->obj.store.lock);
    if (!observer_add(&surface->observers, observer)) FIXME("too many surface observers\n");
    ReleaseSRWLockExclusive(&surface->obj.store.lock);
}

static void WINAPI surface_RemoveSurfaceObserver(void *iface, void *observer)
{
    struct amf_surface *surface = iface;

    TRACE("surface %p, observer %p\n", surface, observer);
    AcquireSRWLockExclusive(&surface->obj.store.lock);
    observer_remove(&surface->observers, observer);
    ReleaseSRWLockExclusive(&surface->obj.store.lock);
}

static void surface_destroy(struct amf_object *object)
{
    struct amf_surface *surface = (struct amf_surface *)object;
    unsigned int i;

    for (i = 0; i < surface->observers.count; i++)
    {
        struct amf_observer *observer = surface->observers.items[i];
        ((const struct surface_observer_vtbl *)observer->vtbl)->OnSurfaceDataRelease(observer, surface);
    }
    free(surface->own);
}

static const GUID *const surface_iids[] =
{
    &amf_iid_interface, &amf_iid_property_storage, &amf_iid_data, &amf_iid_surface, NULL
};

static const struct object_ops surface_ops = {"surface", surface_iids, surface_destroy};

static const struct amf_surface_vtbl surface_vtbl =
{
    AMF_INTERFACE_IMPL
    .SetProperty = object_SetProperty,
    AMF_PROPERTY_STORAGE_IMPL
    .GetMemoryType = surface_GetMemoryType,
    .Duplicate = surface_Duplicate,
    .Convert = surface_Convert,
    .Interop = surface_Interop,
    .GetDataType = surface_GetDataType,
    .IsReusable = surface_IsReusable,
    .SetPts = surface_SetPts,
    .GetPts = surface_GetPts,
    .SetDuration = surface_SetDuration,
    .GetDuration = surface_GetDuration,
    .GetFormat = surface_GetFormat,
    .GetPlanesCount = surface_GetPlanesCount,
    .GetPlaneAt = surface_GetPlaneAt,
    .GetPlane = surface_GetPlane,
    .GetFrameType = surface_GetFrameType,
    .SetFrameType = surface_SetFrameType,
    .SetCrop = surface_SetCrop,
    .CopySurfaceRegion = surface_CopySurfaceRegion,
    .AddSurfaceObserver = surface_AddSurfaceObserver,
    .RemoveSurfaceObserver = surface_RemoveSurfaceObserver,
};

static struct amf_surface *unsafe_surface(void *data)
{
    struct amf_object *object = data;

    return object && object->vtbl == &surface_vtbl ? (struct amf_surface *)object : NULL;
}

/* -------------------------------------------------------------------------------------------------
 * AMFBuffer: host memory; the encoder's output access units
 */

struct amf_buffer
{
    struct amf_object obj;
    BYTE *data;
    SIZE_T size, capacity;
    void (*free_data)(uint8_t *data);
    INT64 pts, duration;
    struct observer_list observers;     /* AMFBufferObserver, guarded by obj.store.lock */
};

struct amf_buffer_vtbl
{
    AMF_INTERFACE_SLOTS
    AMF_PROPERTY_STORAGE_SLOTS
    AMF_DATA_SLOTS
    AMF_RESULT (WINAPI *SetSize)(void *iface, SIZE_T size);
    SIZE_T     (WINAPI *GetSize)(void *iface);
    void *     (WINAPI *GetNative)(void *iface);
    void       (WINAPI *AddBufferObserver)(void *iface, void *observer);
    void       (WINAPI *RemoveBufferObserver)(void *iface, void *observer);
};
C_ASSERT(offsetof(struct amf_buffer_vtbl, GetSize) == 0xc0);
C_ASSERT(offsetof(struct amf_buffer_vtbl, GetNative) == 0xc8);

static const struct amf_buffer_vtbl buffer_vtbl;
static const struct object_ops buffer_ops;

static void free_host(uint8_t *data)
{
    free(data);
}

/* Takes ownership of data, which free_data releases. */
static struct amf_buffer *buffer_create(BYTE *data, SIZE_T size, void (*free_data)(uint8_t *data))
{
    struct amf_buffer *buffer;

    if (!(buffer = calloc(1, sizeof(*buffer)))) return NULL;
    object_init(&buffer->obj, &buffer_vtbl, &buffer_ops);
    buffer->data = data;
    buffer->size = buffer->capacity = size;
    buffer->free_data = free_data;
    return buffer;
}

static struct amf_buffer *buffer_create_host(SIZE_T size)
{
    struct amf_buffer *buffer;
    BYTE *data;

    if (!(data = calloc(1, size ? size : 1))) return NULL;
    if (!(buffer = buffer_create(data, size, free_host))) free(data);
    return buffer;
}

static INT32 WINAPI buffer_GetMemoryType(void *iface)
{
    return AMF_MEMORY_HOST;
}

static AMF_RESULT WINAPI buffer_Duplicate(void *iface, INT32 type, void **out)
{
    struct amf_buffer *buffer = iface, *copy;
    AMF_RESULT ret;

    TRACE("buffer %p, type %d, out %p\n", buffer, type, out);
    if (!out) return AMF_INVALID_POINTER;
    *out = NULL;
    if (type != AMF_MEMORY_HOST && type != AMF_MEMORY_UNKNOWN) return AMF_NOT_SUPPORTED;
    if (!(copy = buffer_create_host(buffer->size))) return AMF_OUT_OF_MEMORY;
    memcpy(copy->data, buffer->data, buffer->size);
    copy->pts = buffer->pts;
    copy->duration = buffer->duration;
    if ((ret = object_AddTo(buffer, copy, TRUE, FALSE)))
    {
        object_Release(copy);
        return ret;
    }
    *out = copy;
    return AMF_OK;
}

static AMF_RESULT WINAPI buffer_Convert(void *iface, INT32 type)
{
    TRACE("buffer %p, type %d\n", iface, type);
    return type == AMF_MEMORY_HOST ? AMF_OK : AMF_NOT_SUPPORTED;
}

static INT32 WINAPI buffer_GetDataType(void *iface)
{
    return AMF_DATA_BUFFER;
}

static BYTE WINAPI buffer_IsReusable(void *iface)
{
    return FALSE;
}

static void WINAPI buffer_SetPts(void *iface, INT64 pts)
{
    ((struct amf_buffer *)iface)->pts = pts;
}

static INT64 WINAPI buffer_GetPts(void *iface)
{
    return ((struct amf_buffer *)iface)->pts;
}

static void WINAPI buffer_SetDuration(void *iface, INT64 duration)
{
    ((struct amf_buffer *)iface)->duration = duration;
}

static INT64 WINAPI buffer_GetDuration(void *iface)
{
    return ((struct amf_buffer *)iface)->duration;
}

static AMF_RESULT WINAPI buffer_SetSize(void *iface, SIZE_T size)
{
    struct amf_buffer *buffer = iface;

    TRACE("buffer %p, size %u\n", buffer, (unsigned int)size);
    if (size > buffer->capacity) return AMF_INVALID_ARG;
    buffer->size = size;
    return AMF_OK;
}

static SIZE_T WINAPI buffer_GetSize(void *iface)
{
    struct amf_buffer *buffer = iface;

    TRACE("buffer %p -> %u\n", buffer, (unsigned int)buffer->size);
    return buffer->size;
}

static void * WINAPI buffer_GetNative(void *iface)
{
    struct amf_buffer *buffer = iface;

    TRACE("buffer %p -> %p\n", buffer, buffer->data);
    return buffer->data;
}

static void WINAPI buffer_AddBufferObserver(void *iface, void *observer)
{
    struct amf_buffer *buffer = iface;

    TRACE("buffer %p, observer %p\n", buffer, observer);
    if (!observer) return;
    AcquireSRWLockExclusive(&buffer->obj.store.lock);
    if (!observer_add(&buffer->observers, observer)) FIXME("too many buffer observers\n");
    ReleaseSRWLockExclusive(&buffer->obj.store.lock);
}

static void WINAPI buffer_RemoveBufferObserver(void *iface, void *observer)
{
    struct amf_buffer *buffer = iface;

    TRACE("buffer %p, observer %p\n", buffer, observer);
    AcquireSRWLockExclusive(&buffer->obj.store.lock);
    observer_remove(&buffer->observers, observer);
    ReleaseSRWLockExclusive(&buffer->obj.store.lock);
}

static void buffer_destroy(struct amf_object *object)
{
    struct amf_buffer *buffer = (struct amf_buffer *)object;
    unsigned int i;

    for (i = 0; i < buffer->observers.count; i++)
    {
        struct amf_observer *observer = buffer->observers.items[i];
        ((const struct buffer_observer_vtbl *)observer->vtbl)->OnBufferDataRelease(observer, buffer);
    }
    if (buffer->free_data) buffer->free_data(buffer->data);
}

static const GUID *const buffer_iids[] =
{
    &amf_iid_interface, &amf_iid_property_storage, &amf_iid_data, &amf_iid_buffer, NULL
};

static const struct object_ops buffer_ops = {"buffer", buffer_iids, buffer_destroy};

static const struct amf_buffer_vtbl buffer_vtbl =
{
    AMF_INTERFACE_IMPL
    .SetProperty = object_SetProperty,
    AMF_PROPERTY_STORAGE_IMPL
    .GetMemoryType = buffer_GetMemoryType,
    .Duplicate = buffer_Duplicate,
    .Convert = buffer_Convert,
    .Interop = buffer_Convert,
    .GetDataType = buffer_GetDataType,
    .IsReusable = buffer_IsReusable,
    .SetPts = buffer_SetPts,
    .GetPts = buffer_GetPts,
    .SetDuration = buffer_SetDuration,
    .GetDuration = buffer_GetDuration,
    .SetSize = buffer_SetSize,
    .GetSize = buffer_GetSize,
    .GetNative = buffer_GetNative,
    .AddBufferObserver = buffer_AddBufferObserver,
    .RemoveBufferObserver = buffer_RemoveBufferObserver,
};

/* -------------------------------------------------------------------------------------------------
 * AMFContext. Adobe asks for InitDX11(NULL) (else InitDX9(NULL)), i.e. "make your own device", and then
 * only ever hands the encoder host-memory frames; nothing here needs a D3D device, so none is created.
 */

struct amf_context
{
    struct amf_object obj;
    void *dx9_device, *dx11_device;     /* what the caller passed, never used */
};

struct amf_context_vtbl
{
    AMF_INTERFACE_SLOTS
    AMF_PROPERTY_STORAGE_SLOTS
    AMF_RESULT (WINAPI *Terminate)(void *iface);
    AMF_RESULT (WINAPI *InitDX9)(void *iface, void *device);
    void *     (WINAPI *GetDX9Device)(void *iface, INT32 version);
    AMF_RESULT (WINAPI *LockDX9)(void *iface);
    AMF_RESULT (WINAPI *UnlockDX9)(void *iface);
    AMF_RESULT (WINAPI *InitDX11)(void *iface, void *device, INT32 version);
    void *     (WINAPI *GetDX11Device)(void *iface, INT32 version);
    AMF_RESULT (WINAPI *LockDX11)(void *iface);
    AMF_RESULT (WINAPI *UnlockDX11)(void *iface);
    AMF_RESULT (WINAPI *InitOpenCL)(void *iface, void *queue);
    void *     (WINAPI *GetOpenCLContext)(void *iface);
    void *     (WINAPI *GetOpenCLCommandQueue)(void *iface);
    void *     (WINAPI *GetOpenCLDeviceID)(void *iface);
    AMF_RESULT (WINAPI *GetOpenCLComputeFactory)(void *iface, void **factory);
    AMF_RESULT (WINAPI *InitOpenCLEx)(void *iface, void *device);
    AMF_RESULT (WINAPI *LockOpenCL)(void *iface);
    AMF_RESULT (WINAPI *UnlockOpenCL)(void *iface);
    AMF_RESULT (WINAPI *InitOpenGL)(void *iface, void *context, void *window, void *dc);
    void *     (WINAPI *GetOpenGLContext)(void *iface);
    void *     (WINAPI *GetOpenGLDrawable)(void *iface);
    AMF_RESULT (WINAPI *LockOpenGL)(void *iface);
    AMF_RESULT (WINAPI *UnlockOpenGL)(void *iface);
    AMF_RESULT (WINAPI *InitXV)(void *iface, void *device);
    void *     (WINAPI *GetXVDevice)(void *iface);
    AMF_RESULT (WINAPI *LockXV)(void *iface);
    AMF_RESULT (WINAPI *UnlockXV)(void *iface);
    AMF_RESULT (WINAPI *InitGralloc)(void *iface, void *device);
    void *     (WINAPI *GetGrallocDevice)(void *iface);
    AMF_RESULT (WINAPI *LockGralloc)(void *iface);
    AMF_RESULT (WINAPI *UnlockGralloc)(void *iface);
    AMF_RESULT (WINAPI *AllocBuffer)(void *iface, INT32 type, SIZE_T size, void **buffer);
    AMF_RESULT (WINAPI *AllocSurface)(void *iface, INT32 type, INT32 format, INT32 width, INT32 height,
                                      void **surface);
    AMF_RESULT (WINAPI *AllocAudioBuffer)(void *iface, INT32 type, INT32 format, INT32 samples,
                                          INT32 sample_rate, INT32 channels, void **buffer);
    AMF_RESULT (WINAPI *CreateBufferFromHostNative)(void *iface, void *data, SIZE_T size, void **buffer,
                                                    void *observer);
    AMF_RESULT (WINAPI *CreateSurfaceFromHostNative)(void *iface, INT32 format, INT32 width, INT32 height,
                                                     INT32 hpitch, INT32 vpitch, void *data, void **surface,
                                                     void *observer);
    AMF_RESULT (WINAPI *CreateSurfaceFromDX9Native)(void *iface, void *dx9_surface, void **surface,
                                                    void *observer);
    AMF_RESULT (WINAPI *CreateSurfaceFromDX11Native)(void *iface, void *dx11_surface, void **surface,
                                                     void *observer);
    AMF_RESULT (WINAPI *CreateSurfaceFromOpenGLNative)(void *iface, INT32 format, void *texture,
                                                       void **surface, void *observer);
    AMF_RESULT (WINAPI *CreateSurfaceFromGrallocNative)(void *iface, void *gralloc_surface, void **surface,
                                                        void *observer);
    AMF_RESULT (WINAPI *CreateSurfaceFromOpenCLNative)(void *iface, INT32 format, INT32 width, INT32 height,
                                                       void **planes, void **surface, void *observer);
    AMF_RESULT (WINAPI *CreateBufferFromOpenCLNative)(void *iface, void *cl_buffer, SIZE_T size,
                                                      void **buffer);
    AMF_RESULT (WINAPI *GetCompute)(void *iface, INT32 type, void **compute);
};
C_ASSERT(offsetof(struct amf_context_vtbl, Terminate) == 0x68);
C_ASSERT(offsetof(struct amf_context_vtbl, InitDX9) == 0x70);
C_ASSERT(offsetof(struct amf_context_vtbl, InitDX11) == 0x90);
C_ASSERT(offsetof(struct amf_context_vtbl, AllocBuffer) == 0x158);
C_ASSERT(offsetof(struct amf_context_vtbl, CreateSurfaceFromHostNative) == 0x178);
C_ASSERT(offsetof(struct amf_context_vtbl, GetCompute) == 0x1b0);

static AMF_RESULT WINAPI context_Terminate(void *iface)
{
    struct amf_context *context = iface;

    TRACE("context %p\n", context);
    context->dx9_device = context->dx11_device = NULL;
    return AMF_OK;
}

static AMF_RESULT WINAPI context_InitDX9(void *iface, void *device)
{
    struct amf_context *context = iface;

    TRACE("context %p, device %p\n", context, device);
    context->dx9_device = device;
    return AMF_OK;
}

static void * WINAPI context_GetDX9Device(void *iface, INT32 version)
{
    struct amf_context *context = iface;

    TRACE("context %p, version %d\n", context, version);
    return context->dx9_device;
}

static AMF_RESULT WINAPI context_Lock(void *iface)
{
    TRACE("context %p\n", iface);
    return AMF_OK;
}

static AMF_RESULT WINAPI context_InitDX11(void *iface, void *device, INT32 version)
{
    struct amf_context *context = iface;

    TRACE("context %p, device %p, version %d\n", context, device, version);
    context->dx11_device = device;
    return AMF_OK;
}

static void * WINAPI context_GetDX11Device(void *iface, INT32 version)
{
    struct amf_context *context = iface;

    TRACE("context %p, version %d\n", context, version);
    return context->dx11_device;
}

static AMF_RESULT WINAPI context_InitOpenCL(void *iface, void *queue)
{
    FIXME("context %p, queue %p: not implemented\n", iface, queue);
    return AMF_NOT_IMPLEMENTED;
}

static void * WINAPI context_GetNull(void *iface)
{
    FIXME("context %p: not implemented\n", iface);
    return NULL;
}

static AMF_RESULT WINAPI context_GetOpenCLComputeFactory(void *iface, void **factory)
{
    FIXME("context %p, factory %p: not implemented\n", iface, factory);
    if (factory) *factory = NULL;
    return AMF_NOT_IMPLEMENTED;
}

static AMF_RESULT WINAPI context_InitOpenCLEx(void *iface, void *device)
{
    FIXME("context %p, device %p: not implemented\n", iface, device);
    return AMF_NOT_IMPLEMENTED;
}

static AMF_RESULT WINAPI context_NotImplemented(void *iface)
{
    FIXME("context %p: not implemented\n", iface);
    return AMF_NOT_IMPLEMENTED;
}

static AMF_RESULT WINAPI context_InitOpenGL(void *iface, void *gl_context, void *window, void *dc)
{
    FIXME("context %p, gl context %p, window %p, dc %p: not implemented\n", iface, gl_context, window, dc);
    return AMF_NOT_IMPLEMENTED;
}

static AMF_RESULT WINAPI context_InitDevice(void *iface, void *device)
{
    FIXME("context %p, device %p: not implemented\n", iface, device);
    return AMF_NOT_IMPLEMENTED;
}

static AMF_RESULT WINAPI context_AllocBuffer(void *iface, INT32 type, SIZE_T size, void **out)
{
    struct amf_buffer *buffer;

    TRACE("context %p, type %d, size %u, out %p\n", iface, type, (unsigned int)size, out);
    if (!out) return AMF_INVALID_POINTER;
    *out = NULL;
    if (type != AMF_MEMORY_HOST)
    {
        FIXME("memory type %d not supported\n", type);
        return AMF_NOT_SUPPORTED;
    }
    if (!(buffer = buffer_create_host(size))) return AMF_OUT_OF_MEMORY;
    *out = buffer;
    return AMF_OK;
}

static AMF_RESULT WINAPI context_AllocSurface(void *iface, INT32 type, INT32 format, INT32 width, INT32 height,
                                              void **out)
{
    FIXME("context %p, type %d, format %d, %dx%d: not implemented\n", iface, type, format, width, height);
    if (out) *out = NULL;
    return AMF_NOT_IMPLEMENTED;
}

static AMF_RESULT WINAPI context_AllocAudioBuffer(void *iface, INT32 type, INT32 format, INT32 samples,
                                                  INT32 sample_rate, INT32 channels, void **out)
{
    FIXME("context %p: not implemented\n", iface);
    if (out) *out = NULL;
    return AMF_NOT_IMPLEMENTED;
}

static AMF_RESULT WINAPI context_CreateBufferFromHostNative(void *iface, void *data, SIZE_T size, void **out,
                                                            void *observer)
{
    FIXME("context %p, data %p, size %u: not implemented\n", iface, data, (unsigned int)size);
    if (out) *out = NULL;
    return AMF_NOT_IMPLEMENTED;
}

static AMF_RESULT WINAPI context_CreateSurfaceFromHostNative(void *iface, INT32 format, INT32 width,
                                                             INT32 height, INT32 hpitch, INT32 vpitch,
                                                             void *data, void **out, void *observer)
{
    struct amf_surface *surface;
    unsigned int i;

    TRACE("context %p, format %d, %dx%d, hpitch %d, vpitch %d, data %p, out %p, observer %p\n", iface,
          format, width, height, hpitch, vpitch, data, out, observer);
    if (!out) return AMF_INVALID_POINTER;
    *out = NULL;
    if (!data) return AMF_INVALID_POINTER;
    if (format != AMF_SURFACE_NV12 && format != AMF_SURFACE_P010)
    {
        FIXME("surface format %d not supported\n", format);
        return AMF_SURFACE_FORMAT_NOT_SUPPORTED;
    }
    if (width <= 0 || height <= 0 || vpitch < height
            || hpitch < (INT32)(((width + 1) & ~1) * format_bytes_per_sample(format)))
    {
        WARN("invalid layout %dx%d, hpitch %d, vpitch %d\n", width, height, hpitch, vpitch);
        return AMF_INVALID_ARG;
    }
    if (!(surface = calloc(1, sizeof(*surface)))) return AMF_OUT_OF_MEMORY;
    object_init(&surface->obj, &surface_vtbl, &surface_ops);
    for (i = 0; i < 2; i++)
    {
        surface->planes[i].vtbl = &plane_vtbl;
        surface->planes[i].surface = surface;
        surface->planes[i].index = i;
    }
    surface->format = format;
    surface->width = surface->crop_width = width;
    surface->height = surface->crop_height = height;
    surface->memory_type = AMF_MEMORY_HOST;
    surface->host = data;
    surface->host_pitch = hpitch;
    surface->host_vpitch = vpitch;
    surface->frame_type = AMF_FRAME_PROGRESSIVE;
    if (observer) observer_add(&surface->observers, observer);
    *out = surface;
    return AMF_OK;
}

static AMF_RESULT WINAPI context_CreateSurfaceFromNative(void *iface, void *native, void **out, void *observer)
{
    FIXME("context %p, native %p: not implemented\n", iface, native);
    if (out) *out = NULL;
    return AMF_NOT_IMPLEMENTED;
}

static AMF_RESULT WINAPI context_CreateSurfaceFromOpenGLNative(void *iface, INT32 format, void *texture,
                                                               void **out, void *observer)
{
    FIXME("context %p, format %d, texture %p: not implemented\n", iface, format, texture);
    if (out) *out = NULL;
    return AMF_NOT_IMPLEMENTED;
}

static AMF_RESULT WINAPI context_CreateSurfaceFromOpenCLNative(void *iface, INT32 format, INT32 width,
                                                               INT32 height, void **planes, void **out,
                                                               void *observer)
{
    FIXME("context %p: not implemented\n", iface);
    if (out) *out = NULL;
    return AMF_NOT_IMPLEMENTED;
}

static AMF_RESULT WINAPI context_CreateBufferFromOpenCLNative(void *iface, void *cl_buffer, SIZE_T size,
                                                              void **out)
{
    FIXME("context %p: not implemented\n", iface);
    if (out) *out = NULL;
    return AMF_NOT_IMPLEMENTED;
}

static AMF_RESULT WINAPI context_GetCompute(void *iface, INT32 type, void **out)
{
    FIXME("context %p, type %d: not implemented\n", iface, type);
    if (out) *out = NULL;
    return AMF_NOT_IMPLEMENTED;
}

static const GUID *const context_iids[] =
{
    &amf_iid_interface, &amf_iid_property_storage, &amf_iid_context, NULL
};

static const struct object_ops context_ops = {"context", context_iids, NULL};

static const struct amf_context_vtbl context_vtbl =
{
    AMF_INTERFACE_IMPL
    .SetProperty = object_SetProperty,
    AMF_PROPERTY_STORAGE_IMPL
    .Terminate = context_Terminate,
    .InitDX9 = context_InitDX9,
    .GetDX9Device = context_GetDX9Device,
    .LockDX9 = context_Lock,
    .UnlockDX9 = context_Lock,
    .InitDX11 = context_InitDX11,
    .GetDX11Device = context_GetDX11Device,
    .LockDX11 = context_Lock,
    .UnlockDX11 = context_Lock,
    .InitOpenCL = context_InitOpenCL,
    .GetOpenCLContext = context_GetNull,
    .GetOpenCLCommandQueue = context_GetNull,
    .GetOpenCLDeviceID = context_GetNull,
    .GetOpenCLComputeFactory = context_GetOpenCLComputeFactory,
    .InitOpenCLEx = context_InitOpenCLEx,
    .LockOpenCL = context_NotImplemented,
    .UnlockOpenCL = context_NotImplemented,
    .InitOpenGL = context_InitOpenGL,
    .GetOpenGLContext = context_GetNull,
    .GetOpenGLDrawable = context_GetNull,
    .LockOpenGL = context_NotImplemented,
    .UnlockOpenGL = context_NotImplemented,
    .InitXV = context_InitDevice,
    .GetXVDevice = context_GetNull,
    .LockXV = context_NotImplemented,
    .UnlockXV = context_NotImplemented,
    .InitGralloc = context_InitDevice,
    .GetGrallocDevice = context_GetNull,
    .LockGralloc = context_NotImplemented,
    .UnlockGralloc = context_NotImplemented,
    .AllocBuffer = context_AllocBuffer,
    .AllocSurface = context_AllocSurface,
    .AllocAudioBuffer = context_AllocAudioBuffer,
    .CreateBufferFromHostNative = context_CreateBufferFromHostNative,
    .CreateSurfaceFromHostNative = context_CreateSurfaceFromHostNative,
    .CreateSurfaceFromDX9Native = context_CreateSurfaceFromNative,
    .CreateSurfaceFromDX11Native = context_CreateSurfaceFromNative,
    .CreateSurfaceFromOpenGLNative = context_CreateSurfaceFromOpenGLNative,
    .CreateSurfaceFromGrallocNative = context_CreateSurfaceFromNative,
    .CreateSurfaceFromOpenCLNative = context_CreateSurfaceFromOpenCLNative,
    .CreateBufferFromOpenCLNative = context_CreateBufferFromOpenCLNative,
    .GetCompute = context_GetCompute,
};

static AMF_RESULT context_create(void **out)
{
    struct amf_context *context;

    if (!(context = calloc(1, sizeof(*context)))) return AMF_OUT_OF_MEMORY;
    object_init(&context->obj, &context_vtbl, &context_ops);
    *out = context;
    return AMF_OK;
}

/* -------------------------------------------------------------------------------------------------
 * Codec tables: the property names of each encoder component
 */

struct codec_desc
{
    vkenc_codec codec;
    const char *name;
    const WCHAR *id;
    const WCHAR *frame_size, *frame_rate, *aspect_ratio, *rate_control, *target_bitrate, *peak_bitrate,
                *filler_data, *profile, *level, *tier, *color_bit_depth, *scan_type, *idr_period,
                *header_spacing, *gop_size, *gops_per_idr, *force_picture_type, *output_data_type,
                *out_primaries, *out_transfer, *out_color_profile, *out_matrix, *out_full_range,
                *cap_max_bitrate, *cap_max_profile, *cap_max_level, *cap_num_streams;
    INT64 rc_cbr;               /* RateControlMethod value for CBR (H.264 1, HEVC 3) */
};

static const struct codec_desc avc_desc =
{
    .codec = VKENC_CODEC_H264,
    .name = "H.264",
    .id = L"AMFVideoEncoderVCE_AVC",
    .frame_size = L"FrameSize",
    .frame_rate = L"FrameRate",
    .aspect_ratio = L"AspectRatio",
    .rate_control = L"RateControlMethod",
    .target_bitrate = L"TargetBitrate",
    .peak_bitrate = L"PeakBitrate",
    .filler_data = L"FillerDataEnable",
    .profile = L"Profile",
    .level = L"ProfileLevel",
    .color_bit_depth = L"ColorBitDepth",
    .scan_type = L"ScanType",
    .idr_period = L"IDRPeriod",
    .header_spacing = L"HeaderInsertionSpacing",
    .force_picture_type = L"ForcePictureType",
    .output_data_type = L"OutputDataType",
    .out_primaries = L"OutColorPrimaries",
    .out_transfer = L"OutColorTransferChar",
    .out_color_profile = L"OutColorProfile",
    .out_full_range = L"FullRangeColor",
    .cap_max_bitrate = L"MaxBitrate",
    .cap_max_profile = L"MaxProfile",
    .cap_max_level = L"MaxLevel",
    .cap_num_streams = L"NumOfStreams",
    .rc_cbr = 1,
};

static const struct codec_desc hevc_desc =
{
    .codec = VKENC_CODEC_HEVC,
    .name = "HEVC",
    .id = L"AMFVideoEncoderHW_HEVC",
    .frame_size = L"HevcFrameSize",
    .frame_rate = L"HevcFrameRate",
    .aspect_ratio = L"HevcAspectRatio",
    .rate_control = L"HevcRateControlMethod",
    .target_bitrate = L"HevcTargetBitrate",
    .peak_bitrate = L"HevcPeakBitrate",
    .filler_data = L"HevcFillerDataEnable",
    .profile = L"HevcProfile",
    .level = L"HevcProfileLevel",
    .tier = L"HevcTier",
    .color_bit_depth = L"HevcColorBitDepth",
    .gop_size = L"HevcGOPSize",
    .gops_per_idr = L"HevcGOPSPerIDR",
    .force_picture_type = L"HevcForcePictureType",
    .output_data_type = L"HevcOutputDataType",
    .out_primaries = L"HevcOutColorPrimaries",
    .out_transfer = L"HevcOutColorTransferChar",
    .out_color_profile = L"HevcOutColorProfile",
    .out_matrix = L"HevcOutMatrixCoeff",
    .out_full_range = L"HevcNominalRange",
    .cap_max_bitrate = L"HevcMaxBitrate",
    .cap_max_profile = L"HevcMaxProfile",
    .cap_max_level = L"HevcMaxLevel",
    .cap_num_streams = L"HevcNumOfStreams",
    .rc_cbr = 3,
};

static const struct codec_desc *const codec_descs[] = {&avc_desc, &hevc_desc};

/* What vkenc_query_caps reported for each codec_descs entry; filled once, by AMFInit. */
static vkenc_caps codec_caps[ARRAY_SIZE(codec_descs)];
static INIT_ONCE codec_caps_once = INIT_ONCE_STATIC_INIT;

/* Used when the backend does not know the encoder's limit; Adobe clamps its bit rate to this. */
#define FALLBACK_MAX_BITRATE 500000000

static INT64 caps_max_bitrate(const vkenc_caps *caps)
{
    if (!caps->max_bitrate || caps->max_bitrate > (UINT64)INT64_MAX) return FALLBACK_MAX_BITRATE;
    return (INT64)caps->max_bitrate;
}

/* -------------------------------------------------------------------------------------------------
 * AMFCaps and AMFIOCaps
 */

struct amf_io_caps
{
    struct amf_object obj;
    INT32 min_width, max_width, min_height, max_height;
    INT32 formats[2];
    unsigned int format_count;
};

struct amf_io_caps_vtbl
{
    AMF_INTERFACE_SLOTS
    void       (WINAPI *GetWidthRange)(void *iface, INT32 *min, INT32 *max);
    void       (WINAPI *GetHeightRange)(void *iface, INT32 *min, INT32 *max);
    INT32      (WINAPI *GetVertAlign)(void *iface);
    INT32      (WINAPI *GetNumOfFormats)(void *iface);
    AMF_RESULT (WINAPI *GetFormatAt)(void *iface, INT32 index, INT32 *format, BYTE *native);
    INT32      (WINAPI *GetNumOfMemoryTypes)(void *iface);
    AMF_RESULT (WINAPI *GetMemoryTypeAt)(void *iface, INT32 index, INT32 *type, BYTE *native);
    BYTE       (WINAPI *IsInterlacedSupported)(void *iface);
};
C_ASSERT(offsetof(struct amf_io_caps_vtbl, GetWidthRange) == 0x18);
C_ASSERT(offsetof(struct amf_io_caps_vtbl, GetHeightRange) == 0x20);

/* The memory types an encoder input accepts: host frames converted to DX11 or DX9, or left in host. */
static const INT32 io_caps_memory_types[] = {AMF_MEMORY_DX11, AMF_MEMORY_DX9, AMF_MEMORY_HOST};

static void WINAPI io_caps_GetWidthRange(void *iface, INT32 *min, INT32 *max)
{
    struct amf_io_caps *caps = iface;

    TRACE("caps %p -> %d..%d\n", caps, caps->min_width, caps->max_width);
    if (min) *min = caps->min_width;
    if (max) *max = caps->max_width;
}

static void WINAPI io_caps_GetHeightRange(void *iface, INT32 *min, INT32 *max)
{
    struct amf_io_caps *caps = iface;

    TRACE("caps %p -> %d..%d\n", caps, caps->min_height, caps->max_height);
    if (min) *min = caps->min_height;
    if (max) *max = caps->max_height;
}

static INT32 WINAPI io_caps_GetVertAlign(void *iface)
{
    return 2;
}

static INT32 WINAPI io_caps_GetNumOfFormats(void *iface)
{
    return ((struct amf_io_caps *)iface)->format_count;
}

static AMF_RESULT WINAPI io_caps_GetFormatAt(void *iface, INT32 index, INT32 *format, BYTE *native)
{
    struct amf_io_caps *caps = iface;

    if (index < 0 || index >= (INT32)caps->format_count) return AMF_INVALID_ARG;
    if (format) *format = caps->formats[index];
    if (native) *native = TRUE;
    return AMF_OK;
}

static INT32 WINAPI io_caps_GetNumOfMemoryTypes(void *iface)
{
    return ARRAY_SIZE(io_caps_memory_types);
}

static AMF_RESULT WINAPI io_caps_GetMemoryTypeAt(void *iface, INT32 index, INT32 *type, BYTE *native)
{
    if (index < 0 || index >= (INT32)ARRAY_SIZE(io_caps_memory_types)) return AMF_INVALID_ARG;
    if (type) *type = io_caps_memory_types[index];
    if (native) *native = io_caps_memory_types[index] != AMF_MEMORY_HOST;
    return AMF_OK;
}

static BYTE WINAPI io_caps_IsInterlacedSupported(void *iface)
{
    return FALSE;
}

static const GUID *const io_caps_iids[] = {&amf_iid_interface, NULL};

static const struct object_ops io_caps_ops = {"io caps", io_caps_iids, NULL};

static const struct amf_io_caps_vtbl io_caps_vtbl =
{
    AMF_INTERFACE_IMPL
    .GetWidthRange = io_caps_GetWidthRange,
    .GetHeightRange = io_caps_GetHeightRange,
    .GetVertAlign = io_caps_GetVertAlign,
    .GetNumOfFormats = io_caps_GetNumOfFormats,
    .GetFormatAt = io_caps_GetFormatAt,
    .GetNumOfMemoryTypes = io_caps_GetNumOfMemoryTypes,
    .GetMemoryTypeAt = io_caps_GetMemoryTypeAt,
    .IsInterlacedSupported = io_caps_IsInterlacedSupported,
};

struct amf_caps
{
    struct amf_object obj;
    const struct codec_desc *desc;
    vkenc_caps caps;
};

struct amf_caps_vtbl
{
    AMF_INTERFACE_SLOTS
    AMF_PROPERTY_STORAGE_SLOTS
    INT32      (WINAPI *GetAccelerationType)(void *iface);
    AMF_RESULT (WINAPI *GetInputCaps)(void *iface, void **out);
    AMF_RESULT (WINAPI *GetOutputCaps)(void *iface, void **out);
};
C_ASSERT(offsetof(struct amf_caps_vtbl, GetInputCaps) == 0x70);

static INT32 WINAPI caps_GetAccelerationType(void *iface)
{
    return AMF_ACCEL_HARDWARE;
}

static AMF_RESULT caps_create_io(struct amf_caps *caps, BOOL input, void **out)
{
    struct amf_io_caps *io;

    if (!out) return AMF_INVALID_POINTER;
    *out = NULL;
    if (!(io = calloc(1, sizeof(*io)))) return AMF_OUT_OF_MEMORY;
    object_init(&io->obj, &io_caps_vtbl, &io_caps_ops);
    io->min_width = caps->caps.min_width;
    io->max_width = caps->caps.max_width;
    io->min_height = caps->caps.min_height;
    io->max_height = caps->caps.max_height;
    if (input)
    {
        io->formats[io->format_count++] = AMF_SURFACE_NV12;
        if (caps->desc->codec == VKENC_CODEC_HEVC && caps->caps.supports_10bit)
            io->formats[io->format_count++] = AMF_SURFACE_P010;
    }
    *out = io;
    return AMF_OK;
}

static AMF_RESULT WINAPI caps_GetInputCaps(void *iface, void **out)
{
    TRACE("caps %p, out %p\n", iface, out);
    return caps_create_io(iface, TRUE, out);
}

static AMF_RESULT WINAPI caps_GetOutputCaps(void *iface, void **out)
{
    TRACE("caps %p, out %p\n", iface, out);
    return caps_create_io(iface, FALSE, out);
}

static const GUID *const caps_iids[] = {&amf_iid_interface, &amf_iid_property_storage, NULL};

static const struct object_ops caps_ops = {"caps", caps_iids, NULL};

static const struct amf_caps_vtbl caps_vtbl =
{
    AMF_INTERFACE_IMPL
    .SetProperty = object_SetProperty,
    AMF_PROPERTY_STORAGE_IMPL
    .GetAccelerationType = caps_GetAccelerationType,
    .GetInputCaps = caps_GetInputCaps,
    .GetOutputCaps = caps_GetOutputCaps,
};

static AMF_RESULT caps_create(const struct codec_desc *desc, const vkenc_caps *vkcaps, void **out)
{
    struct amf_caps *caps;

    if (!(caps = calloc(1, sizeof(*caps)))) return AMF_OUT_OF_MEMORY;
    object_init(&caps->obj, &caps_vtbl, &caps_ops);
    caps->desc = desc;
    caps->caps = *vkcaps;
    store_set_int(&caps->obj.store, desc->cap_max_bitrate, caps_max_bitrate(vkcaps));
    store_set_int(&caps->obj.store, desc->cap_num_streams, 16);
    if (desc->codec == VKENC_CODEC_H264)
    {
        store_set_int(&caps->obj.store, desc->cap_max_profile, 100);
        store_set_int(&caps->obj.store, desc->cap_max_level, 52);
        store_set_bool(&caps->obj.store, L"BFrames", FALSE);
    }
    else
    {
        store_set_int(&caps->obj.store, desc->cap_max_profile, vkcaps->supports_10bit ? 2 : 1);
        store_set_int(&caps->obj.store, desc->cap_max_level, 186);
    }
    *out = caps;
    return AMF_OK;
}

/* -------------------------------------------------------------------------------------------------
 * AMFComponent: the H.264 and HEVC encoders
 *
 * Adobe's pattern (one component per codec, created by its startup probe and reused for every export):
 *   SetProperty xN, GetPropertyInfo(bitrate), GetCaps, Init; then SubmitInput from the export thread
 *   while a poll thread calls QueryOutput every millisecond; Drain; Terminate. The next export calls
 *   SetProperty and Init again on the same component; teardown calls Terminate a second time.
 *
 * Locking: state_lock is held exclusive by Init, ReInit, Flush and Terminate (which create and destroy
 * the vkenc encoder) and shared by everything that uses it. vkenc_submit and vkenc_receive are
 * thread-safe with respect to each other, so they run under separate locks; vkenc.h does not say that
 * of vkenc_drain, so Drain takes both.
 */

struct pts_entry
{
    INT64 pts, duration;
};

struct amf_component
{
    struct amf_object obj;
    const struct codec_desc *desc;
    void *context;                  /* acquired */
    vkenc_caps caps;
    amf_property_info info[2];      /* target and peak bit rate */
    SRWLOCK state_lock;
    SRWLOCK submit_lock;
    SRWLOCK receive_lock;
    SRWLOCK pts_lock;
    vkenc *enc;
    vkenc_params params;
    LONG drained;                   /* vkenc_drain returned: every frame can now be received */
    LONG64 submitted, received;
    struct pts_entry *pts;          /* ring of the submitted frames' timestamps, in submission order */
    SIZE_T pts_head, pts_count, pts_size;
};

struct amf_component_vtbl
{
    AMF_INTERFACE_SLOTS
    AMF_PROPERTY_STORAGE_SLOTS
    /* AMFPropertyStorageEx; MSVC puts the two GetPropertyInfo overloads in reverse declaration order */
    SIZE_T     (WINAPI *GetPropertiesInfoCount)(void *iface);
    AMF_RESULT (WINAPI *GetPropertyInfo)(void *iface, const WCHAR *name, const amf_property_info **info);
    AMF_RESULT (WINAPI *GetPropertyInfoAt)(void *iface, SIZE_T index, const amf_property_info **info);
    AMF_RESULT (WINAPI *ValidateProperty)(void *iface, const WCHAR *name, const amf_variant *value,
                                          amf_variant *validated);
    /* AMFComponent */
    AMF_RESULT (WINAPI *Init)(void *iface, INT32 format, INT32 width, INT32 height);
    AMF_RESULT (WINAPI *ReInit)(void *iface, INT32 width, INT32 height);
    AMF_RESULT (WINAPI *Terminate)(void *iface);
    AMF_RESULT (WINAPI *Drain)(void *iface);
    AMF_RESULT (WINAPI *Flush)(void *iface);
    AMF_RESULT (WINAPI *SubmitInput)(void *iface, void *data);
    AMF_RESULT (WINAPI *QueryOutput)(void *iface, void **data);
    void *     (WINAPI *GetContext)(void *iface);
    AMF_RESULT (WINAPI *SetOutputDataAllocatorCB)(void *iface, void *callback);
    AMF_RESULT (WINAPI *GetCaps)(void *iface, void **caps);
    AMF_RESULT (WINAPI *Optimize)(void *iface, void *callback);
};
C_ASSERT(offsetof(struct amf_component_vtbl, SetProperty) == 0x18);
C_ASSERT(offsetof(struct amf_component_vtbl, GetPropertyInfo) == 0x70);
C_ASSERT(offsetof(struct amf_component_vtbl, GetPropertyInfoAt) == 0x78);
C_ASSERT(offsetof(struct amf_component_vtbl, Init) == 0x88);
C_ASSERT(offsetof(struct amf_component_vtbl, ReInit) == 0x90);
C_ASSERT(offsetof(struct amf_component_vtbl, Terminate) == 0x98);
C_ASSERT(offsetof(struct amf_component_vtbl, Drain) == 0xa0);
C_ASSERT(offsetof(struct amf_component_vtbl, SubmitInput) == 0xb0);
C_ASSERT(offsetof(struct amf_component_vtbl, QueryOutput) == 0xb8);
C_ASSERT(offsetof(struct amf_component_vtbl, GetCaps) == 0xd0);
C_ASSERT(offsetof(struct amf_component_vtbl, Optimize) == 0xd8);

static void * WINAPI property_info_destructor(amf_property_info *info, unsigned int flags)
{
    /* The infos live inside their component; there is nothing to delete. */
    return info;
}

static const struct property_info_vtbl property_info_vtbl = {property_info_destructor};

static void component_init_info(struct amf_component *comp)
{
    INT64 max = caps_max_bitrate(&comp->caps);
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(comp->info); i++)
    {
        amf_property_info *info = &comp->info[i];

        info->vtbl = &property_info_vtbl;
        info->name = i ? comp->desc->peak_bitrate : comp->desc->target_bitrate;
        info->desc = i ? L"Peak bit rate in bits" : L"Target bit rate in bits";
        info->type = AMF_VARIANT_INT64;
        info->content_type = AMF_PROPERTY_CONTENT_DEFAULT;
        info->default_value.type = AMF_VARIANT_INT64;
        info->default_value.u.int64Value = i ? 30000000 : 20000000;
        if (info->default_value.u.int64Value > max) info->default_value.u.int64Value = max;
        info->min_value.type = AMF_VARIANT_INT64;
        info->min_value.u.int64Value = 0;
        info->max_value.type = AMF_VARIANT_INT64;
        info->max_value.u.int64Value = max;
        info->access_type = AMF_PROPERTY_ACCESS_FULL;
    }
}

static void pts_reset(struct amf_component *comp)
{
    AcquireSRWLockExclusive(&comp->pts_lock);
    comp->pts_head = comp->pts_count = 0;
    ReleaseSRWLockExclusive(&comp->pts_lock);
}

static BOOL pts_push(struct amf_component *comp, INT64 pts, INT64 duration)
{
    BOOL ret = TRUE;

    AcquireSRWLockExclusive(&comp->pts_lock);
    if (comp->pts_count == comp->pts_size)
    {
        SIZE_T size = comp->pts_size ? comp->pts_size * 2 : 64, i;
        struct pts_entry *ring = malloc(size * sizeof(*ring));

        if (!ring) ret = FALSE;
        else
        {
            for (i = 0; i < comp->pts_count; i++)
                ring[i] = comp->pts[(comp->pts_head + i) % comp->pts_size];
            free(comp->pts);
            comp->pts = ring;
            comp->pts_size = size;
            comp->pts_head = 0;
        }
    }
    if (ret)
    {
        struct pts_entry *e = &comp->pts[(comp->pts_head + comp->pts_count++) % comp->pts_size];
        e->pts = pts;
        e->duration = duration;
    }
    ReleaseSRWLockExclusive(&comp->pts_lock);
    return ret;
}

/* Takes back the entry pts_push just added, when vkenc_submit did not accept the frame. */
static void pts_unpush(struct amf_component *comp)
{
    AcquireSRWLockExclusive(&comp->pts_lock);
    if (comp->pts_count) comp->pts_count--;
    ReleaseSRWLockExclusive(&comp->pts_lock);
}

static struct pts_entry pts_pop(struct amf_component *comp)
{
    struct pts_entry e = {0, 0};

    AcquireSRWLockExclusive(&comp->pts_lock);
    if (comp->pts_count)
    {
        e = comp->pts[comp->pts_head];
        comp->pts_head = (comp->pts_head + 1) % comp->pts_size;
        comp->pts_count--;
    }
    ReleaseSRWLockExclusive(&comp->pts_lock);
    return e;
}

/* ISO/IEC 23091-2 code points from AMF's color properties; AMF uses 0 for "undefined". */
static int color_code(INT64 value)
{
    return value > 0 && value < 256 ? (int)value : -1;
}

/* Builds the vkenc parameters from the properties set so far, as an AMF encoder does at Init. */
static AMF_RESULT component_build_params(struct amf_component *comp, INT32 format, INT32 width, INT32 height,
                                         vkenc_params *params)
{
    const struct codec_desc *desc = comp->desc;
    struct property_store *store = &comp->obj.store;
    UINT32 a, b;
    INT64 value, method, profile_value;

    memset(params, 0, sizeof(*params));
    params->codec = desc->codec;

    if (format != AMF_SURFACE_NV12 && format != AMF_SURFACE_P010)
    {
        ERR("%s: surface format %d is not supported (NV12 or P010 only)\n", desc->name, format);
        return AMF_SURFACE_FORMAT_NOT_SUPPORTED;
    }
    if (format == AMF_SURFACE_P010 && (desc->codec != VKENC_CODEC_HEVC || !comp->caps.supports_10bit))
    {
        ERR("%s: 10-bit (P010) input is not supported on %s\n", desc->name, comp->caps.device_name);
        return AMF_SURFACE_FORMAT_NOT_SUPPORTED;
    }
    params->format = format == AMF_SURFACE_P010 ? VKENC_FMT_P010 : VKENC_FMT_NV12;

    if (width <= 0 || height <= 0)
    {
        ERR("%s: invalid size %dx%d\n", desc->name, width, height);
        return AMF_INVALID_RESOLUTION;
    }
    if ((comp->caps.max_width && ((UINT32)width < comp->caps.min_width || (UINT32)width > comp->caps.max_width))
            || (comp->caps.max_height && ((UINT32)height < comp->caps.min_height
                                          || (UINT32)height > comp->caps.max_height)))
    {
        ERR("%s: %dx%d is outside what %s encodes (%ux%u to %ux%u)\n", desc->name, width, height,
            comp->caps.device_name, comp->caps.min_width, comp->caps.min_height, comp->caps.max_width,
            comp->caps.max_height);
        return AMF_INVALID_RESOLUTION;
    }
    params->width = width;
    params->height = height;
    if (store_get_pair(store, desc->frame_size, &a, &b) && ((INT32)a != width || (INT32)b != height))
        WARN("%s: %s is %dx%d but Init asks for %dx%d; using Init's\n", desc->name, debugstr_w(desc->frame_size),
             (INT32)a, (INT32)b, width, height);

    if (store_get_pair(store, desc->frame_rate, &a, &b))
    {
        if (!a || !b)
        {
            ERR("%s: invalid frame rate %u/%u\n", desc->name, a, b);
            return AMF_INVALID_ARG;
        }
        params->fps_num = a;
        params->fps_den = b;
    }
    else
    {
        params->fps_num = 30;
        params->fps_den = 1;
    }
    if (store_get_pair(store, desc->aspect_ratio, &a, &b) && a && b)
    {
        params->sar_num = a;
        params->sar_den = b;
    }

    /* Rate control. AMF's default for the transcoding usage is peak-constrained VBR. */
    method = store_int(store, desc->rate_control, 2);
    if (method == desc->rc_cbr || method == 6 /* HIGH_QUALITY_CBR */) params->rc = VKENC_RC_CBR;
    else
    {
        if (method != 2) FIXME("%s: rate control method %lld is encoded as peak-constrained VBR\n", desc->name,
                               (long long)method);
        params->rc = VKENC_RC_VBR;
    }
    value = store_int(store, desc->target_bitrate, 20000000);
    params->target_bitrate = value > 0 ? (UINT64)value : 20000000;
    value = store_int(store, desc->peak_bitrate, 0);
    if (params->rc == VKENC_RC_CBR) params->peak_bitrate = params->target_bitrate;
    else params->peak_bitrate = value > 0 ? (UINT64)value : params->target_bitrate * 3 / 2;
    if (params->peak_bitrate < params->target_bitrate) params->peak_bitrate = params->target_bitrate;
    /* Filler data only means something in CBR; a CBR export leaves it set for the next VBR one. */
    params->filler_data = params->rc == VKENC_RC_CBR && store_int(store, desc->filler_data, 0);

    /* GOP structure. */
    if (desc->codec == VKENC_CODEC_H264)
    {
        value = store_int(store, desc->idr_period, 0);
        params->idr_period = value > 0 && value <= UINT32_MAX ? (UINT32)value : 0;
        value = store_int(store, desc->header_spacing, 0);
        params->header_spacing = value > 0 && value <= UINT32_MAX ? (UINT32)value : 0;
    }
    else
    {
        INT64 gop = store_int(store, desc->gop_size, 60), per_idr = store_int(store, desc->gops_per_idr, 1);

        if (per_idr <= 0)
        {
            FIXME("%s: %s %lld (no IDR after the first) is not supported; using the default IDR period\n",
                  desc->name, debugstr_w(desc->gops_per_idr), (long long)per_idr);
            params->idr_period = 0;
        }
        else if (gop > 0 && gop * per_idr <= UINT32_MAX) params->idr_period = (UINT32)(gop * per_idr);
        /* Parameter sets go out with every IDR, which covers every HevcHeaderInsertionMode. */
        params->header_spacing = 0;
    }

    /* Profile, level, tier. */
    if (desc->codec == VKENC_CODEC_H264)
    {
        profile_value = store_int(store, desc->profile, 77);
        if (profile_value == 256) profile_value = 66;   /* constrained baseline */
        if (profile_value == 257) profile_value = 100;  /* constrained high */
        params->profile = (int)profile_value;
        params->level = (int)store_int(store, desc->level, 42);
        params->interlaced = store_int(store, desc->scan_type, 0) == 1;
    }
    else
    {
        int want = format == AMF_SURFACE_P010 ? 2 : 1;

        profile_value = store_int(store, desc->profile, want);
        /* One HEVC component serves Main and Main10 exports; the input format decides. */
        if (profile_value != want)
            WARN("%s: %s %lld does not match %s input; using %d\n", desc->name, debugstr_w(desc->profile),
                 (long long)profile_value, want == 2 ? "P010" : "NV12", want);
        params->profile = want;
        params->level = (int)store_int(store, desc->level, 153);
        params->tier = store_int(store, desc->tier, 0) == 1;
    }

    /* VUI color description. */
    params->color_primaries = color_code(store_int(store, desc->out_primaries, 0));
    params->transfer_characteristics = color_code(store_int(store, desc->out_transfer, 0));
    params->matrix_coefficients = -1;
    params->full_range = desc->out_full_range && store_int(store, desc->out_full_range, 0) == 1;
    if (desc->out_matrix && store_get_int(store, desc->out_matrix, &value) && value >= 0 && value < 256)
        params->matrix_coefficients = (int)value;
    else
    {
        switch (store_int(store, desc->out_color_profile, AMF_COLOR_PROFILE_UNKNOWN))
        {
        case AMF_COLOR_PROFILE_FULL_601: params->full_range = 1; /* fall through */
        case AMF_COLOR_PROFILE_601: params->matrix_coefficients = 6; break;
        case AMF_COLOR_PROFILE_FULL_709: params->full_range = 1; /* fall through */
        case AMF_COLOR_PROFILE_709: params->matrix_coefficients = 1; break;
        case AMF_COLOR_PROFILE_FULL_2020: params->full_range = 1; /* fall through */
        case AMF_COLOR_PROFILE_2020: params->matrix_coefficients = 9; break;
        }
    }
    return AMF_OK;
}

static void trace_params(const struct amf_component *comp, const vkenc_params *p)
{
    TRACE("%s: %s %ux%u %u/%u fps, SAR %u:%u, %s %llu/%llu bps%s, IDR %u, headers %u, profile %d level %d "
          "tier %d%s, VUI %d/%d/%d%s\n", comp->desc->name, p->format == VKENC_FMT_P010 ? "P010" : "NV12",
          p->width, p->height, p->fps_num, p->fps_den, p->sar_num, p->sar_den,
          p->rc == VKENC_RC_CBR ? "CBR" : "VBR", (unsigned long long)p->target_bitrate,
          (unsigned long long)p->peak_bitrate, p->filler_data ? " +filler" : "", p->idr_period, p->header_spacing,
          p->profile, p->level, p->tier, p->interlaced ? " interlaced" : "", p->color_primaries,
          p->transfer_characteristics, p->matrix_coefficients, p->full_range ? " full range" : "");
}

/* Called with state_lock held exclusive. */
static AMF_RESULT component_start(struct amf_component *comp, const vkenc_params *params)
{
    char err[256] = "";
    vkenc *enc = NULL;

    trace_params(comp, params);
    if (vkenc_create(params, &enc, err, sizeof(err)) < 0 || !enc)
    {
        err[sizeof(err) - 1] = 0;
        ERR("%s: the encoder could not be created: %s\n", comp->desc->name, err[0] ? err : "(no reason given)");
        return AMF_FAIL;
    }
    comp->enc = enc;
    comp->params = *params;
    comp->drained = 0;
    comp->submitted = comp->received = 0;
    pts_reset(comp);
    return AMF_OK;
}

/* Called with state_lock held exclusive. */
static void component_stop(struct amf_component *comp)
{
    if (!comp->enc) return;
    TRACE("%s %p: %lld frames in, %lld out\n", comp->desc->name, comp, (long long)comp->submitted,
          (long long)comp->received);
    vkenc_destroy(comp->enc);
    comp->enc = NULL;
    comp->drained = 0;
    comp->submitted = comp->received = 0;
    pts_reset(comp);
}

static AMF_RESULT WINAPI component_SetProperty(void *iface, const WCHAR *name, const amf_variant *value)
{
    struct amf_component *comp = iface;

    if (name && value && comp->desc->color_bit_depth && !amf_wcscmp(name, comp->desc->color_bit_depth)
            && value->type == AMF_VARIANT_INT64 && value->u.int64Value == 10
            && (comp->desc->codec != VKENC_CODEC_HEVC || !comp->caps.supports_10bit))
    {
        WARN("%s: 10-bit encoding is not supported on %s\n", comp->desc->name, comp->caps.device_name);
        return AMF_NOT_SUPPORTED;
    }
    return object_SetProperty(iface, name, value);
}

static SIZE_T WINAPI component_GetPropertiesInfoCount(void *iface)
{
    struct amf_component *comp = iface;

    TRACE("%s %p\n", comp->desc->name, comp);
    return ARRAY_SIZE(comp->info);
}

static AMF_RESULT WINAPI component_GetPropertyInfo(void *iface, const WCHAR *name, const amf_property_info **out)
{
    struct amf_component *comp = iface;
    unsigned int i;

    TRACE("%s %p, name %s, out %p\n", comp->desc->name, comp, debugstr_w(name), out);
    if (!out) return AMF_INVALID_POINTER;
    *out = NULL;
    if (!name) return AMF_INVALID_POINTER;
    for (i = 0; i < ARRAY_SIZE(comp->info); i++)
    {
        if (amf_wcscmp(comp->info[i].name, name)) continue;
        TRACE("max %lld\n", (long long)comp->info[i].max_value.u.int64Value);
        *out = &comp->info[i];
        return AMF_OK;
    }
    FIXME("%s: no property info for %s\n", comp->desc->name, debugstr_w(name));
    return AMF_NOT_FOUND;
}

static AMF_RESULT WINAPI component_GetPropertyInfoAt(void *iface, SIZE_T index, const amf_property_info **out)
{
    struct amf_component *comp = iface;

    TRACE("%s %p, index %u, out %p\n", comp->desc->name, comp, (unsigned int)index, out);
    if (!out) return AMF_INVALID_POINTER;
    *out = NULL;
    if (index >= ARRAY_SIZE(comp->info)) return AMF_INVALID_ARG;
    *out = &comp->info[index];
    return AMF_OK;
}

static AMF_RESULT WINAPI component_ValidateProperty(void *iface, const WCHAR *name, const amf_variant *value,
                                                    amf_variant *validated)
{
    TRACE("%p, %s = %s\n", iface, debugstr_w(name), debugstr_amf_variant(value));
    if (!name || !value || !validated) return AMF_INVALID_POINTER;
    return variant_copy(validated, value);
}

static AMF_RESULT WINAPI component_Init(void *iface, INT32 format, INT32 width, INT32 height)
{
    struct amf_component *comp = iface;
    vkenc_params params;
    AMF_RESULT ret;

    TRACE("%s %p, format %d, %dx%d\n", comp->desc->name, comp, format, width, height);
    AcquireSRWLockExclusive(&comp->state_lock);
    if (comp->enc)
    {
        WARN("%s %p: already initialized\n", comp->desc->name, comp);
        ret = AMF_ALREADY_INITIALIZED;
    }
    else if (!(ret = component_build_params(comp, format, width, height, &params)))
        ret = component_start(comp, &params);
    ReleaseSRWLockExclusive(&comp->state_lock);
    return ret;
}

static AMF_RESULT WINAPI component_ReInit(void *iface, INT32 width, INT32 height)
{
    struct amf_component *comp = iface;
    vkenc_params params;
    AMF_RESULT ret;

    TRACE("%s %p, %dx%d\n", comp->desc->name, comp, width, height);
    AcquireSRWLockExclusive(&comp->state_lock);
    if (!comp->enc) ret = AMF_NOT_INITIALIZED;
    else
    {
        params = comp->params;
        component_stop(comp);
        params.width = width;
        params.height = height;
        ret = component_start(comp, &params);
    }
    ReleaseSRWLockExclusive(&comp->state_lock);
    return ret;
}

static AMF_RESULT WINAPI component_Terminate(void *iface)
{
    struct amf_component *comp = iface;

    TRACE("%s %p\n", comp->desc->name, comp);
    /* Adobe terminates at the end of every export and again at teardown; both are fine. */
    AcquireSRWLockExclusive(&comp->state_lock);
    component_stop(comp);
    ReleaseSRWLockExclusive(&comp->state_lock);
    return AMF_OK;
}

static AMF_RESULT WINAPI component_Drain(void *iface)
{
    struct amf_component *comp = iface;
    AMF_RESULT ret = AMF_OK;
    int r;

    TRACE("%s %p\n", comp->desc->name, comp);
    AcquireSRWLockShared(&comp->state_lock);
    if (!comp->enc) ret = AMF_NOT_INITIALIZED;
    else if (!comp->drained)
    {
        AcquireSRWLockExclusive(&comp->submit_lock);
        AcquireSRWLockExclusive(&comp->receive_lock);
        if ((r = vkenc_drain(comp->enc)) < 0)
        {
            ERR("%s: vkenc_drain failed (%d)\n", comp->desc->name, r);
            ret = AMF_FAIL;
        }
        else InterlockedExchange(&comp->drained, 1);
        ReleaseSRWLockExclusive(&comp->receive_lock);
        ReleaseSRWLockExclusive(&comp->submit_lock);
        TRACE("%s %p: drained after %lld frames\n", comp->desc->name, comp, (long long)comp->submitted);
    }
    ReleaseSRWLockShared(&comp->state_lock);
    return ret;
}

static AMF_RESULT WINAPI component_Flush(void *iface)
{
    struct amf_component *comp = iface;
    vkenc_params params;
    AMF_RESULT ret = AMF_OK;

    TRACE("%s %p\n", comp->desc->name, comp);
    /* Drops everything pending; vkenc has no flush, so start a new encoder with the same parameters. */
    AcquireSRWLockExclusive(&comp->state_lock);
    if (comp->enc)
    {
        params = comp->params;
        component_stop(comp);
        ret = component_start(comp, &params);
    }
    ReleaseSRWLockExclusive(&comp->state_lock);
    return ret;
}

static AMF_RESULT WINAPI component_SubmitInput(void *iface, void *data)
{
    struct amf_component *comp = iface;
    struct amf_surface *surface;
    const BYTE *pixels;
    BYTE *temp = NULL;
    AMF_RESULT ret;
    INT32 pitch;
    INT64 force;
    int r;

    TRACE("%s %p, data %p\n", comp->desc->name, comp, data);
    if (!data) return AMF_INVALID_POINTER;
    if (!(surface = unsafe_surface(data)))
    {
        WARN("%s: %p is not one of our surfaces\n", comp->desc->name, data);
        return AMF_INVALID_DATA_TYPE;
    }

    AcquireSRWLockShared(&comp->state_lock);
    if (!comp->enc)
    {
        ret = AMF_NOT_INITIALIZED;
        goto done;
    }
    if (comp->drained)
    {
        WARN("%s: input after Drain\n", comp->desc->name);
        ret = AMF_EOF;
        goto done;
    }
    if ((surface->format == AMF_SURFACE_P010) != (comp->params.format == VKENC_FMT_P010))
    {
        ERR("%s: surface format %d does not match Init's\n", comp->desc->name, surface->format);
        ret = AMF_SURFACE_FORMAT_NOT_SUPPORTED;
        goto done;
    }
    if (surface->crop_width < (INT32)comp->params.width || surface->crop_height < (INT32)comp->params.height)
    {
        ERR("%s: %dx%d surface for a %ux%u encoder\n", comp->desc->name, surface->crop_width,
            surface->crop_height, comp->params.width, comp->params.height);
        ret = AMF_INVALID_RESOLUTION;
        goto done;
    }
    if (!(pixels = surface_frame(surface, comp->params.width, comp->params.height, &pitch, &temp)))
    {
        ERR("%s: out of memory\n", comp->desc->name);
        ret = AMF_OUT_OF_MEMORY;
        goto done;
    }
    force = store_int(&surface->obj.store, comp->desc->force_picture_type, 0);

    AcquireSRWLockExclusive(&comp->submit_lock);
    if (!pts_push(comp, surface->pts, surface->duration)) r = -1;
    else if ((r = vkenc_submit(comp->enc, pixels, pitch, force == AMF_PICTURE_TYPE_IDR)))
        pts_unpush(comp);
    else
        InterlockedIncrement64(&comp->submitted);
    ReleaseSRWLockExclusive(&comp->submit_lock);

    if (!r) ret = AMF_OK;
    else if (r == 1) ret = AMF_INPUT_FULL;
    else
    {
        ERR("%s: vkenc_submit failed (%d) on frame %lld\n", comp->desc->name, r, (long long)comp->submitted);
        ret = AMF_FAIL;
    }
    TRACE("%s %p: frame %lld -> %d\n", comp->desc->name, comp, (long long)comp->submitted, ret);

done:
    ReleaseSRWLockShared(&comp->state_lock);
    free(temp);
    return ret;
}

static AMF_RESULT WINAPI component_QueryOutput(void *iface, void **out)
{
    struct amf_component *comp = iface;
    struct amf_buffer *buffer;
    struct pts_entry pts = {0, 0};
    uint8_t *data = NULL;
    size_t size = 0;
    AMF_RESULT ret;
    int r, is_idr = 0;
    LONG drained;

    if (!out) return AMF_INVALID_POINTER;
    *out = NULL;

    AcquireSRWLockShared(&comp->state_lock);
    if (!comp->enc)
    {
        ReleaseSRWLockShared(&comp->state_lock);
        return AMF_NOT_INITIALIZED;
    }
    /* Read before receiving: once drained is set, an empty receive means the stream is complete. */
    drained = comp->drained;
    AcquireSRWLockExclusive(&comp->receive_lock);
    r = vkenc_receive(comp->enc, &data, &size, &is_idr);
    if (r == 1)
    {
        pts = pts_pop(comp);
        InterlockedIncrement64(&comp->received);
    }
    ReleaseSRWLockExclusive(&comp->receive_lock);

    if (r == 1)
    {
        if (!(buffer = buffer_create(data, size, vkenc_free_packet)))
        {
            ERR("%s: out of memory, access unit %lld lost\n", comp->desc->name, (long long)comp->received);
            vkenc_free_packet(data);
            ret = AMF_OUT_OF_MEMORY;
        }
        else
        {
            buffer->pts = pts.pts;
            buffer->duration = pts.duration;
            store_set_int(&buffer->obj.store, comp->desc->output_data_type,
                          is_idr ? AMF_OUTPUT_DATA_TYPE_IDR : AMF_OUTPUT_DATA_TYPE_P);
            TRACE("%s %p: access unit %lld, %u bytes%s -> buffer %p\n", comp->desc->name, comp,
                  (long long)comp->received, (unsigned int)size, is_idr ? ", IDR" : "", buffer);
            *out = buffer;
            ret = AMF_OK;
        }
    }
    else if (!r) ret = drained ? AMF_EOF : AMF_REPEAT;
    else
    {
        ERR("%s: vkenc_receive failed (%d)\n", comp->desc->name, r);
        ret = AMF_FAIL;
    }
    ReleaseSRWLockShared(&comp->state_lock);
    return ret;
}

static void * WINAPI component_GetContext(void *iface)
{
    struct amf_component *comp = iface;

    TRACE("%s %p\n", comp->desc->name, comp);
    return comp->context;
}

static AMF_RESULT WINAPI component_SetOutputDataAllocatorCB(void *iface, void *callback)
{
    TRACE("%p, callback %p\n", iface, callback);
    if (!callback) return AMF_OK;
    FIXME("output data allocators are not supported\n");
    return AMF_NOT_SUPPORTED;
}

static AMF_RESULT WINAPI component_GetCaps(void *iface, void **out)
{
    struct amf_component *comp = iface;

    TRACE("%s %p, out %p\n", comp->desc->name, comp, out);
    if (!out) return AMF_INVALID_POINTER;
    *out = NULL;
    /* Adobe skips its frame-size check when GetCaps fails, which beats checking against made-up limits. */
    if (!comp->caps.max_width || !comp->caps.max_height)
    {
        WARN("%s: the backend reported no size limits\n", comp->desc->name);
        return AMF_NOT_SUPPORTED;
    }
    return caps_create(comp->desc, &comp->caps, out);
}

static AMF_RESULT WINAPI component_Optimize(void *iface, void *callback)
{
    TRACE("%p, callback %p\n", iface, callback);
    return AMF_OK;
}

static void component_destroy(struct amf_object *object)
{
    struct amf_component *comp = (struct amf_component *)object;

    component_stop(comp);
    free(comp->pts);
    interface_release(comp->context);
}

static const GUID *const component_iids[] =
{
    &amf_iid_interface, &amf_iid_property_storage, &amf_iid_property_storage_ex, &amf_iid_component, NULL
};

static const struct object_ops component_ops = {"component", component_iids, component_destroy};

static const struct amf_component_vtbl component_vtbl =
{
    AMF_INTERFACE_IMPL
    .SetProperty = component_SetProperty,
    AMF_PROPERTY_STORAGE_IMPL
    .GetPropertiesInfoCount = component_GetPropertiesInfoCount,
    .GetPropertyInfo = component_GetPropertyInfo,
    .GetPropertyInfoAt = component_GetPropertyInfoAt,
    .ValidateProperty = component_ValidateProperty,
    .Init = component_Init,
    .ReInit = component_ReInit,
    .Terminate = component_Terminate,
    .Drain = component_Drain,
    .Flush = component_Flush,
    .SubmitInput = component_SubmitInput,
    .QueryOutput = component_QueryOutput,
    .GetContext = component_GetContext,
    .SetOutputDataAllocatorCB = component_SetOutputDataAllocatorCB,
    .GetCaps = component_GetCaps,
    .Optimize = component_Optimize,
};

static AMF_RESULT component_create(void *context, const struct codec_desc *desc, const vkenc_caps *caps,
                                   void **out)
{
    struct amf_component *comp;

    if (!(comp = calloc(1, sizeof(*comp)))) return AMF_OUT_OF_MEMORY;
    object_init(&comp->obj, &component_vtbl, &component_ops);
    comp->desc = desc;
    comp->caps = *caps;
    InitializeSRWLock(&comp->state_lock);
    InitializeSRWLock(&comp->submit_lock);
    InitializeSRWLock(&comp->receive_lock);
    InitializeSRWLock(&comp->pts_lock);
    component_init_info(comp);
    if ((comp->context = context)) interface_acquire(context);
    *out = comp;
    return AMF_OK;
}

/* -------------------------------------------------------------------------------------------------
 * AMFDebug and AMFTrace: process-wide objects without reference counting. Adobe uses both without a
 * NULL check, so they are real objects; nothing in this runtime writes AMF traces.
 */

struct amf_debug_vtbl
{
    void (WINAPI *EnablePerformanceMonitor)(void *iface, BYTE enable);
    BYTE (WINAPI *PerformanceMonitorEnabled)(void *iface);
    void (WINAPI *AssertsEnable)(void *iface, BYTE enable);
    BYTE (WINAPI *AssertsEnabled)(void *iface);
};
C_ASSERT(offsetof(struct amf_debug_vtbl, AssertsEnable) == 0x10);

static LONG debug_perf_monitor, debug_asserts = 1;

static void WINAPI debug_EnablePerformanceMonitor(void *iface, BYTE enable)
{
    TRACE("%u\n", enable);
    InterlockedExchange(&debug_perf_monitor, !!enable);
}

static BYTE WINAPI debug_PerformanceMonitorEnabled(void *iface)
{
    return debug_perf_monitor;
}

static void WINAPI debug_AssertsEnable(void *iface, BYTE enable)
{
    TRACE("%u\n", enable);
    InterlockedExchange(&debug_asserts, !!enable);
}

static BYTE WINAPI debug_AssertsEnabled(void *iface)
{
    return debug_asserts;
}

static const struct amf_debug_vtbl debug_vtbl =
{
    debug_EnablePerformanceMonitor,
    debug_PerformanceMonitorEnabled,
    debug_AssertsEnable,
    debug_AssertsEnabled,
};

static struct { const struct amf_debug_vtbl *vtbl; } amf_debug = {&debug_vtbl};

struct amf_trace_vtbl
{
    void         (WINAPI *TraceW)(void *iface, const WCHAR *path, INT32 line, INT32 level, const WCHAR *scope,
                                  INT32 count, const WCHAR *format, ...);
    void         (WINAPI *Trace)(void *iface, const WCHAR *path, INT32 line, INT32 level, const WCHAR *scope,
                                 const WCHAR *message, void *args);
    INT32        (WINAPI *SetGlobalLevel)(void *iface, INT32 level);
    INT32        (WINAPI *GetGlobalLevel)(void *iface);
    BYTE         (WINAPI *EnableWriter)(void *iface, const WCHAR *writer, BYTE enable);
    BYTE         (WINAPI *WriterEnabled)(void *iface, const WCHAR *writer);
    AMF_RESULT   (WINAPI *TraceEnableAsync)(void *iface, BYTE enable);
    AMF_RESULT   (WINAPI *TraceFlush)(void *iface);
    AMF_RESULT   (WINAPI *SetPath)(void *iface, const WCHAR *path);
    AMF_RESULT   (WINAPI *GetPath)(void *iface, WCHAR *path, SIZE_T *size);
    INT32        (WINAPI *SetWriterLevel)(void *iface, const WCHAR *writer, INT32 level);
    INT32        (WINAPI *GetWriterLevel)(void *iface, const WCHAR *writer);
    INT32        (WINAPI *SetWriterLevelForScope)(void *iface, const WCHAR *writer, const WCHAR *scope,
                                                  INT32 level);
    INT32        (WINAPI *GetWriterLevelForScope)(void *iface, const WCHAR *writer, const WCHAR *scope);
    INT32        (WINAPI *GetIndentation)(void *iface);
    void         (WINAPI *Indent)(void *iface, INT32 add);
    void         (WINAPI *RegisterWriter)(void *iface, const WCHAR *writer, void *object, BYTE enable);
    void         (WINAPI *UnregisterWriter)(void *iface, const WCHAR *writer);
    const WCHAR *(WINAPI *GetResultText)(void *iface, AMF_RESULT result);
    const WCHAR *(WINAPI *SurfaceGetFormatName)(void *iface, INT32 format);
    INT32        (WINAPI *SurfaceGetFormatByName)(void *iface, const WCHAR *name);
    const WCHAR *(WINAPI *GetMemoryTypeName)(void *iface, INT32 type);
    INT32        (WINAPI *GetMemoryTypeByName)(void *iface, const WCHAR *name);
    const WCHAR *(WINAPI *GetSampleFormatName)(void *iface, INT32 format);
    INT32        (WINAPI *GetSampleFormatByName)(void *iface, const WCHAR *name);
};
C_ASSERT(offsetof(struct amf_trace_vtbl, SetGlobalLevel) == 0x10);
C_ASSERT(offsetof(struct amf_trace_vtbl, EnableWriter) == 0x20);
C_ASSERT(sizeof(struct amf_trace_vtbl) == 25 * sizeof(void *));

#define AMF_TRACE_INFO 2

static LONG trace_level = AMF_TRACE_INFO, trace_indent;

/* The SDK's three standard writers: Console, DebugOutput, File. */
static const WCHAR *const trace_writers[] = {L"Console", L"DebugOutput", L"File"};
static LONG trace_writer_enabled[ARRAY_SIZE(trace_writers)];
static LONG trace_writer_level[ARRAY_SIZE(trace_writers)] = {AMF_TRACE_INFO, AMF_TRACE_INFO, AMF_TRACE_INFO};
static WCHAR trace_path[MAX_PATH];
static SRWLOCK trace_path_lock = SRWLOCK_INIT;

static int trace_writer_index(const WCHAR *writer)
{
    unsigned int i;

    if (!writer) return -1;
    for (i = 0; i < ARRAY_SIZE(trace_writers); i++)
        if (!amf_wcscmp(trace_writers[i], writer)) return i;
    return -1;
}

static void WINAPI trace_TraceW(void *iface, const WCHAR *path, INT32 line, INT32 level, const WCHAR *scope,
                                INT32 count, const WCHAR *format, ...)
{
    TRACE("%s: %s\n", debugstr_w(scope), debugstr_w(format));
}

static void WINAPI trace_Trace(void *iface, const WCHAR *path, INT32 line, INT32 level, const WCHAR *scope,
                               const WCHAR *message, void *args)
{
    TRACE("%s: %s\n", debugstr_w(scope), debugstr_w(message));
}

static INT32 WINAPI trace_SetGlobalLevel(void *iface, INT32 level)
{
    TRACE("%d\n", level);
    return InterlockedExchange(&trace_level, level);
}

static INT32 WINAPI trace_GetGlobalLevel(void *iface)
{
    return trace_level;
}

static BYTE WINAPI trace_EnableWriter(void *iface, const WCHAR *writer, BYTE enable)
{
    int i = trace_writer_index(writer);

    TRACE("%s, %u\n", debugstr_w(writer), enable);
    if (i < 0) return FALSE;
    return InterlockedExchange(&trace_writer_enabled[i], !!enable);
}

static BYTE WINAPI trace_WriterEnabled(void *iface, const WCHAR *writer)
{
    int i = trace_writer_index(writer);

    return i < 0 ? FALSE : trace_writer_enabled[i];
}

static AMF_RESULT WINAPI trace_TraceEnableAsync(void *iface, BYTE enable)
{
    TRACE("%u\n", enable);
    return AMF_OK;
}

static AMF_RESULT WINAPI trace_TraceFlush(void *iface)
{
    return AMF_OK;
}

static AMF_RESULT WINAPI trace_SetPath(void *iface, const WCHAR *path)
{
    TRACE("%s\n", debugstr_w(path));
    if (!path) return AMF_INVALID_POINTER;
    if (amf_wcslen(path) >= ARRAY_SIZE(trace_path)) return AMF_INVALID_ARG;
    AcquireSRWLockExclusive(&trace_path_lock);
    memcpy(trace_path, path, (amf_wcslen(path) + 1) * sizeof(WCHAR));
    ReleaseSRWLockExclusive(&trace_path_lock);
    return AMF_OK;
}

static AMF_RESULT WINAPI trace_GetPath(void *iface, WCHAR *path, SIZE_T *size)
{
    AMF_RESULT ret = AMF_OK;
    SIZE_T len;

    if (!size) return AMF_INVALID_POINTER;
    AcquireSRWLockExclusive(&trace_path_lock);
    len = amf_wcslen(trace_path) + 1;
    if (path && *size >= len) memcpy(path, trace_path, len * sizeof(WCHAR));
    else if (path) ret = AMF_INVALID_ARG;
    *size = len;
    ReleaseSRWLockExclusive(&trace_path_lock);
    return ret;
}

static INT32 WINAPI trace_SetWriterLevel(void *iface, const WCHAR *writer, INT32 level)
{
    int i = trace_writer_index(writer);

    TRACE("%s, %d\n", debugstr_w(writer), level);
    if (i < 0) return AMF_TRACE_INFO;
    return InterlockedExchange(&trace_writer_level[i], level);
}

static INT32 WINAPI trace_GetWriterLevel(void *iface, const WCHAR *writer)
{
    int i = trace_writer_index(writer);

    return i < 0 ? AMF_TRACE_INFO : trace_writer_level[i];
}

static INT32 WINAPI trace_SetWriterLevelForScope(void *iface, const WCHAR *writer, const WCHAR *scope,
                                                 INT32 level)
{
    TRACE("%s, %s, %d\n", debugstr_w(writer), debugstr_w(scope), level);
    return trace_GetWriterLevel(iface, writer);
}

static INT32 WINAPI trace_GetWriterLevelForScope(void *iface, const WCHAR *writer, const WCHAR *scope)
{
    return trace_GetWriterLevel(iface, writer);
}

static INT32 WINAPI trace_GetIndentation(void *iface)
{
    return trace_indent;
}

static void WINAPI trace_Indent(void *iface, INT32 add)
{
    InterlockedExchangeAdd(&trace_indent, add);
}

static void WINAPI trace_RegisterWriter(void *iface, const WCHAR *writer, void *object, BYTE enable)
{
    TRACE("%s, %p, %u: this runtime writes no traces\n", debugstr_w(writer), object, enable);
}

static void WINAPI trace_UnregisterWriter(void *iface, const WCHAR *writer)
{
    TRACE("%s\n", debugstr_w(writer));
}

static const WCHAR * WINAPI trace_GetResultText(void *iface, AMF_RESULT result)
{
    if (result >= 0 && result < AMF_RESULT_COUNT) return result_text[result];
    return L"AMF_UNKNOWN_RESULT";
}

static const struct { INT32 value; const WCHAR *name; } surface_format_names[] =
{
    {AMF_SURFACE_UNKNOWN, L"UNKNOWN"}, {AMF_SURFACE_NV12, L"NV12"}, {2, L"YV12"}, {3, L"BGRA"}, {4, L"ARGB"},
    {5, L"RGBA"}, {6, L"GRAY8"}, {7, L"YUV420P"}, {8, L"U8V8"}, {9, L"YUY2"}, {AMF_SURFACE_P010, L"P010"},
    {11, L"RGBA_F16"}, {12, L"UYVY"}, {13, L"R10G10B10A2"}, {14, L"Y210"}, {15, L"AYUV"}, {16, L"Y410"},
    {17, L"Y416"}, {18, L"GRAY32"}, {19, L"P012"}, {20, L"P016"},
};

static const WCHAR * WINAPI trace_SurfaceGetFormatName(void *iface, INT32 format)
{
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(surface_format_names); i++)
        if (surface_format_names[i].value == format) return surface_format_names[i].name;
    return L"UNKNOWN";
}

static INT32 WINAPI trace_SurfaceGetFormatByName(void *iface, const WCHAR *name)
{
    unsigned int i;

    if (!name) return AMF_SURFACE_UNKNOWN;
    for (i = 0; i < ARRAY_SIZE(surface_format_names); i++)
        if (!amf_wcscmp(surface_format_names[i].name, name)) return surface_format_names[i].value;
    return AMF_SURFACE_UNKNOWN;
}

static const WCHAR *const memory_type_names[] =
{
    L"Unknown", L"Host", L"DX9", L"DX11", L"OpenCL", L"OpenGL", L"XV", L"Gralloc", L"ComputeDX9",
    L"ComputeDX11", L"Vulkan", L"DX12",
};
C_ASSERT(ARRAY_SIZE(memory_type_names) == AMF_MEMORY_LAST);

static const WCHAR * WINAPI trace_GetMemoryTypeName(void *iface, INT32 type)
{
    if (type >= 0 && type < AMF_MEMORY_LAST) return memory_type_names[type];
    return memory_type_names[0];
}

static INT32 WINAPI trace_GetMemoryTypeByName(void *iface, const WCHAR *name)
{
    unsigned int i;

    if (!name) return AMF_MEMORY_UNKNOWN;
    for (i = 0; i < ARRAY_SIZE(memory_type_names); i++)
        if (!amf_wcscmp(memory_type_names[i], name)) return i;
    return AMF_MEMORY_UNKNOWN;
}

static const WCHAR * WINAPI trace_GetSampleFormatName(void *iface, INT32 format)
{
    return L"UNKNOWN";
}

static INT32 WINAPI trace_GetSampleFormatByName(void *iface, const WCHAR *name)
{
    return -1;
}

static const struct amf_trace_vtbl trace_vtbl =
{
    trace_TraceW,
    trace_Trace,
    trace_SetGlobalLevel,
    trace_GetGlobalLevel,
    trace_EnableWriter,
    trace_WriterEnabled,
    trace_TraceEnableAsync,
    trace_TraceFlush,
    trace_SetPath,
    trace_GetPath,
    trace_SetWriterLevel,
    trace_GetWriterLevel,
    trace_SetWriterLevelForScope,
    trace_GetWriterLevelForScope,
    trace_GetIndentation,
    trace_Indent,
    trace_RegisterWriter,
    trace_UnregisterWriter,
    trace_GetResultText,
    trace_SurfaceGetFormatName,
    trace_SurfaceGetFormatByName,
    trace_GetMemoryTypeName,
    trace_GetMemoryTypeByName,
    trace_GetSampleFormatName,
    trace_GetSampleFormatByName,
};

static struct { const struct amf_trace_vtbl *vtbl; } amf_trace = {&trace_vtbl};

/* -------------------------------------------------------------------------------------------------
 * AMFFactory: a process-wide object without reference counting, as in the SDK
 */

struct amf_factory_vtbl
{
    AMF_RESULT    (WINAPI *CreateContext)(void *iface, void **context);
    AMF_RESULT    (WINAPI *CreateComponent)(void *iface, void *context, const WCHAR *id, void **component);
    AMF_RESULT    (WINAPI *SetCacheFolder)(void *iface, const WCHAR *path);
    const WCHAR * (WINAPI *GetCacheFolder)(void *iface);
    AMF_RESULT    (WINAPI *GetDebug)(void *iface, void **debug);
    AMF_RESULT    (WINAPI *GetTrace)(void *iface, void **trace);
    AMF_RESULT    (WINAPI *GetPrograms)(void *iface, void **programs);
};
C_ASSERT(offsetof(struct amf_factory_vtbl, CreateComponent) == 0x08);
C_ASSERT(offsetof(struct amf_factory_vtbl, GetDebug) == 0x20);
C_ASSERT(offsetof(struct amf_factory_vtbl, GetTrace) == 0x28);

static WCHAR cache_folder[MAX_PATH];
static SRWLOCK cache_folder_lock = SRWLOCK_INIT;

static AMF_RESULT WINAPI factory_CreateContext(void *iface, void **out)
{
    AMF_RESULT ret;

    TRACE("out %p\n", out);
    if (!out) return AMF_INVALID_POINTER;
    *out = NULL;
    ret = context_create(out);
    TRACE("-> %d, context %p\n", ret, *out);
    return ret;
}

static AMF_RESULT WINAPI factory_CreateComponent(void *iface, void *context, const WCHAR *id, void **out)
{
    static LONG fixme_once;
    unsigned int i;
    AMF_RESULT ret;

    TRACE("context %p, id %s, out %p\n", context, debugstr_w(id), out);
    if (!out) return AMF_INVALID_POINTER;
    *out = NULL;
    if (!id) return AMF_INVALID_POINTER;
    for (i = 0; i < ARRAY_SIZE(codec_descs); i++)
    {
        if (amf_wcscmp(codec_descs[i]->id, id)) continue;
        if (!codec_caps[i].available)
        {
            ERR("%s: no hardware encoder: %s\n", codec_descs[i]->name, codec_caps[i].reason);
            return AMF_ENCODER_NOT_PRESENT;
        }
        ret = component_create(context, codec_descs[i], &codec_caps[i], out);
        TRACE("-> %d, %s component %p\n", ret, codec_descs[i]->name, *out);
        return ret;
    }
    /* Decoders and the converter are not implemented; Adobe falls back to software for those. */
    if (!InterlockedExchange(&fixme_once, 1)) FIXME("component %s is not supported\n", debugstr_w(id));
    else TRACE("component %s is not supported\n", debugstr_w(id));
    return AMF_NOT_SUPPORTED;
}

static AMF_RESULT WINAPI factory_SetCacheFolder(void *iface, const WCHAR *path)
{
    TRACE("%s\n", debugstr_w(path));
    if (path && amf_wcslen(path) >= ARRAY_SIZE(cache_folder)) return AMF_INVALID_ARG;
    AcquireSRWLockExclusive(&cache_folder_lock);
    if (path) memcpy(cache_folder, path, (amf_wcslen(path) + 1) * sizeof(WCHAR));
    else cache_folder[0] = 0;
    ReleaseSRWLockExclusive(&cache_folder_lock);
    return AMF_OK;
}

static const WCHAR * WINAPI factory_GetCacheFolder(void *iface)
{
    return cache_folder;
}

static AMF_RESULT WINAPI factory_GetDebug(void *iface, void **out)
{
    TRACE("out %p\n", out);
    if (!out) return AMF_INVALID_POINTER;
    *out = &amf_debug;
    return AMF_OK;
}

static AMF_RESULT WINAPI factory_GetTrace(void *iface, void **out)
{
    TRACE("out %p\n", out);
    if (!out) return AMF_INVALID_POINTER;
    *out = &amf_trace;
    return AMF_OK;
}

static AMF_RESULT WINAPI factory_GetPrograms(void *iface, void **out)
{
    FIXME("out %p: not supported\n", out);
    if (out) *out = NULL;
    return AMF_NOT_SUPPORTED;
}

static const struct amf_factory_vtbl factory_vtbl =
{
    factory_CreateContext,
    factory_CreateComponent,
    factory_SetCacheFolder,
    factory_GetCacheFolder,
    factory_GetDebug,
    factory_GetTrace,
    factory_GetPrograms,
};

static struct { const struct amf_factory_vtbl *vtbl; } amf_factory = {&factory_vtbl};

/* -------------------------------------------------------------------------------------------------
 * Gates and exports
 *
 * Windows parity: a machine without an AMD GPU has no AMD driver and so no amfrt64.dll, and Adobe then
 * never offers AMD hardware encoding. DllMain refuses to load without an AMD GPU (unless
 * NEUTRON_AMF_ANY_GPU=1, for development); AMFInit then refuses when vkenc finds no encoder to use.
 */

static BOOL env_any_gpu(void)
{
    char value[8];
    DWORD len = GetEnvironmentVariableA("NEUTRON_AMF_ANY_GPU", value, sizeof(value));

    return len == 1 && value[0] == '1';
}

/* Looks for a DRM card whose PCI vendor is AMD. *seen counts the cards whose vendor could be read. */
static BOOL find_amd_gpu(unsigned int *seen)
{
    struct dirent *de;
    BOOL found = FALSE;
    DIR *dir;

    *seen = 0;
    if (!(dir = opendir("/sys/class/drm"))) return FALSE;
    while ((de = readdir(dir)))
    {
        char path[64 + sizeof(de->d_name)];
        unsigned int vendor;
        FILE *f;

        /* card0, card1, ...; not the connectors (card0-DP-1) or render nodes */
        if (strncmp(de->d_name, "card", 4) || !de->d_name[4]
                || strspn(de->d_name + 4, "0123456789") != strlen(de->d_name + 4))
            continue;
        snprintf(path, sizeof(path), "/sys/class/drm/%s/device/vendor", de->d_name);
        if (!(f = fopen(path, "r"))) continue;
        if (fscanf(f, "%x", &vendor) == 1)
        {
            (*seen)++;
            TRACE("%s: PCI vendor %#06x\n", de->d_name, vendor);
            if (vendor == 0x1002) found = TRUE;
        }
        fclose(f);
    }
    closedir(dir);
    return found;
}

/* -------------------------------------------------------------------------------------------------
 * Status file for `neutron doctor`
 *
 * After the caps query (once per process, from AMFInit), $WINEPREFIX/neutron-amf.json says whether
 * hardware encoding works here and, if not, why:
 *   {"format":1,"time":"2026-10-16T14:03:22Z","gpu":{"vendor":"0x1002","device":"0x73df","name":"..."},
 *    "codecs":{"h264":{"available":true,"backend":"vulkan","max_width":4096,"max_height":2304,
 *    "tenbit":false,"reason":""},"hevc":{...}}}
 * Only a loaded amfrt64 writes it (DllMain found an AMD GPU, or NEUTRON_AMF_ANY_GPU=1). WINEPREFIX comes
 * from the Unix environment; no file when it is unset or the prefix is not writable. Written to a
 * temporary file and renamed, so a reader never sees half a file.
 */

static void json_string(FILE *f, const char *s)
{
    fputc('"', f);
    for (; *s; s++)
    {
        unsigned char c = *s;

        if (c == '"' || c == '\\') fprintf(f, "\\%c", c);
        else if (c < 0x20) fprintf(f, "\\u%04x", c);
        else if (c >= 0x7f) fputc('?', f);    /* GPU names and reasons are ASCII; keep the JSON valid */
        else fputc(c, f);
    }
    fputc('"', f);
}

static void write_status_file(void)
{
    const char *prefix = getenv("WINEPREFIX");
    const vkenc_caps *gpu = NULL;
    char path[4096], temp[4096], stamp[32] = "";
    struct tm tm;
    time_t now;
    unsigned int i;
    FILE *f;
    int ok;

    if (!prefix || !*prefix) return;
    if (snprintf(path, sizeof(path), "%s/neutron-amf.json", prefix) >= (int)sizeof(path)) return;
    if (snprintf(temp, sizeof(temp), "%s/.neutron-amf.json.%lu.tmp", prefix,
                 (unsigned long)GetCurrentProcessId()) >= (int)sizeof(temp))
        return;

    /* The GPU: the one that encodes, else whichever a backend found. */
    for (i = 0; i < ARRAY_SIZE(codec_descs) && !gpu; i++)
        if (codec_caps[i].available) gpu = &codec_caps[i];
    for (i = 0; i < ARRAY_SIZE(codec_descs) && !gpu; i++)
        if (codec_caps[i].vendor_id) gpu = &codec_caps[i];

    now = time(NULL);
    if (gmtime_r(&now, &tm)) strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%SZ", &tm);

    if (!(f = fopen(temp, "w")))
    {
        TRACE("cannot write %s\n", debugstr_a(temp));
        return;
    }
    fprintf(f, "{\"format\":1,\"time\":");
    json_string(f, stamp);
    fprintf(f, ",\"gpu\":{\"vendor\":\"0x%04x\",\"device\":\"0x%04x\",\"name\":", gpu ? gpu->vendor_id : 0,
            gpu ? gpu->device_id : 0);
    json_string(f, gpu ? gpu->device_name : "");
    fprintf(f, "},\"codecs\":{");
    for (i = 0; i < ARRAY_SIZE(codec_descs); i++)
    {
        const vkenc_caps *caps = &codec_caps[i];

        fprintf(f, "%s\"%s\":{\"available\":%s,\"backend\":", i ? "," : "",
                codec_descs[i]->codec == VKENC_CODEC_HEVC ? "hevc" : "h264", caps->available ? "true" : "false");
        json_string(f, caps->available ? vkenc_backend_name(codec_descs[i]->codec) : "none");
        fprintf(f, ",\"max_width\":%u,\"max_height\":%u,\"tenbit\":%s,\"reason\":", caps->max_width,
                caps->max_height, caps->available && caps->supports_10bit ? "true" : "false");
        json_string(f, caps->available ? "" : caps->reason);
        fputc('}', f);
    }
    fprintf(f, "}}\n");
    ok = !ferror(f);
    if (fclose(f)) ok = 0;
    if (!ok || rename(temp, path))
    {
        TRACE("cannot write %s\n", debugstr_a(path));
        remove(temp);
        return;
    }
    TRACE("wrote %s\n", debugstr_a(path));
}

static BOOL CALLBACK query_codec_caps(INIT_ONCE *once, void *param, void **context)
{
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(codec_descs); i++)
    {
        vkenc_caps *caps = &codec_caps[i];
        int r;

        memset(caps, 0, sizeof(*caps));
        if ((r = vkenc_query_caps(codec_descs[i]->codec, caps)) < 0)
        {
            caps->available = 0;
            if (!caps->reason[0]) snprintf(caps->reason, sizeof(caps->reason), "vkenc_query_caps failed (%d)", r);
        }
        caps->device_name[sizeof(caps->device_name) - 1] = 0;
        caps->reason[sizeof(caps->reason) - 1] = 0;
        if (caps->available)
            TRACE("%s: %s [%04x:%04x], %ux%u to %ux%u, max %llu bps, 10-bit %d, CBR %d, VBR %d\n",
                  codec_descs[i]->name, caps->device_name, caps->vendor_id, caps->device_id, caps->min_width,
                  caps->min_height, caps->max_width, caps->max_height, (unsigned long long)caps->max_bitrate,
                  caps->supports_10bit, caps->supports_cbr, caps->supports_vbr);
        else
            TRACE("%s: unavailable: %s\n", codec_descs[i]->name, caps->reason);
    }
    write_status_file();
    return TRUE;
}

AMF_RESULT CDECL AMFInit(UINT64 version, void **factory)
{
    unsigned int i, available = 0;

    TRACE("version %u.%u.%u.%u, factory %p\n", AMF_VERSION_MAJOR(version), AMF_VERSION_MINOR(version),
          AMF_VERSION_RELEASE(version), AMF_VERSION_BUILD(version), factory);
    if (!factory) return AMF_INVALID_POINTER;
    *factory = NULL;
    if (AMF_VERSION_MAJOR(version) != 1)
    {
        ERR("AMF %u.%u is not supported (this runtime is 1.5)\n", AMF_VERSION_MAJOR(version),
            AMF_VERSION_MINOR(version));
        return AMF_NOT_SUPPORTED;
    }
    if (version > AMF_RUNTIME_VERSION)
        WARN("the application wants AMF %u.%u.%u, newer than this runtime's 1.5.2\n", AMF_VERSION_MAJOR(version),
             AMF_VERSION_MINOR(version), AMF_VERSION_RELEASE(version));

    InitOnceExecuteOnce(&codec_caps_once, query_codec_caps, NULL, NULL);
    for (i = 0; i < ARRAY_SIZE(codec_descs); i++)
        if (codec_caps[i].available) available++;
    if (!available)
    {
        ERR("no usable hardware encoder, so no AMF: H.264: %s; HEVC: %s\n", codec_caps[0].reason,
            codec_caps[1].reason);
        return AMF_NO_DEVICE;
    }
    *factory = &amf_factory;
    return AMF_OK;
}

AMF_RESULT CDECL AMFQueryVersion(UINT64 *version)
{
    TRACE("version %p\n", version);
    if (!version) return AMF_INVALID_POINTER;
    *version = AMF_RUNTIME_VERSION;
    return AMF_OK;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void *reserved)
{
    unsigned int seen;

    TRACE("(%p, %u, %p)\n", instance, (unsigned int)reason, reserved);

    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(instance);
    if (find_amd_gpu(&seen)) return TRUE;
    if (env_any_gpu())
    {
        WARN("no AMD GPU; loading anyway because NEUTRON_AMF_ANY_GPU=1\n");
        return TRUE;
    }
    /* What Windows does without an AMD driver: there is no amfrt64.dll to load. */
    if (seen) WARN("no AMD GPU among %u DRM devices; not loading (NEUTRON_AMF_ANY_GPU=1 overrides)\n", seen);
    else ERR("cannot read any GPU vendor from /sys/class/drm; not loading (NEUTRON_AMF_ANY_GPU=1 overrides)\n");
    return FALSE;
}
