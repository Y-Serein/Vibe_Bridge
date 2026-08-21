#ifndef TUSB_CONFIG_H
#define TUSB_CONFIG_H

#define CFG_TUSB_MCU          OPT_MCU_RP2040
#define BOARD_TUD_MAX_SPEED   OPT_MODE_FULL_SPEED
#define CFG_TUD_MAX_SPEED     OPT_MODE_FULL_SPEED
#define CFG_TUSB_RHPORT0_MODE OPT_MODE_DEVICE
#define BOARD_TUD_RHPORT      0
#ifdef CFG_TUSB_OS
#undef CFG_TUSB_OS
#endif
#define CFG_TUSB_OS OPT_OS_NONE
#define CFG_TUSB_DEBUG        0
#define CFG_TUD_ENABLED       1

#define CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_ALIGN __attribute__((aligned(4)))
#define CFG_TUD_ENDPOINT0_SIZE 64

#define CFG_TUD_VIDEO     0
#define CFG_TUD_CDC       0
#define CFG_TUD_MSC       0
#define CFG_TUD_HID       0
#define CFG_TUD_MIDI      0
#define CFG_TUD_VENDOR    0
#define CFG_TUD_ECM_RNDIS 0
#define CFG_TUD_NCM       1

#define CFG_TUD_NCM_IN_NTB_MAX_SIZE  8192
#define CFG_TUD_NCM_OUT_NTB_MAX_SIZE 8192
#define CFG_TUD_NCM_IN_NTB_N         3
#define CFG_TUD_NCM_OUT_NTB_N        1
#define CFG_TUD_NCM_IN_MAX_DATAGRAMS_PER_NTB  8
#define CFG_TUD_NCM_OUT_MAX_DATAGRAMS_PER_NTB 6

#endif
