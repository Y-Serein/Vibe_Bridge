MEMORY
{
    FLASH(rx) : ORIGIN = 0x10000000,
                LENGTH = (4 * 1024 * 1024 - 128 * 1024)
    PARAM(rx) : ORIGIN = 0x10000000 + (4 * 1024 * 1024 - 128 * 1024),
                LENGTH = (128 * 1024)
}

