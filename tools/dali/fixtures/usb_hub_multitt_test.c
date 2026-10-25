#include "UsbBus.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static USB_DEVICE device;
static USB_DEVICE_DESC device_desc;
static USB_INTERFACE hub_if;
static USB_INTERFACE_DESC interface_desc;
static USB_INTERFACE_SETTING settings[2];
static USB_ENDPOINT_DESC endpoints[2];
static USB_ENDPOINT_DESC *endpoint_lists[2][1];
static UINTN control_calls;
static EFI_STATUS control_status;
static UINT32 control_usb_status;
static BOOLEAN update_interface_state;
static EFI_USB_DEVICE_REQUEST last_request;
static UINT32 last_timeout;

static EFI_STATUS EFIAPI mock_control_transfer(
  IN EFI_USB_IO_PROTOCOL     *This,
  IN EFI_USB_DEVICE_REQUEST  *Request,
  IN EFI_USB_DATA_DIRECTION  Direction,
  IN UINT32                  Timeout,
  IN OUT VOID                *Data,
  IN UINTN                   DataLength,
  OUT UINT32                 *UsbStatus
  )
{
  assert(This == &hub_if.UsbIo);
  assert(Request != NULL && UsbStatus != NULL);
  assert(Direction == EfiUsbNoData && Data == NULL && DataLength == 0);
  control_calls++;
  last_request = *Request;
  last_timeout = Timeout;
  *UsbStatus = control_usb_status;
  if (EFI_ERROR(control_status) || (control_usb_status != EFI_USB_NOERROR)) {
    return control_status;
  }

  if (update_interface_state) {
    interface_desc.ActiveIndex = 1;
    hub_if.IfSetting = &settings[1];
  }
  return EFI_SUCCESS;
}

static void setup_case(UINT8 Speed, UINT8 DeviceProtocol, BOOLEAN HaveAlt1,
                       UINT8 Alt1Protocol, BOOLEAN Alt1InterruptIn)
{
  memset(&device, 0, sizeof device);
  memset(&device_desc, 0, sizeof device_desc);
  memset(&hub_if, 0, sizeof hub_if);
  memset(&interface_desc, 0, sizeof interface_desc);
  memset(settings, 0, sizeof settings);
  memset(endpoints, 0, sizeof endpoints);
  memset(endpoint_lists, 0, sizeof endpoint_lists);
  control_calls = 0;
  control_status = EFI_SUCCESS;
  control_usb_status = EFI_USB_NOERROR;
  update_interface_state = TRUE;
  memset(&last_request, 0, sizeof last_request);
  last_timeout = 0;

  device.Speed = Speed;
  device.DevDesc = &device_desc;
  device_desc.Desc.DeviceProtocol = DeviceProtocol;
  hub_if.Device = &device;
  hub_if.IfDesc = &interface_desc;
  hub_if.IfSetting = &settings[0];
  hub_if.UsbIo.UsbControlTransfer = mock_control_transfer;

  interface_desc.NumOfSetting = HaveAlt1 ? 2 : 1;
  interface_desc.ActiveIndex = 0;
  interface_desc.Settings[0] = &settings[0];
  settings[0].Desc.InterfaceNumber = 3;
  settings[0].Desc.AlternateSetting = 0;
  settings[0].Desc.InterfaceClass = USB_HUB_CLASS_CODE;
  settings[0].Desc.InterfaceSubClass = USB_HUB_SUBCLASS_CODE;
  settings[0].Desc.InterfaceProtocol = 1;
  settings[0].Desc.NumEndpoints = 1;
  settings[0].Endpoints = endpoint_lists[0];
  endpoints[0].Desc.EndpointAddress = USB_ENDPOINT_DIR_IN | 1;
  endpoints[0].Desc.Attributes = USB_ENDPOINT_INTERRUPT;
  endpoint_lists[0][0] = &endpoints[0];

  if (HaveAlt1) {
    interface_desc.Settings[1] = &settings[1];
    settings[1].Desc.InterfaceNumber = 3;
    settings[1].Desc.AlternateSetting = 1;
    settings[1].Desc.InterfaceClass = USB_HUB_CLASS_CODE;
    settings[1].Desc.InterfaceSubClass = USB_HUB_SUBCLASS_CODE;
    settings[1].Desc.InterfaceProtocol = Alt1Protocol;
    settings[1].Desc.NumEndpoints = 1;
    settings[1].Endpoints = endpoint_lists[1];
    endpoints[1].Desc.EndpointAddress = Alt1InterruptIn ? (USB_ENDPOINT_DIR_IN | 1) : 1;
    endpoints[1].Desc.Attributes = USB_ENDPOINT_INTERRUPT;
    endpoint_lists[1][0] = &endpoints[1];
  }
}

