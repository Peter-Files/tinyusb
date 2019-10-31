/* 
 * The MIT License (MIT)
 *
 * Copyright (c) 2019 Ha Thach (tinyusb.org)
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
 * This file is part of the TinyUSB stack.
 */

#ifndef DEBUG
#define DEBUG 1
#endif
#include "tusb_option.h"


// #if TUSB_OPT_DEVICE_ENABLED && (CFG_TUSB_MCU == OPT_MCU_FOMU_EPTRI)
#if 1

#include "device/dcd.h"
#include "dcd_fomu.h"
#include "csr.h"
#include "irq.h"
void fomu_error(uint32_t line);
void mputs(const char *str);
void mputln(const char *str);

static uint8_t last_address;

//--------------------------------------------------------------------+
// SIE Command
//--------------------------------------------------------------------+

#define EP_SIZE 64

uint16_t volatile rx_buffer_offset[16];
uint8_t volatile * rx_buffer[16];
uint16_t volatile rx_buffer_max[16];

volatile uint8_t tx_ep;
volatile bool tx_active;
volatile uint16_t tx_buffer_offset[16];
uint8_t volatile * tx_buffer[16];
volatile uint16_t tx_buffer_max[16];
volatile uint8_t reset_count;

#if DEBUG
__attribute__((used)) uint8_t volatile * last_tx_buffer;
__attribute__((used)) volatile uint8_t last_tx_ep;
uint8_t setup_packet_bfr[10];
#endif
//--------------------------------------------------------------------+
// PIPE HELPER
//--------------------------------------------------------------------+

static bool advance_tx_ep(void) {
  // Move on to the next transmit buffer in a round-robin manner
  uint8_t prev_tx_ep = tx_ep;
  for (tx_ep = (tx_ep + 1) & 0xf; tx_ep != prev_tx_ep; tx_ep = ((tx_ep + 1) & 0xf)) {
    if (tx_buffer[tx_ep])
      return true;
  }
  if (!tx_buffer[tx_ep])
    return false;
  return true;
}

static void tx_more_data(void) {
  // Send more data
  uint8_t added_bytes;
  for (added_bytes = 0; (added_bytes < EP_SIZE) && (tx_buffer_offset[tx_ep] < tx_buffer_max[tx_ep]); added_bytes++) {
    usb_in_data_write(tx_buffer[tx_ep][tx_buffer_offset[tx_ep]++]);
  }

  // Updating the epno queues the data
  usb_in_ctrl_write(tx_ep & 0xf);
}

static void process_tx(void) {
#if DEBUG
  // If the system isn't idle, then something is very wrong.
  uint8_t in_status = usb_in_status_read();
  if (!(in_status & (1 << CSR_USB_IN_STATUS_IDLE_OFFSET)))
    fomu_error(__LINE__);
#endif

  // If the buffer is now empty, search for the next buffer to fill.
  if (!tx_buffer[tx_ep]) {
    if (advance_tx_ep())
      tx_more_data();
    else
      tx_active = false;
    return;
  }

  if (tx_buffer_offset[tx_ep] >= tx_buffer_max[tx_ep]) {
#if DEBUG
    last_tx_buffer = tx_buffer[tx_ep];
    last_tx_ep = tx_ep;
#endif
    tx_buffer[tx_ep] = NULL;
    uint16_t xferred_bytes = tx_buffer_max[tx_ep];
    uint8_t xferred_ep = tx_ep;

    if (!advance_tx_ep())
      tx_active = false;
    if ((xferred_bytes == 13) && (tx_buffer_max[tx_ep] == 512))
      fomu_error(__LINE__);
    dcd_event_xfer_complete(0, tu_edpt_addr(xferred_ep, TUSB_DIR_IN), xferred_bytes, XFER_RESULT_SUCCESS, true);
    if (!tx_active)
      return;
  }

  tx_more_data();
  return;
}

