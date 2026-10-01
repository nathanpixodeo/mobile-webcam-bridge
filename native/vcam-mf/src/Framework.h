// Common includes for vcam-mf.dll. Included first by every translation unit of this project.
#pragma once

#include <windows.h>
#include <unknwn.h>

// Turns the DEFINE_GUID declarations of the headers below into selectany definitions, so the MF
// and KS GUIDs we reference never depend on which import library happens to export them.
#include <initguid.h>

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mfvirtualcamera.h>

#include <ks.h>
#include <ksproxy.h>
#include <ksmedia.h>

#include <wrl/implements.h>

#include <wil/com.h>
#include <wil/resource.h>
#include <wil/result_macros.h>
