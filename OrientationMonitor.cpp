#include <windows.h>
#include <initguid.h>
#include <winioctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <cfgmgr32.h>
#include <roapi.h>
#include <hstring.h>
#include <wrl/implements.h>
#include <wrl/client.h>
#include <wrl/wrappers/corewrappers.h>
#include <windows.devices.sensors.h>
#include <windows.foundation.h>

using namespace Microsoft::WRL;
using namespace Microsoft::WRL::Wrappers;
using namespace ABI::Windows::Foundation;
using namespace ABI::Windows::Devices::Sensors;

DEFINE_GUID(GUID_DEVINTERFACE_ORIENTATION_MONITOR_SERVICE,
    0x422bc88e, 0x1437, 0x44dd, 0x98, 0xa3, 0xb2, 0x48, 0x78, 0x14, 0x47, 0xb6);

#define ORIENTATION_MONITOR_SERVICE CTL_CODE(0x1212, 0x122, METHOD_BUFFERED, FILE_ANY_ACCESS)

typedef enum {
    ORIENTATION_PORTRAIT = 0,
    ORIENTATION_PORTRAIT_REVERSED,
    ORIENTATION_LANDSCAPE_270,
    ORIENTATION_LANDSCAPE_90,
} ORIENTATION_STATE;

typedef struct {
    ORIENTATION_STATE Orientation_state;
} OrientationState;

static SERVICE_STATUS_HANDLE g_ServiceStatusHandle = NULL;
static SERVICE_STATUS g_ServiceStatus = {};
static HANDLE g_StopEvent = NULL;
static HANDLE g_SensorThread = NULL;

static BOOL SendOrientation(ORIENTATION_STATE state)
{
    DWORD interfaceListSize = 0;
    CONFIGRET cr;

    cr = CM_Get_Device_Interface_List_SizeW(
        &interfaceListSize,
        (LPGUID)&GUID_DEVINTERFACE_ORIENTATION_MONITOR_SERVICE,
        NULL,
        CM_GET_DEVICE_INTERFACE_LIST_PRESENT);

    if (cr != CR_SUCCESS || interfaceListSize == 0)
        return FALSE;

    PWSTR interfaceList = (PWSTR)HeapAlloc(GetProcessHeap(), 0, interfaceListSize * sizeof(WCHAR));
    if (!interfaceList)
        return FALSE;

    cr = CM_Get_Device_Interface_ListW(
        (LPGUID)&GUID_DEVINTERFACE_ORIENTATION_MONITOR_SERVICE,
        NULL,
        interfaceList,
        interfaceListSize,
        CM_GET_DEVICE_INTERFACE_LIST_PRESENT);

    if (cr != CR_SUCCESS || interfaceList[0] == L'\0')
    {
        HeapFree(GetProcessHeap(), 0, interfaceList);
        return FALSE;
    }

    HANDLE hDevice = CreateFileW(
        interfaceList,
        GENERIC_READ | GENERIC_WRITE,
        0, NULL, OPEN_EXISTING, 0, NULL);

    HeapFree(GetProcessHeap(), 0, interfaceList);

    if (hDevice == INVALID_HANDLE_VALUE)
        return FALSE;

    OrientationState input;
    input.Orientation_state = state;

    DWORD bytesReturned = 0;
    BOOL result = DeviceIoControl(
        hDevice,
        ORIENTATION_MONITOR_SERVICE,
        &input,
        sizeof(input),
        NULL, 0,
        &bytesReturned,
        NULL);

    CloseHandle(hDevice);
    return result;
}

static ORIENTATION_STATE MapOrientation(SimpleOrientation value)
{
    switch (value)
    {
    case SimpleOrientation_NotRotated:                        return ORIENTATION_PORTRAIT;
    case SimpleOrientation_Rotated180DegreesCounterclockwise: return ORIENTATION_PORTRAIT_REVERSED;
    case SimpleOrientation_Rotated270DegreesCounterclockwise: return ORIENTATION_LANDSCAPE_270;
    case SimpleOrientation_Rotated90DegreesCounterclockwise:  return ORIENTATION_LANDSCAPE_90;
    default:                                                  return ORIENTATION_PORTRAIT;
    }
}

class OrientationHandler WrlFinal :
    public RuntimeClass<RuntimeClassFlags<WinRtClassicComMix>,
        ITypedEventHandler<SimpleOrientationSensor*, SimpleOrientationSensorOrientationChangedEventArgs*>>
{
public:
    HRESULT STDMETHODCALLTYPE Invoke(
        ISimpleOrientationSensor* sensor,
        ISimpleOrientationSensorOrientationChangedEventArgs* e)
    {
        UNREFERENCED_PARAMETER(sensor);
        SimpleOrientation orientation = SimpleOrientation_NotRotated;
        HRESULT hr = e->get_Orientation(&orientation);
        if (SUCCEEDED(hr))
            SendOrientation(MapOrientation(orientation));
        return S_OK;
    }
};

