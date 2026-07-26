/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2021, Ha Thach (tinyusb.org)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 */

#include "bsp/board_api.h"
#include "tusb.h"

//--------------------------------------------------------------------+
// MACRO TYPEDEF CONSTANT ENUM DECLARATION
//--------------------------------------------------------------------+

// If your host terminal support ansi escape code such as TeraTerm
// it can be use to simulate mouse cursor movement within terminal
#define USE_ANSI_ESCAPE   0

#define MAX_REPORT  4

static uint8_t const keycode2ascii[128][2] =  { HID_KEYCODE_TO_ASCII };

// TinyUSB numbers HID instances per device. Keep a fixed-size table keyed by
// both device address and interface instance; CFG_TUH_HID is the maximum
// number of simultaneously mounted HID interfaces.
struct hid_info_t
{
  uint8_t dev_addr;
  uint8_t instance;
  uint8_t report_count;
  tuh_hid_report_info_t report_info[MAX_REPORT];
  hid_keyboard_report_t prev_keyboard_report;
};

static hid_info_t hid_info[CFG_TUH_HID] = {};

static hid_info_t* find_hid_info(uint8_t dev_addr, uint8_t instance)
{
  for (hid_info_t& info : hid_info)
  {
    if (info.dev_addr == dev_addr && info.instance == instance)
      return &info;
  }

  return nullptr;
}

static hid_info_t* allocate_hid_info(uint8_t dev_addr, uint8_t instance)
{
  if (hid_info_t* info = find_hid_info(dev_addr, instance))
    return info;

  for (hid_info_t& info : hid_info)
  {
    if (info.dev_addr == 0)
    {
      info = {};
      info.dev_addr = dev_addr;
      info.instance = instance;
      return &info;
    }
  }

  return nullptr;
}

struct input_bits_t {
  bool a: true;
  bool b: true;
  bool select: true;
  bool start: true;
  bool right: true;
  bool left: true;
  bool up: true;
  bool down: true;
};
///extern input_bits_t keyboard_bits;
extern input_bits_t gamepad1_bits;

void process_kbd_report(
  hid_keyboard_report_t const *report,
  hid_keyboard_report_t const *prev_report
);

static void process_mouse_report(hid_mouse_report_t const * report);
static void process_generic_report(uint8_t dev_addr, uint8_t instance, uint8_t const* report, uint16_t len);

//--------------------------------------------------------------------+
// TinyUSB Callbacks
//--------------------------------------------------------------------+

// Invoked when device with hid interface is mounted
// Report descriptor is also available for use. tuh_hid_parse_report_descriptor()
// can be used to parse common/simple enough descriptor.
// Note: if report descriptor length > CFG_TUH_ENUMERATION_BUFSIZE, it will be skipped
// therefore report_desc = NULL, desc_len = 0
void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance, uint8_t const* desc_report, uint16_t desc_len)
{
  hid_info_t* info = allocate_hid_info(dev_addr, instance);
  if (!info)
    return;

  uint8_t const itf_protocol = tuh_hid_interface_protocol(dev_addr, instance);

  // Boot-protocol keyboards and mice have a fixed report format. Generic HID
  // interfaces must be described here so process_generic_report() can match
  // their report ID and usage.
  info->report_count = 0;
  if (itf_protocol == HID_ITF_PROTOCOL_NONE && desc_report && desc_len)
  {
    info->report_count = tuh_hid_parse_report_descriptor(
        info->report_info, MAX_REPORT, desc_report, desc_len);
  }

  // Arm the first interrupt-IN transfer. Further reports are re-armed in
  // tuh_hid_report_received_cb().
  tuh_hid_receive_report(dev_addr, instance);
}

// Invoked when device with hid interface is un-mounted
void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance)
{
  if (hid_info_t* info = find_hid_info(dev_addr, instance))
    *info = {};
}

