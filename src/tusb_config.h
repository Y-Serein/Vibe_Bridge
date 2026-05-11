#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

#ifdef __cplusplus
extern "C" {
#endif

#define CFG_TUSB_MCU          OPT_MCU_RP2040
#define BOARD_TUD_MAX_SPEED   OPT_MODE_FULL_SPEED
#define CFG_TUD_MAX_SPEED     OPT_MODE_FULL_SPEED
#define CFG_TUSB_RHPORT0_MODE OPT_MODE_DEVICE
#define BOARD_TUD_RHPORT      0

#ifdef CFG_TUSB_OS
#    undef CFG_TUSB_OS
#endif
#define CFG_TUSB_OS OPT_OS_NONE

#ifndef CFG_TUSB_DEBUG
#    define CFG_TUSB_DEBUG 0
#endif

// Enable Device stack
#define CFG_TUD_ENABLED 1

/* USB DMA on some MCUs can only access a specific SRAM region with restriction
 * on alignment. Tinyusb use follows macros to declare transferring memory so
 * that they can be put into those specific section. e.g
 * - CFG_TUSB_MEM SECTION : __attribute__ (( section(".usb_ram") ))
 * - CFG_TUSB_MEM_ALIGN   : __attribute__ ((aligned(4)))
 */
#ifndef CFG_TUSB_MEM_SECTION
#    define CFG_TUSB_MEM_SECTION
#endif

#ifndef CFG_TUSB_MEM_ALIGN
#    define CFG_TUSB_MEM_ALIGN __attribute__((aligned(4)))
#endif

//--------------------------------------------------------------------
// DEVICE CONFIGURATION
//--------------------------------------------------------------------

#ifndef CFG_TUD_ENDPOINT0_SIZE
#    define CFG_TUD_ENDPOINT0_SIZE 64
#endif

//------------- CLASS -------------//
// The number of video control interfaces
#define CFG_TUD_VIDEO 1

// The number of video streaming interfaces
#define CFG_TUD_VIDEO_STREAMING 1

// video streaming endpoint size
// 160x120 YUY2 at 10fps: ceil(38400 bytes / 100 ms) + 2-byte UVC header.
// Match TinyUSB's negotiated dwMaxPayloadTransferSize to avoid over-reserving
// full-speed isochronous bandwidth on Linux.
#define CFG_TUD_VIDEO_STREAMING_EP_BUFSIZE 386

#endif /* _TUSB_CONFIG_H_ */