static void test_single_tt_and_non_high_speed(void)
{
  setup_case(EFI_USB_SPEED_HIGH, 1, TRUE, 2, TRUE);
  assert(UsbHubSelectMultiTtSetting(&hub_if) == EFI_SUCCESS);
  assert(control_calls == 0 && hub_if.IfSetting == &settings[0]);

  setup_case(EFI_USB_SPEED_FULL, 2, TRUE, 2, TRUE);
  assert(UsbHubSelectMultiTtSetting(&hub_if) == EFI_SUCCESS);
  assert(control_calls == 0 && hub_if.IfSetting == &settings[0]);
}

static void test_multi_tt_selection(void)
{
  setup_case(EFI_USB_SPEED_HIGH, 2, TRUE, 2, TRUE);
  assert(UsbHubSelectMultiTtSetting(&hub_if) == EFI_SUCCESS);
  assert(control_calls == 1);
  assert(last_request.RequestType == USB_REQUEST_TYPE(EfiUsbNoData, USB_REQ_TYPE_STANDARD, USB_TARGET_INTERFACE));
  assert(last_request.Request == USB_REQ_SET_INTERFACE && last_request.Value == 1 && last_request.Index == 3);
  assert(last_request.Length == 0 && last_timeout == USB_GENERAL_DEVICE_REQUEST_TIMEOUT);
  assert(interface_desc.ActiveIndex == 1 && hub_if.IfSetting == &settings[1]);

  setup_case(EFI_USB_SPEED_HIGH, 2, TRUE, 2, TRUE);
  interface_desc.ActiveIndex = 1;
  hub_if.IfSetting = &settings[1];
  assert(UsbHubSelectMultiTtSetting(&hub_if) == EFI_SUCCESS);
  assert(control_calls == 0 && hub_if.IfSetting == &settings[1]);
}

static void test_invalid_alternate_stays_single_tt(void)
{
  setup_case(EFI_USB_SPEED_HIGH, 2, FALSE, 0, FALSE);
  assert(UsbHubSelectMultiTtSetting(&hub_if) == EFI_SUCCESS && control_calls == 0);
  assert(hub_if.IfSetting == &settings[0]);

  setup_case(EFI_USB_SPEED_HIGH, 2, TRUE, 1, TRUE);
  assert(UsbHubSelectMultiTtSetting(&hub_if) == EFI_SUCCESS && control_calls == 0);
  assert(hub_if.IfSetting == &settings[0]);

  setup_case(EFI_USB_SPEED_HIGH, 2, TRUE, 2, FALSE);
  assert(UsbHubSelectMultiTtSetting(&hub_if) == EFI_SUCCESS && control_calls == 0);
  assert(hub_if.IfSetting == &settings[0]);
}

static void test_failed_switch_is_not_published(void)
{
  setup_case(EFI_USB_SPEED_HIGH, 2, TRUE, 2, TRUE);
  control_status = EFI_DEVICE_ERROR;
  update_interface_state = FALSE;
  assert(UsbHubSelectMultiTtSetting(&hub_if) == EFI_DEVICE_ERROR);
  assert(control_calls == 1 && interface_desc.ActiveIndex == 0 && hub_if.IfSetting == &settings[0]);

  setup_case(EFI_USB_SPEED_HIGH, 2, TRUE, 2, TRUE);
  control_usb_status = EFI_USB_ERR_STALL;
  update_interface_state = FALSE;
  assert(UsbHubSelectMultiTtSetting(&hub_if) == EFI_DEVICE_ERROR);
  assert(control_calls == 1 && interface_desc.ActiveIndex == 0 && hub_if.IfSetting == &settings[0]);

  setup_case(EFI_USB_SPEED_HIGH, 2, TRUE, 2, TRUE);
  update_interface_state = FALSE;
  assert(UsbHubSelectMultiTtSetting(&hub_if) == EFI_DEVICE_ERROR);
  assert(control_calls == 1 && interface_desc.ActiveIndex == 0 && hub_if.IfSetting == &settings[0]);
}

int main(void)
{
  test_single_tt_and_non_high_speed();
  test_multi_tt_selection();
  test_invalid_alternate_stays_single_tt();
  test_failed_switch_is_not_published();
  puts("PASS: actual Mu USB hub multi-TT selector, standard SET_INTERFACE request, single-TT fallback and failure retention; control-transfer model only");
  return 0;
}
