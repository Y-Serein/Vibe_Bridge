.param (NOLOAD) :
{
    __param_start = .;
    KEEP(*(.param))
    KEEP(*(.param.*))
    __param_end = .;
} > PARAM

