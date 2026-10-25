#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bluetoothapis.h>
#include <cstdio>
int main(){
  BLUETOOTH_FIND_RADIO_PARAMS rp{sizeof(rp)};
  HANDLE radio=nullptr;
  HBLUETOOTH_RADIO_FIND rf=BluetoothFindFirstRadio(&rp,&radio);
  int n=0;
  if(rf){ do{ ++n; }while(BluetoothFindNextRadio(rf,&radio)); BluetoothFindRadioClose(rf); }
  printf("radios=%d err=%lu\n", n, (unsigned long)GetLastError());
  if(radio){
    BLUETOOTH_RADIO_INFO ri{}; ri.dwSize=sizeof(ri);
    if(BluetoothGetRadioInfo(radio,&ri)==ERROR_SUCCESS) wprintf(L"radio name=%s\n", ri.szName);
    BLUETOOTH_DEVICE_SEARCH_PARAMS sp{};
    sp.dwSize=sizeof(sp); sp.fReturnAuthenticated=TRUE; sp.fReturnRemembered=TRUE;
    sp.fReturnConnected=TRUE; sp.fReturnUnknown=FALSE; sp.hRadio=radio; sp.cTimeoutMultiplier=20;
    BLUETOOTH_DEVICE_INFO di{}; di.dwSize=sizeof(di);
    HBLUETOOTH_DEVICE_FIND df=BluetoothFindFirstDevice(&sp,&di);
    if(df){ do{ wprintf(L"dev: %s connected=%d\n", di.szName, di.fConnected); di.dwSize=sizeof(di);}while(BluetoothFindNextDevice(df,&di)); BluetoothFindDeviceClose(df); }
    else printf("no devices err=%lu\n",(unsigned long)GetLastError());
    CloseHandle(radio);
  }
  return 0;
}
