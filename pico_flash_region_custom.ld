/* This is an augmenting linker script, loaded together with the Pico SDK
 * RP2350 memory map. The physical Flash is 2 MiB, but the running image must
 * remain inside the first 768 KiB so its equally sized OTA slot can always
 * stage any image that links successfully. */
ASSERT(__flash_binary_end <= 0x100c0000,
       "active firmware exceeds the 768 KiB OTA-compatible slot")