// Invoked when received report from device via interrupt endpoint
void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t instance, uint8_t const* report, uint16_t len)
{
  hid_info_t* info = find_hid_info(dev_addr, instance);
  if (!info || !report)
    return;

  uint8_t const itf_protocol = tuh_hid_interface_protocol(dev_addr, instance);

  switch (itf_protocol)
  {
    case HID_ITF_PROTOCOL_KEYBOARD:
      if (len >= sizeof(hid_keyboard_report_t))
      {
        TU_LOG2("HID receive boot keyboard report\r\n");
        auto const* keyboard_report =
            reinterpret_cast<hid_keyboard_report_t const*>(report);
        process_kbd_report(keyboard_report, &info->prev_keyboard_report);
        info->prev_keyboard_report = *keyboard_report;
      }
    break;

    case HID_ITF_PROTOCOL_MOUSE:
      if (len >= sizeof(hid_mouse_report_t))
      {
        TU_LOG2("HID receive boot mouse report\r\n");
        process_mouse_report(reinterpret_cast<hid_mouse_report_t const*>(report));
      }
    break;

    default:
      // Generic report requires matching ReportID and contents with previous parsed report info
      process_generic_report(dev_addr, instance, report, len);
    break;
  }

  // continue to request to receive report
  tuh_hid_receive_report(dev_addr, instance);
}

//--------------------------------------------------------------------+
// Keyboard
//--------------------------------------------------------------------+

// look up new key in previous keys
static inline bool find_key_in_report(hid_keyboard_report_t const *report, uint8_t keycode)
{
  for(uint8_t i=0; i<6; i++)
  {
    if (report->keycode[i] == keycode)  return true;
  }

  return false;
}

//--------------------------------------------------------------------+
// Mouse
//--------------------------------------------------------------------+

static void process_mouse_report(hid_mouse_report_t const * report)
{
  (void)report;
}

//--------------------------------------------------------------------+
// Generic Report
//--------------------------------------------------------------------+
static void process_generic_report(uint8_t dev_addr, uint8_t instance, uint8_t const* report, uint16_t len)
{
  hid_info_t* info = find_hid_info(dev_addr, instance);
  if (!info || !report || len == 0)
    return;

  uint8_t const rpt_count = info->report_count;
  tuh_hid_report_info_t* rpt_info_arr = info->report_info;
  tuh_hid_report_info_t* rpt_info = NULL;

  if ( rpt_count == 1 && rpt_info_arr[0].report_id == 0)
  {
    // Simple report without report ID as 1st byte
    rpt_info = &rpt_info_arr[0];
  }else
  {
    // Composite report, 1st byte is report ID, data starts from 2nd byte
    uint8_t const rpt_id = report[0];

    // Find report id in the array
    for(uint8_t i=0; i<rpt_count; i++)
    {
      if (rpt_id == rpt_info_arr[i].report_id )
      {
        rpt_info = &rpt_info_arr[i];
        break;
      }
    }

    report++;
    len--;
  }
  if (!rpt_info)
    return;

  // For complete list of Usage Page & Usage checkout src/class/hid/hid.h. For examples:
  // - Keyboard                     : Desktop, Keyboard
  // - Mouse                        : Desktop, Mouse
  // - Gamepad                      : Desktop, Gamepad
  // - Consumer Control (Media Key) : Consumer, Consumer Control
  // - System Control (Power key)   : Desktop, System Control
  // - Generic (vendor)             : 0xFFxx, xx
  if ( rpt_info->usage_page == HID_USAGE_PAGE_DESKTOP )
  {
    switch (rpt_info->usage)
    {
      case HID_USAGE_DESKTOP_KEYBOARD:
        if (len >= sizeof(hid_keyboard_report_t))
        {
          TU_LOG1("HID receive keyboard report\r\n");
          // Assume keyboard follows the boot report layout.
          auto const* keyboard_report =
              reinterpret_cast<hid_keyboard_report_t const*>(report);
          process_kbd_report(keyboard_report, &info->prev_keyboard_report);
          info->prev_keyboard_report = *keyboard_report;
        }
      break;

      case HID_USAGE_DESKTOP_MOUSE:
        if (len >= sizeof(hid_mouse_report_t))
        {
          TU_LOG1("HID receive mouse report\r\n");
          // Assume mouse follows the boot report layout.
          process_mouse_report(reinterpret_cast<hid_mouse_report_t const*>(report));
        }
      break;

      default: break;
    }
  }
}
