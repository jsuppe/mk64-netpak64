/* PI-manager-arbitrated single-word IO, as in vanilla libultra. The retail
 * mk64 link never needed these; the vendored SC64 USB transport (src/usb.c,
 * task #46) calls them so its PI traffic serializes against the game's own
 * DMA (audio banks, course loads) through the PI access queue. */
#include "libultra_internal.h"
#include "hardware.h"

extern u32 osRomBase;

s32 osPiRawWriteIo(u32 devAddr, u32 data) {
    register int status;
    status = HW_REG(PI_STATUS_REG, u32);
    while (status & (PI_STATUS_BUSY | PI_STATUS_IOBUSY | PI_STATUS_ERROR)) {
        status = HW_REG(PI_STATUS_REG, u32);
    }
    HW_REG(osRomBase | devAddr, u32) = data;
    return 0;
}

s32 osPiReadIo(u32 devAddr, u32* data) {
    s32 ret;
    __osPiGetAccess();
    ret = osPiRawReadIo(devAddr, data);
    __osPiRelAccess();
    return ret;
}

s32 osPiWriteIo(u32 devAddr, u32 data) {
    s32 ret;
    __osPiGetAccess();
    ret = osPiRawWriteIo(devAddr, data);
    __osPiRelAccess();
    return ret;
}