static void process_rx(void) {
  uint8_t out_status = usb_out_status_read();
#if DEBUG
  // If the OUT handler is still waiting to send, don't do anything.
  if (!(out_status & (1 << CSR_USB_OUT_STATUS_HAVE_OFFSET)))
    fomu_error(__LINE__);
    // return;
#endif
  uint8_t rx_ep = (out_status >> CSR_USB_OUT_STATUS_EPNO_OFFSET) & 0xf;

  // If the destination buffer doesn't exist, don't drain the hardware
  // fifo.  Note that this can cause deadlocks if the host is waiting
  // on some other endpoint's data!
#if DEBUG
  if (rx_buffer[rx_ep] == NULL) {
    fomu_error(__LINE__);
    return;
  }
#endif

  // Drain the FIFO into the destination buffer
  uint32_t total_read = 0;
  uint32_t current_offset = rx_buffer_offset[rx_ep];
#if DEBUG
  if (current_offset > rx_buffer_max[rx_ep])
    fomu_error(__LINE__);
#endif
  while (usb_out_status_read() & (1 << CSR_USB_OUT_STATUS_HAVE_OFFSET)) {
    uint8_t c = usb_out_data_read();
    total_read++;
    if ((rx_buffer_offset[rx_ep] + current_offset) < rx_buffer_max[rx_ep])
      rx_buffer[rx_ep][current_offset++] = c;
  }
#if DEBUG
  if (total_read > 66)
    fomu_error(__LINE__);
  if (total_read < 2)
    fomu_error(__LINE__);
#endif

  // Strip off the CRC16
  rx_buffer_offset[rx_ep] += (total_read - 2);
  if (rx_buffer_offset[rx_ep] > rx_buffer_max[rx_ep])
    rx_buffer_offset[rx_ep] = rx_buffer_max[rx_ep];

  // If there's no more data, complete the transfer to tinyusb
  if (rx_buffer_max[rx_ep] == rx_buffer_offset[rx_ep]) {
#if DEBUG
    if (rx_buffer[rx_ep] == NULL)
      fomu_error(__LINE__);
#endif

    // Free up this buffer.
    rx_buffer[rx_ep] = NULL;
    uint16_t len = rx_buffer_offset[rx_ep];

#if DEBUG
    // Validate that all enabled endpoints have buffers,
    // and no disabled endpoints have buffers.
    uint16_t ep_en_mask = usb_out_enable_status_read();
    int i;
    for (i = 0; i < 16; i++) {
      if ((!!(ep_en_mask & (1 << i))) ^ (!!(rx_buffer[i]))) {
        // uint8_t new_status = usb_out_status_read();
        // // Another IRQ came in while we were processing, so ignore this endpoint.
        // if ((new_status & 0x20) && ((new_status & 0xf) == i))
        //   continue;
        fomu_error(__LINE__);
      }
    }
#endif
    dcd_event_xfer_complete(0, tu_edpt_addr(rx_ep, TUSB_DIR_OUT), len, XFER_RESULT_SUCCESS, true);
  }
  else {
    // If there's more data, re-enable data reception on this endpoint
    usb_out_ctrl_write((1 << CSR_USB_OUT_CTRL_ENABLE_OFFSET) | rx_ep);
  }

  // Now that the buffer is drained, clear the pending IRQ.
  usb_out_ev_pending_write(usb_out_ev_pending_read());
}

//--------------------------------------------------------------------+
// CONTROLLER API
//--------------------------------------------------------------------+

static void dcd_reset(void)
{
  reset_count++;
  usb_setup_ev_enable_write(0);
  usb_in_ev_enable_write(0);
  usb_out_ev_enable_write(0);

  if (last_address)
    asm("ebreak");
  usb_address_write(0);

  // Reset all three FIFO handlers
  usb_setup_ctrl_write(1 << CSR_USB_SETUP_CTRL_RESET_OFFSET);
  usb_in_ctrl_write(1 << CSR_USB_IN_CTRL_RESET_OFFSET);
  usb_out_ctrl_write(1 << CSR_USB_OUT_CTRL_RESET_OFFSET);

  memset((void *)rx_buffer, 0, sizeof(rx_buffer));
  memset((void *)rx_buffer_max, 0, sizeof(rx_buffer_max));
  memset((void *)rx_buffer_offset, 0, sizeof(rx_buffer_offset));

  memset((void *)tx_buffer, 0, sizeof(tx_buffer));
  memset((void *)tx_buffer_max, 0, sizeof(tx_buffer_max));
  memset((void *)tx_buffer_offset, 0, sizeof(tx_buffer_offset));
  tx_ep = 0;
  tx_active = false;

  // Enable all event handlers and clear their contents
  usb_setup_ev_pending_write(-1);
  usb_in_ev_pending_write(-1);
  usb_out_ev_pending_write(-1);
  usb_in_ev_enable_write(1);
  usb_out_ev_enable_write(1);
  usb_setup_ev_enable_write(3);

  dcd_event_bus_signal(0, DCD_EVENT_BUS_RESET, true);
}

// Initializes the USB peripheral for device mode and enables it.
void dcd_init(uint8_t rhport)
{
  (void) rhport;

  usb_pullup_out_write(0);

  // Enable all event handlers and clear their contents
  usb_setup_ev_pending_write(usb_setup_ev_pending_read());
  usb_in_ev_pending_write(usb_in_ev_pending_read());
  usb_out_ev_pending_write(usb_out_ev_pending_read());
  usb_in_ev_enable_write(1);
  usb_out_ev_enable_write(1);
  usb_setup_ev_enable_write(3);

  // Turn on the external pullup
  usb_pullup_out_write(1);
}

