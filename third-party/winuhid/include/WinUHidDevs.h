#pragma once

#ifdef __cplusplus
#define WINUHID_EXTERN_C extern "C"
#else
#define WINUHID_EXTERN_C
#endif

#if defined(WINUHID_STATIC)
#define WINUHID_API WINUHID_EXTERN_C
#elif defined(WINUHID_EXPORTS)
#define WINUHID_API WINUHID_EXTERN_C __declspec(dllexport)
#else
#define WINUHID_API WINUHID_EXTERN_C __declspec(dllimport)
#endif

typedef struct _WINUHID_PRESET_DEVICE_INFO {
	//
	// Optionally specifies hardware ID information. If unspecified, a preset value will be used.
	//
	// NOTE: Because product ID values are namespaced by the vendor ID, you must provide a vendor ID
	// if you provide a product ID. Likewise, you must specify a product ID (and vendor ID) if you
	// specify a version number.
	//
	USHORT VendorID;
	USHORT ProductID;
	USHORT VersionNumber;

	//
	// Optionally distinguishes the physical device collection
	//
	GUID ContainerId;

	//
	// Optionally distinguishes instances of the same device
	//
	PCWSTR InstanceID;

	//
	// Optionally specifies additional hardware IDs to provide for PnP enumeration
	//
	// Note: This value must be a REG_MULTI_SZ - that is, a set of null terminated strings terminated by a second consecutive null.
	//
	PCWSTR HardwareIDs;
} WINUHID_PRESET_DEVICE_INFO, *PWINUHID_PRESET_DEVICE_INFO;
typedef CONST WINUHID_PRESET_DEVICE_INFO* PCWINUHID_PRESET_DEVICE_INFO;