static DWORD WINAPI SensorMonitorThread(LPVOID param)
{
    (void)param;
    HRESULT hr;
    ComPtr<ISimpleOrientationSensorStatics> sensorStatics;
    ComPtr<ISimpleOrientationSensor> sensor;
    EventRegistrationToken token = {};

    hr = RoInitialize(RO_INIT_MULTITHREADED);
    if (FAILED(hr))
        return 1;

    hr = GetActivationFactory(
        HStringReference(RuntimeClass_Windows_Devices_Sensors_SimpleOrientationSensor).Get(),
        &sensorStatics);
    if (FAILED(hr) || !sensorStatics)
    {
        RoUninitialize();
        return 1;
    }

    while (true)
    {
        hr = sensorStatics->GetDefault(&sensor);
        if (SUCCEEDED(hr) && sensor)
            break;

        if (WaitForSingleObject(g_StopEvent, 500) == WAIT_OBJECT_0)
        {
            RoUninitialize();
            return 0;
        }
    }

    ComPtr<OrientationHandler> handler;
    hr = MakeAndInitialize<OrientationHandler>(&handler);
    if (FAILED(hr))
    {
        RoUninitialize();
        return 1;
    }

    sensor->add_OrientationChanged(handler.Get(), &token);
    WaitForSingleObject(g_StopEvent, INFINITE);
    sensor->remove_OrientationChanged(token);
    RoUninitialize();
    return 0;
}

static DWORD WINAPI ServiceCtrlHandlerEx(DWORD dwControl, DWORD dwEventType, LPVOID lpEventData, LPVOID lpContext)
{
    (void)dwEventType; (void)lpEventData; (void)lpContext;
    switch (dwControl)
    {
    case SERVICE_CONTROL_STOP:
        g_ServiceStatus.dwCurrentState = SERVICE_STOP_PENDING;
        g_ServiceStatus.dwWaitHint = 5000;
        SetServiceStatus(g_ServiceStatusHandle, &g_ServiceStatus);
        SetEvent(g_StopEvent);
        return NO_ERROR;
    default:
        return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

static void WINAPI ServiceMain(DWORD argc, LPSTR *argv)
{
    (void)argc; (void)argv;

    g_StopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!g_StopEvent)
        return;

    g_ServiceStatusHandle = RegisterServiceCtrlHandlerExW(
        L"OrientationMonitor", ServiceCtrlHandlerEx, NULL);
    if (!g_ServiceStatusHandle)
    {
        CloseHandle(g_StopEvent);
        g_StopEvent = NULL;
        return;
    }

    g_ServiceStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_ServiceStatus.dwCurrentState = SERVICE_RUNNING;
    g_ServiceStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP;
    SetServiceStatus(g_ServiceStatusHandle, &g_ServiceStatus);

    g_SensorThread = CreateThread(NULL, 0, SensorMonitorThread, NULL, 0, NULL);

    HANDLE handles[2] = { g_StopEvent, g_SensorThread };
    WaitForMultipleObjects(2, handles, FALSE, INFINITE);

    if (g_SensorThread)
    {
        WaitForSingleObject(g_SensorThread, 5000);
        if (WaitForSingleObject(g_SensorThread, 0) == WAIT_TIMEOUT)
            TerminateThread(g_SensorThread, 0);
        CloseHandle(g_SensorThread);
        g_SensorThread = NULL;
    }

    g_ServiceStatus.dwCurrentState = SERVICE_STOPPED;
    SetServiceStatus(g_ServiceStatusHandle, &g_ServiceStatus);

    CloseHandle(g_StopEvent);
    g_StopEvent = NULL;
}

int main(int argc, char *argv[])
{
    if (argc > 1 && strcmp(argv[1], "test") == 0)
    {
        printf("OrientationMonitor: test\n");
        printf("test PORTRAIT\n");
        SendOrientation(ORIENTATION_PORTRAIT);
        Sleep(1000);
        printf("test LANDSCAPE_90\n");
        SendOrientation(ORIENTATION_LANDSCAPE_90);
        Sleep(1000);
        printf("test PORTRAIT_REVERSED\n");
        SendOrientation(ORIENTATION_PORTRAIT_REVERSED);
        Sleep(1000);
        printf("test LANDSCAPE_270\n");
        SendOrientation(ORIENTATION_LANDSCAPE_270);
        printf("end of test\n");
        return 0;
    }

    SERVICE_TABLE_ENTRYA table[] = {
        { (LPSTR)"OrientationMonitor", (LPSERVICE_MAIN_FUNCTIONA)ServiceMain },
        { NULL, NULL }
    };

    if (!StartServiceCtrlDispatcherA(table))
        return 1;

    return 0;
}