// Enables or disables the USB device interrupt(s). May be used to
// prevent concurrency issues when mutating data structures shared
// between main code and the interrupt handler.
void dcd_int_enable(uint8_t rhport)
{
  (void) rhport;
	irq_setmask(irq_getmask() | (1 << USB_INTERRUPT));
}

void dcd_int_disable(uint8_t rhport)
{
  (void) rhport;
  irq_setmask(irq_getmask() & ~(1 << USB_INTERRUPT));
}

// Called when the device is given a new bus address.
void dcd_set_address(uint8_t rhport, uint8_t dev_addr)
{
  // Respond with ACK status first before changing device address
  dcd_edpt_xfer(rhport, tu_edpt_addr(0, TUSB_DIR_IN), NULL, 0);

  // Wait for the response packet to get sent
  while (tx_active)
    ;

  // Activate the new address
  usb_address_write(dev_addr);
  last_address = dev_addr;
}

// Called when the device received SET_CONFIG request, you can leave this
// empty if your peripheral does not require any specific action.
void dcd_set_config(uint8_t rhport, uint8_t config_num)
{
  (void) rhport;
  (void) config_num;
}

// Called to remote wake up host when suspended (e.g hid keyboard)
void dcd_remote_wakeup(uint8_t rhport)
{
  (void) rhport;
}

//--------------------------------------------------------------------+
// DCD Endpoint Port
//--------------------------------------------------------------------+
bool dcd_edpt_open(uint8_t rhport, tusb_desc_endpoint_t const * p_endpoint_desc)
{
  (void) rhport;
  uint8_t ep_num = tu_edpt_number(p_endpoint_desc->bEndpointAddress);
  uint8_t ep_dir = tu_edpt_dir(p_endpoint_desc->bEndpointAddress);

  if (p_endpoint_desc->bmAttributes.xfer == TUSB_XFER_ISOCHRONOUS)
    return false; // Not supported

  if (ep_dir == TUSB_DIR_OUT) {
    rx_buffer_offset[ep_num] = 0;
    rx_buffer_max[ep_num] = 0;
    rx_buffer[ep_num] = NULL;
  }

  else if (ep_dir == TUSB_DIR_OUT) {
    tx_buffer_offset[ep_num] = 0;
    tx_buffer_max[ep_num] = 0;
    tx_buffer[ep_num] = NULL;
  }

  return true;
}

void dcd_edpt_stall(uint8_t rhport, uint8_t ep_addr)
{
  (void) rhport;
  if (tu_edpt_number(ep_addr) == 2)
    fomu_error(__LINE__);

  if (tu_edpt_dir(ep_addr) == TUSB_DIR_OUT) {
    uint8_t enable = 0;
    if (rx_buffer[ep_addr])
      enable = 1;
    usb_out_ctrl_write((1 << CSR_USB_OUT_CTRL_STALL_OFFSET) | (enable << CSR_USB_OUT_CTRL_ENABLE_OFFSET) | tu_edpt_number(ep_addr));
  }
  else
    usb_in_ctrl_write((1 << CSR_USB_IN_CTRL_STALL_OFFSET) | tu_edpt_number(ep_addr));
}

void dcd_edpt_clear_stall(uint8_t rhport, uint8_t ep_addr)
{
  (void) rhport;
  if (tu_edpt_dir(ep_addr) == TUSB_DIR_OUT) {
    uint8_t enable = 0;
    if (rx_buffer[ep_addr])
      enable = 1;
    usb_out_ctrl_write((0 << CSR_USB_OUT_CTRL_STALL_OFFSET) | (enable << CSR_USB_OUT_CTRL_ENABLE_OFFSET) | tu_edpt_number(ep_addr));
  }
  // IN endpoints will get unstalled when more data is written.
}

