/* glue.c — FatFs disk I/O adapter for Drivers/tf.c
 *
 * The reference FatFs_SPI project uses sd_driver here.  This project already
 * has a polled SPI TF driver, so all physical I/O is routed through tf.c.
 */
#include <stdio.h>
#include "ff.h"
#include "diskio.h"
#include "tf.h"

DSTATUS disk_status(BYTE pdrv)
{
    if (pdrv != 0) return STA_NOINIT;
    return TF_IsReady() ? 0 : STA_NOINIT;
}

DSTATUS disk_initialize(BYTE pdrv)
{
    printf("[FATFS] disk_initialize(%u) enter\n", (unsigned)pdrv);
    if (pdrv != 0) {
        printf("[FATFS] invalid pdrv\n");
        return STA_NOINIT;
    }

    printf("[FATFS] calling TF_Init\n");
    int rc = TF_Init();
    printf("[FATFS] TF_Init rc=%d ready=%u\n", rc, (unsigned)TF_IsReady());
    return (rc == TF_OK) ? 0 : STA_NOINIT;
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count)
{
    printf("[FATFS] disk_read lba=%lu count=%u enter\n",
           (unsigned long)sector, (unsigned)count);
    if (pdrv != 0 || !buff || count == 0) return RES_PARERR;
    if (!TF_IsReady()) return RES_NOTRDY;

    int rc = TF_ReadBlocks((uint32_t)sector, buff, (uint32_t)count);
    printf("[FATFS] TF_ReadBlocks rc=%d\n", rc);
    return (rc == TF_OK) ? RES_OK : RES_ERROR;
}

#if FF_FS_READONLY == 0
DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count)
{
    if (pdrv != 0 || !buff || count == 0) return RES_PARERR;
    if (!TF_IsReady()) return RES_NOTRDY;

    return (TF_WriteBlocks((uint32_t)sector, buff, (uint32_t)count) == TF_OK)
        ? RES_OK : RES_ERROR;
}
#endif

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
    if (pdrv != 0 || !buff) return RES_PARERR;
    if (!TF_IsReady()) return RES_NOTRDY;

    switch (cmd) {
    case CTRL_SYNC:
        return RES_OK;

    case GET_SECTOR_COUNT: {
        uint32_t sectors = 0;
        if (TF_GetSectors(&sectors) != TF_OK || sectors == 0) return RES_ERROR;
        *(LBA_t *)buff = (LBA_t)sectors;
        return RES_OK;
    }

    case GET_SECTOR_SIZE:
        *(WORD *)buff = TF_BLOCK_SIZE;
        return RES_OK;

    case GET_BLOCK_SIZE:
        /* Unknown erase granularity: FatFs accepts 1 for non-flash media. */
        *(DWORD *)buff = 1;
        return RES_OK;

    default:
        return RES_PARERR;
    }
}
