/*
 * This file is part of the Micro Python project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2016 Scott Shawcroft for Adafruit Industries
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
 */

#include <string.h>

#include "access_vfs.h"

#include "asf/common/services/usb/class/msc/device/udi_msc.h"
#include "extmod/fsusermount.h"
#include "lib/fatfs/diskio.h"
#include "py/mpconfig.h"
#include "py/mpstate.h"
#include "py/misc.h"

#define VFS_INDEX 0

//! This function tests memory state, and starts memory initialization
//! @return                            Ctrl_status
//!   It is ready                ->    CTRL_GOOD
//!   Memory unplug              ->    CTRL_NO_PRESENT
//!   Not initialized or changed ->    CTRL_BUSY
//!   An error occurred          ->    CTRL_FAIL
Ctrl_status vfs_test_unit_ready(void)
{
    if (VFS_INDEX >= MP_ARRAY_SIZE(MP_STATE_PORT(fs_user_mount))) {
        return CTRL_FAIL;
    }
    DSTATUS status = disk_status(VFS_INDEX);
    if (status == STA_NOINIT) {
        return CTRL_NO_PRESENT;
    }

    return CTRL_GOOD;
}

//! This function returns the address of the last valid sector
//! @param uint32_t_nb_sector  Pointer to the last valid sector (sector=512 bytes)
//! @return                            Ctrl_status
//!   It is ready                ->    CTRL_GOOD
//!   Memory unplug              ->    CTRL_NO_PRESENT
//!   Not initialized or changed ->    CTRL_BUSY
//!   An error occurred          ->    CTRL_FAIL
Ctrl_status vfs_read_capacity(uint32_t *last_valid_sector)
{
    if (disk_ioctl(VFS_INDEX, GET_SECTOR_COUNT, last_valid_sector) != RES_OK) {
        return CTRL_FAIL;
    }
    // Subtract one from the sector count to get the last valid sector.
    (*last_valid_sector)--;
    return CTRL_GOOD;
}

//! This function returns the write-protected mode
//!
//! @return true if the memory is protected
//!
bool vfs_wr_protect(void)
{
    if (VFS_INDEX >= MP_ARRAY_SIZE(MP_STATE_PORT(fs_user_mount))) {
        return true;
    }
    fs_user_mount_t *vfs = MP_STATE_PORT(fs_user_mount)[VFS_INDEX];
    if (vfs == NULL) {
        return true;
    }

    // This is used to determine the writeability of the disk from USB.
    if (vfs->writeblocks[0] == MP_OBJ_NULL ||
        (vfs->flags & FSUSER_USB_WRITEABLE) == 0) {
        return true;
    }
    return false;
}

//! This function informs about the memory type
//!
//! @return true if the memory is removable
//!
bool vfs_removal(void)
{
	return true;
}

bool vfs_unload(bool unload)
{
	return unload;
}

// TODO(tannewt): Transfer more than a single sector at a time if we need more
// speed.
//! This function transfers the memory data to the USB MSC interface
//!
//! @param addr         Sector address to start read
//! @param nb_sector    Number of sectors to transfer (sector=512 bytes)
//!
//! @return                            Ctrl_status
//!   It is ready                ->    CTRL_GOOD
//!   Memory unplug              ->    CTRL_NO_PRESENT
//!   Not initialized or changed ->    CTRL_BUSY
//!   An error occurred          ->    CTRL_FAIL
//!
Ctrl_status vfs_usb_read_10(uint32_t addr, volatile uint16_t nb_sector)
{
    uint8_t sector_buffer[FLASH_BLOCK_SIZE];
    for (uint16_t sector = 0; sector < nb_sector; sector++) {
        DRESULT result = disk_read(VFS_INDEX, sector_buffer, addr + sector, 1);
        if (result == RES_PARERR) {
            return CTRL_NO_PRESENT;
        }
        if (result == RES_ERROR) {
            return CTRL_FAIL;
        }
        if (!udi_msc_trans_block(true, sector_buffer, FLASH_BLOCK_SIZE, NULL)) {
            return CTRL_FAIL; // transfer aborted
        }
    }
    return CTRL_GOOD;
}


//! This function transfers the USB MSC data to the memory
//!
//! @param addr         Sector address to start write
//! @param nb_sector    Number of sectors to transfer (sector=512 bytes)
//!
//! @return                            Ctrl_status
//!   It is ready                ->    CTRL_GOOD
//!   Memory unplug              ->    CTRL_NO_PRESENT
//!   Not initialized or changed ->    CTRL_BUSY
//!   An error occurred          ->    CTRL_FAIL
//!
Ctrl_status vfs_usb_write_10(uint32_t addr, volatile uint16_t nb_sector)
{
    uint8_t sector_buffer[FLASH_BLOCK_SIZE];
    for (uint16_t sector = 0; sector < nb_sector; sector++) {
        if (!udi_msc_trans_block(false, sector_buffer, FLASH_BLOCK_SIZE, NULL)) {
            return CTRL_FAIL; // transfer aborted
        }
        uint32_t sector_address = addr + sector;
        DRESULT result = disk_write(VFS_INDEX, sector_buffer, sector_address, 1);
        if (result == RES_PARERR) {
            return CTRL_NO_PRESENT;
        }
        if (result == RES_ERROR) {
            return CTRL_FAIL;
        }
        // Since by getting here we assume the mount is read-only to MicroPython
        // lets update the cached FatFs sector if its the one we just wrote.
        fs_user_mount_t *vfs =  MP_STATE_PORT(fs_user_mount)[VFS_INDEX];
        volatile uint16_t x = addr;
        (void) x;
        #if _MAX_SS != _MIN_SS
        if (vfs->ssize == FLASH_BLOCK_SIZE) {
        #else
        // The compiler can optimize this away.
        if (_MAX_SS == FLASH_BLOCK_SIZE) {
        #endif
            if (sector_address == vfs->fatfs.winsect && sector_address > 0) {
                memcpy(vfs->fatfs.win, sector_buffer, FLASH_BLOCK_SIZE);
            }
        }
    }
    return CTRL_GOOD;
}