bool dcd_edpt_xfer (uint8_t rhport, uint8_t ep_addr, uint8_t* buffer, uint16_t total_bytes)
{
  (void)rhport;
  uint8_t ep_num = tu_edpt_number(ep_addr);
  uint8_t ep_dir = tu_edpt_dir(ep_addr);
  TU_ASSERT(ep_num < 16);

  // Give a nonzero buffer when we transmit 0 bytes, so that the
  // system doesn't think the endpoint is idle.
  if ((buffer == NULL) && (total_bytes == 0)) {
    buffer = (uint8_t *)0xffffffff;
  }

  TU_ASSERT(buffer != NULL);

  if (ep_dir == TUSB_DIR_IN) {
    // Wait for the tx pipe to free up
    uint8_t previous_reset_count = reset_count;
    // Continue until the buffer is empty, the system is idle, and the fifo is empty.
    while (tx_buffer[ep_num] != NULL)
      ;

    dcd_int_disable(0);
    if (total_bytes == 499)
      asm("ebreak");
    // If a reset happens while we're waiting, abort the transfer
    if (previous_reset_count != reset_count)
      return true;

    TU_ASSERT(tx_buffer[ep_num] == NULL);
    tx_buffer_offset[ep_num] = 0;
    tx_buffer_max[ep_num] = total_bytes;
    tx_buffer[ep_num] = buffer;

    // If the current buffer is NULL, then that means the tx logic is idle.
    // Update the tx_ep to point to our endpoint number and queue the data.
    // Otherwise, let it be and it'll get picked up after the next transfer
    // finishes.
    if (!tx_active) {
      tx_ep = ep_num;
      tx_active = true;
      tx_more_data();
    }
    dcd_int_enable(0);
  }

  else if (ep_dir == TUSB_DIR_OUT) {
    while (rx_buffer[ep_num] != NULL)
      ;

    dcd_int_disable(0);
    TU_ASSERT(rx_buffer[ep_num] == NULL);
    rx_buffer[ep_num] = buffer;
    rx_buffer_offset[ep_num] = 0;
    rx_buffer_max[ep_num] = total_bytes;

    // Enable receiving on this particular endpoint
    usb_out_ctrl_write((1 << CSR_USB_OUT_CTRL_ENABLE_OFFSET) | ep_num);
// #if DEBUG
//     uint16_t ep_en_mask = usb_out_enable_status_read();
//     int i;
//     for (i = 0; i < 16; i++) {
//       if ((!!(ep_en_mask & (1 << i))) ^ (!!(rx_buffer[i]))) {
//         if (rx_buffer[i] && usb_out_ev_pending_read() && (usb_out_status_read() & 0xf) == i)
//           continue;
//         fomu_error(__LINE__);
//       }
//     }
// #endif
    dcd_int_enable(0);
  }
  return true;
}

//--------------------------------------------------------------------+
// ISR
//--------------------------------------------------------------------+

void hal_dcd_isr(uint8_t rhport)
{
  uint8_t setup_pending   = usb_setup_ev_pending_read() & usb_setup_ev_enable_read();
  uint8_t in_pending      = usb_in_ev_pending_read() & usb_in_ev_enable_read();
  uint8_t out_pending     = usb_out_ev_pending_read() & usb_out_ev_enable_read();
#if !DEBUG
  uint8_t setup_packet_bfr[10];
#endif
  usb_setup_ev_pending_write(setup_pending);

  // This event means a bus reset occurred.  Reset everything, and
  // abandon any further processing.
  if (setup_pending & 2) {
    dcd_reset();
    return;
  }

  // An "IN" transaction just completed.
  // Note that due to the way tinyusb's callback system is implemented,
  // we must handle IN and OUT packets before we handle SETUP packets.
  // This ensures that any responses to SETUP packets aren't overwritten.
  // For example, oftentimes a host will request part of a descriptor
  // to begin with, then make a subsequent request.  If we don't handle
  // the IN packets first, then the second request will be truncated.
  if (in_pending) {
#if DEBUG
    if (!usb_in_ev_enable_read())
      fomu_error(__LINE__);
#endif
    usb_in_ev_pending_write(in_pending);
    process_tx();
  } 

  // An "OUT" transaction just completed so we have new data.
  // (But only if we can accept the data)
  // if (out_pending) {
  if (out_pending) {
#if DEBUG
    if (!usb_out_ev_enable_read())
      fomu_error(__LINE__);
#endif
    process_rx();
  }

  // We got a SETUP packet.  Copy it to the setup buffer and clear
  // the "pending" bit.
  if (setup_pending & 1) {
    // Setup packets are always 8 bytes, plus two bytes of crc16.
    uint32_t setup_length = 0;

#if DEBUG
    if (!(usb_setup_status_read() & (1 << CSR_USB_SETUP_STATUS_HAVE_OFFSET)))
      fomu_error(__LINE__);
#endif

    while (usb_setup_status_read() & (1 << CSR_USB_SETUP_STATUS_HAVE_OFFSET)) {
      uint8_t c = usb_setup_data_read();
      if (setup_length < sizeof(setup_packet_bfr))
        setup_packet_bfr[setup_length] = c;
      setup_length++;
    }

    // If we have 10 bytes, that's a full SETUP packet plus CRC16.
    // Otherwise, it was an RX error.
    if (setup_length == 10) {
      dcd_event_setup_received(rhport, setup_packet_bfr, true);
      // Acknowledge the packet, so long as it isn't a SET_ADDRESS
      // packet.  If it is, leave it unacknowledged and we'll do this
      // in the `dcd_set_address` function instead.
      // if (!((setup_packet_bfr[0] == 0x00) && (setup_packet_bfr[1] == 0x05)))
      usb_setup_ctrl_write(1 << CSR_USB_SETUP_CTRL_ACK_OFFSET);
    }
#if DEBUG
    else {
      fomu_error(__LINE__);
    }
#endif
  }
}

#endif
