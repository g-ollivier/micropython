/*
 * This file is part of the Micro Python project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2013, 2014 Damien P. George
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

#include <stdint.h>
#include <string.h>

#include "asf/sam0/drivers/sercom/spi/spi.h"

#include "py/gc.h"
#include "py/obj.h"
#include "py/runtime.h"
#include "lib/fatfs/ff.h"
#include "extmod/fsusermount.h"

#include "asf/sam0/drivers/nvm/nvm.h"

#include "spi_flash.h"

#define SPI_FLASH_PART1_START_BLOCK (0x1)

#define NO_SECTOR_LOADED 0xFFFFFFFF

#define CMD_READ_JEDEC_ID 0x9f
#define CMD_READ_DATA 0x03
#define CMD_SECTOR_ERASE 0x20
// #define CMD_SECTOR_ERASE CMD_READ_JEDEC_ID
#define CMD_ENABLE_WRITE 0x06
#define CMD_PAGE_PROGRAM 0x02
// #define CMD_PAGE_PROGRAM CMD_READ_JEDEC_ID
#define CMD_READ_STATUS 0x05

static bool spi_flash_is_initialised = false;

struct spi_module spi_flash_instance;

// The total size of the flash.
static uint32_t flash_size;

// The erase sector size.
static uint32_t sector_size;

// The page size. Its the maximum number of bytes that can be written at once.
static uint32_t page_size;

// The currently cached sector in the cache, ram or flash based.
static uint32_t current_sector;

// Track which blocks (up to 32) in the current sector currently live in the
// cache.
static uint32_t dirty_mask;

// We use this when we can allocate the whole cache in RAM.
static uint8_t** ram_cache;

// Address of the scratch flash sector.
#define SCRATCH_SECTOR (flash_size - sector_size)

// Enable the flash over SPI.
static void flash_enable() {
    port_pin_set_output_level(SPI_FLASH_CS, false);
}

// Disable the flash over SPI.
static void flash_disable() {
    port_pin_set_output_level(SPI_FLASH_CS, true);
}

// Wait until both the write enable and write in progress bits have cleared.
static bool wait_for_flash_ready() {
    uint8_t status_request[2] = {CMD_READ_STATUS, 0x00};
    uint8_t response[2] = {0x00, 0x01};
    enum status_code status = STATUS_OK;
    // Both the write enable and write in progress bits should be low.
    while (status == STATUS_OK && ((response[1] & 0x1) == 1 || (response[1] & 0x2) == 2)) {
        flash_enable();
        status = spi_transceive_buffer_wait(&spi_flash_instance, status_request, response, 2);
        flash_disable();
    }
    return status == STATUS_OK;
}

// Turn on the write enable bit so we can program and erase the flash.
static bool write_enable() {
    flash_enable();
    uint8_t command = CMD_ENABLE_WRITE;
    enum status_code status = spi_write_buffer_wait(&spi_flash_instance, &command, 1);
    flash_disable();
    return status == STATUS_OK;
}

// Pack the low 24 bits of the address into a uint8_t array.
static void address_to_bytes(uint32_t address, uint8_t* bytes) {
    bytes[0] = (address >> 16) & 0xff;
    bytes[1] = (address >> 8) & 0xff;
    bytes[2] = address & 0xff;
}

// Read data_length's worth of bytes starting at address into data.
static bool read_flash(uint32_t address, uint8_t* data, uint32_t data_length) {
    wait_for_flash_ready();
    enum status_code status;
    // We can read as much as we want sequentially.
    uint8_t read_request[4] = {CMD_READ_DATA, 0x00, 0x00, 0x00};
    address_to_bytes(address, read_request + 1);
    flash_enable();
    status = spi_write_buffer_wait(&spi_flash_instance, read_request, 4);
    if (status == STATUS_OK) {
        status = spi_read_buffer_wait(&spi_flash_instance, data, data_length, 0x00);
    }
    flash_disable();
    return status == STATUS_OK;
}

// Writes data_length's worth of bytes starting at address from data. Assumes
// that the sector that address resides in has already been erased. So make sure
// to run erase_sector.
static bool write_flash(uint32_t address, const uint8_t* data, uint32_t data_length) {
    if (page_size == 0) {
        return false;
    }
    for (uint32_t bytes_written = 0;
        bytes_written < data_length;
        bytes_written += page_size) {
        if (!wait_for_flash_ready() || !write_enable()) {
            return false;
        }
        flash_enable();
        uint8_t command[4] = {CMD_PAGE_PROGRAM, 0x00, 0x00, 0x00};
        address_to_bytes(address + bytes_written, command + 1);
        enum status_code status;
        status = spi_write_buffer_wait(&spi_flash_instance, command, 4);
        if (status == STATUS_OK) {
            status = spi_write_buffer_wait(&spi_flash_instance, data + bytes_written, page_size);
        }
        flash_disable();
        if (status != STATUS_OK) {
            return false;
        }
    }
    return true;
}

// Erases the given sector. Make sure you copied all of the data out of it you
// need! Also note, sector_address is really 24 bits.
static bool erase_sector(uint32_t sector_address) {
    // Before we erase the sector we need to wait for any writes to finish and
    // and then enable the write again.
    if (!wait_for_flash_ready() || !write_enable()) {
        return false;
    }

    uint8_t erase_request[4] = {CMD_SECTOR_ERASE, 0x00, 0x00, 0x00};
    address_to_bytes(sector_address, erase_request + 1);

    flash_enable();
    enum status_code status = spi_write_buffer_wait(&spi_flash_instance, erase_request, 4);
    flash_disable();
    return status == STATUS_OK;
}

// Sector is really 24 bits.
static bool copy_block(uint32_t src_address, uint32_t dest_address) {
    // Copy page by page to minimize RAM buffer.
    uint8_t buffer[page_size];
    for (int i = 0; i < FLASH_BLOCK_SIZE / page_size; i++) {
        if (!read_flash(src_address + i * page_size, buffer, page_size)) {
            return false;
        }
        if (!write_flash(dest_address + i * page_size, buffer, page_size)) {
            return false;
        }
    }
    return true;
}

void spi_flash_init(void) {
    if (!spi_flash_is_initialised) {
        struct spi_config config_spi_master;
        spi_get_config_defaults(&config_spi_master);
        config_spi_master.mux_setting = SPI_FLASH_MUX_SETTING;
        config_spi_master.pinmux_pad0 = SPI_FLASH_PAD0_PINMUX;
        config_spi_master.pinmux_pad1 = SPI_FLASH_PAD1_PINMUX;
        config_spi_master.pinmux_pad2 = SPI_FLASH_PAD2_PINMUX;
        config_spi_master.pinmux_pad3 = SPI_FLASH_PAD3_PINMUX;
        config_spi_master.mode_specific.master.baudrate = SPI_FLASH_BAUDRATE;
        spi_init(&spi_flash_instance, SPI_FLASH_SERCOM, &config_spi_master);
        spi_enable(&spi_flash_instance);

        // Manage chip select ourselves.
        struct port_config pin_conf;
        port_get_config_defaults(&pin_conf);

        pin_conf.direction  = PORT_PIN_DIR_OUTPUT;
        port_pin_set_config(SPI_FLASH_CS, &pin_conf);
        flash_disable();

        // Activity LED for flash writes.
        #ifdef MICROPY_HW_LED_MSC
            port_pin_set_config(MICROPY_HW_LED_MSC, &pin_conf);
            port_pin_set_output_level(MICROPY_HW_LED_MSC, false);
        #endif

        uint8_t jedec_id_request[4] = {CMD_READ_JEDEC_ID, 0x00, 0x00, 0x00};
        uint8_t response[4] = {0x00, 0x00, 0x00, 0x00};
        flash_enable();
        volatile enum status_code status = spi_transceive_buffer_wait(&spi_flash_instance, jedec_id_request, response, 4);
        flash_disable();
        (void) status;
        if (response[1] == 0x01 && response[2] == 0x40 && response[3] == 0x15) {
            flash_size = 1 << 21; // 2 MiB
            sector_size = 1 << 12; // 4 KiB
            page_size = 256; // 256 bytes
        } else {
            // Unknown flash chip!
            flash_size = 0;
        }

        current_sector = NO_SECTOR_LOADED;
        dirty_mask = 0;
        ram_cache = NULL;

        spi_flash_is_initialised = true;
    }
}

// The size of each individual block.
uint32_t spi_flash_get_block_size(void) {
    return FLASH_BLOCK_SIZE;
}

// The total number of available blocks.
uint32_t spi_flash_get_block_count(void) {
    // We subtract one erase sector size because we may use it as a staging area
    // for writes.
    return SPI_FLASH_PART1_START_BLOCK + (flash_size - sector_size) / FLASH_BLOCK_SIZE;
}

// Flush the cache that was written to the scratch portion of flash. Only used
// when ram is tight.
static bool flush_scratch_flash() {
    // First, copy out any blocks that we haven't touched from the sector we've
    // cached.
    bool copy_to_scratch_ok = true;
    for (int i = 0; i < sector_size / FLASH_BLOCK_SIZE; i++) {
        if ((dirty_mask & (1 << i)) == 0) {
            copy_to_scratch_ok = copy_to_scratch_ok &&
                copy_block(current_sector + i * FLASH_BLOCK_SIZE,
                           SCRATCH_SECTOR + i * FLASH_BLOCK_SIZE);
        }
    }
    if (!copy_to_scratch_ok) {
        // TODO(tannewt): Do more here. We opted to not erase and copy bad data
        // in. We still risk losing the data written to the scratch sector.
        return false;
    }
    // Second, erase the current sector.
    erase_sector(current_sector);
    // Finally, copy the new version into it.
    for (int i = 0; i < sector_size / FLASH_BLOCK_SIZE; i++) {
        copy_block(SCRATCH_SECTOR + i * FLASH_BLOCK_SIZE,
                   current_sector + i * FLASH_BLOCK_SIZE);
    }
    return true;
}

// Attempts to allocate a new set of page buffers for caching a full sector in
// ram. Each page is allocated separately so that the GC doesn't need to provide
// one huge block. We can free it as we write if we want to also.
static bool allocate_ram_cache() {
    uint8_t blocks_per_sector = sector_size / FLASH_BLOCK_SIZE;
    uint8_t pages_per_block = FLASH_BLOCK_SIZE / page_size;
    ram_cache = gc_alloc(blocks_per_sector * pages_per_block * sizeof(uint32_t), false);
    if (ram_cache == NULL) {
        return false;
    }
    // Declare i and j outside the loops in case we fail to allocate everything
    // we need. In that case we'll give it back.
    int i = 0;
    int j = 0;
    bool success = true;
    for (i = 0; i < sector_size / FLASH_BLOCK_SIZE; i++) {
        for (int j = 0; j < pages_per_block; j++) {
            uint8_t *page_cache = gc_alloc(page_size, false);
            if (page_cache == NULL) {
                success = false;
                break;
            }
            ram_cache[i * pages_per_block + j] = page_cache;
        }
        if (!success) {
            break;
        }
    }
    // We couldn't allocate enough so give back what we got.
    if (!success) {
        for (; i >= 0; i--) {
            for (; j >= 0; j--) {
                gc_free(ram_cache[i * pages_per_block + j]);
            }
            j = pages_per_block - 1;
        }
        gc_free(ram_cache);
        ram_cache = NULL;
    }
    return success;
}

// Flush the cached sector from ram onto the flash. We'll free the cache unless
// keep_cache is true.
static bool flush_ram_cache(bool keep_cache) {
    // First, copy out any blocks that we haven't touched from the sector
    // we've cached. If we don't do this we'll erase the data during the sector
    // erase below.
    bool copy_to_ram_ok = true;
    uint8_t pages_per_block = FLASH_BLOCK_SIZE / page_size;
    for (int i = 0; i < sector_size / FLASH_BLOCK_SIZE; i++) {
        if ((dirty_mask & (1 << i)) == 0) {
            for (int j = 0; j < pages_per_block; j++) {
                copy_to_ram_ok = read_flash(
                    current_sector + (i * pages_per_block + j) * page_size,
                    ram_cache[i * pages_per_block + j],
                    page_size);
                if (!copy_to_ram_ok) {
                    break;
                }
            }
        }
        if (!copy_to_ram_ok) {
            break;
        }
    }

    if (!copy_to_ram_ok) {
        return false;
    }
    // Second, erase the current sector.
    erase_sector(current_sector);
    // Lastly, write all the data in ram that we've cached.
    for (int i = 0; i < sector_size / FLASH_BLOCK_SIZE; i++) {
        for (int j = 0; j < pages_per_block; j++) {
            write_flash(current_sector + (i * pages_per_block + j) * page_size,
                        ram_cache[i * pages_per_block + j],
                        page_size);
            if (!keep_cache) {
                gc_free(ram_cache[i * pages_per_block + j]);
            }
        }
    }
    // We're done with the cache for now so give it back.
    if (!keep_cache) {
        gc_free(ram_cache);
        ram_cache = NULL;
    }
    return true;
}

// Delegates to the correct flash flush method depending on the existing cache.
static void spi_flash_flush_keep_cache(bool keep_cache) {
    if (current_sector == NO_SECTOR_LOADED) {
        return;
    }
    #ifdef MICROPY_HW_LED_MSC
        port_pin_set_output_level(MICROPY_HW_LED_MSC, true);
    #endif
    // If we've cached to the flash itself flush from there.
    if (ram_cache == NULL) {
        flush_scratch_flash();
    } else {
        flush_ram_cache(keep_cache);
    }
    current_sector = NO_SECTOR_LOADED;
    #ifdef MICROPY_HW_LED_MSC
        port_pin_set_output_level(MICROPY_HW_LED_MSC, false);
    #endif
}

// External flash function used. If called externally we assume we won't need
// the cache after.
void spi_flash_flush(void) {
    spi_flash_flush_keep_cache(false);
}

// Builds a partition entry for the MBR.
static void build_partition(uint8_t *buf, int boot, int type,
                            uint32_t start_block, uint32_t num_blocks) {
    buf[0] = boot;

    if (num_blocks == 0) {
        buf[1] = 0;
        buf[2] = 0;
        buf[3] = 0;
    } else {
        buf[1] = 0xff;
        buf[2] = 0xff;
        buf[3] = 0xff;
    }

    buf[4] = type;

    if (num_blocks == 0) {
        buf[5] = 0;
        buf[6] = 0;
        buf[7] = 0;
    } else {
        buf[5] = 0xff;
        buf[6] = 0xff;
        buf[7] = 0xff;
    }

    buf[8] = start_block;
    buf[9] = start_block >> 8;
    buf[10] = start_block >> 16;
    buf[11] = start_block >> 24;

    buf[12] = num_blocks;
    buf[13] = num_blocks >> 8;
    buf[14] = num_blocks >> 16;
    buf[15] = num_blocks >> 24;
}

static uint32_t convert_block_to_flash_addr(uint32_t block) {
    if (SPI_FLASH_PART1_START_BLOCK <= block && block < spi_flash_get_block_count()) {
        // a block in partition 1
        block -= SPI_FLASH_PART1_START_BLOCK;
        return block * FLASH_BLOCK_SIZE;
    }
    // bad block
    return -1;
}

bool spi_flash_read_block(uint8_t *dest, uint32_t block) {
    if (block == 0) {
        // Fake the MBR so we can decide on our own partition table
        for (int i = 0; i < 446; i++) {
            dest[i] = 0;
        }

        build_partition(dest + 446, 0, 0x01 /* FAT12 */,
                        SPI_FLASH_PART1_START_BLOCK,
                        spi_flash_get_block_count() - SPI_FLASH_PART1_START_BLOCK);
        build_partition(dest + 462, 0, 0, 0, 0);
        build_partition(dest + 478, 0, 0, 0, 0);
        build_partition(dest + 494, 0, 0, 0, 0);

        dest[510] = 0x55;
        dest[511] = 0xaa;

        return true;
    } else if (block < SPI_FLASH_PART1_START_BLOCK) {
        memset(dest, 0, FLASH_BLOCK_SIZE);
        return true;
    } else {
        // Non-MBR block, get data from flash memory.
        uint32_t address = convert_block_to_flash_addr(block);
        if (address == -1) {
            // bad block number
            return false;
        }

        // Mask out the lower bits that designate the address within the sector.
        uint32_t this_sector = address & (~(sector_size - 1));
        uint8_t block_index = (address / FLASH_BLOCK_SIZE) % (sector_size / FLASH_BLOCK_SIZE);
        uint8_t mask = 1 << (block_index);
        // We're reading from the currently cached sector.
        if (current_sector == this_sector && (mask & dirty_mask) > 0) {
            if (ram_cache != NULL) {
                uint8_t pages_per_block = FLASH_BLOCK_SIZE / page_size;
                for (int i = 0; i < pages_per_block; i++) {
                    memcpy(dest + i * page_size,
                           ram_cache[block_index * pages_per_block + i],
                           page_size);
                }
                return true;
            } else {
                uint32_t scratch_address = SCRATCH_SECTOR + block_index * FLASH_BLOCK_SIZE;
                return read_flash(scratch_address, dest, FLASH_BLOCK_SIZE);
            }
        }
        return read_flash(address, dest, FLASH_BLOCK_SIZE);
    }
}

bool spi_flash_write_block(const uint8_t *data, uint32_t block) {
    if (block < SPI_FLASH_PART1_START_BLOCK) {
        // Fake writing below the flash partition.
        return true;
    } else {
        // Non-MBR block, copy to cache
        uint32_t address = convert_block_to_flash_addr(block);
        if (address == -1) {
            // bad block number
            return false;
        }
        // Wait for any previous writes to finish.
        wait_for_flash_ready();
        // Mask out the lower bits that designate the address within the sector.
        uint32_t this_sector = address & (~(sector_size - 1));
        uint8_t block_index = (address / FLASH_BLOCK_SIZE) % (sector_size / FLASH_BLOCK_SIZE);
        uint8_t mask = 1 << (block_index);
        // Flush the cache if we're moving onto a sector our we're writing the
        // same block again.
        if (current_sector != this_sector || (mask & dirty_mask) > 0) {
            if (current_sector != NO_SECTOR_LOADED) {
                spi_flash_flush_keep_cache(true);
            }
            if (ram_cache == NULL && !allocate_ram_cache()) {
                erase_sector(SCRATCH_SECTOR);
                wait_for_flash_ready();
            }
            current_sector = this_sector;
            dirty_mask = 0;
        }
        dirty_mask |= mask;
        // Copy the block to the appropriate cache.
        if (ram_cache != NULL) {
            uint8_t pages_per_block = FLASH_BLOCK_SIZE / page_size;
            for (int i = 0; i < pages_per_block; i++) {
                memcpy(ram_cache[block_index * pages_per_block + i],
                       data + i * page_size,
                       page_size);
            }
            return true;
        } else {
            uint32_t scratch_address = SCRATCH_SECTOR + block_index * FLASH_BLOCK_SIZE;
            return write_flash(scratch_address, data, FLASH_BLOCK_SIZE);
        }
    }
}

mp_uint_t spi_flash_read_blocks(uint8_t *dest, uint32_t block_num, uint32_t num_blocks) {
    for (size_t i = 0; i < num_blocks; i++) {
        if (!spi_flash_read_block(dest + i * FLASH_BLOCK_SIZE, block_num + i)) {
            return 1; // error
        }
    }
    return 0; // success
}

mp_uint_t spi_flash_write_blocks(const uint8_t *src, uint32_t block_num, uint32_t num_blocks) {
    for (size_t i = 0; i < num_blocks; i++) {
        if (!spi_flash_write_block(src + i * FLASH_BLOCK_SIZE, block_num + i)) {
            return 1; // error
        }
    }
    return 0; // success
}

/******************************************************************************/
// MicroPython bindings
//
// Expose the flash as an object with the block protocol.

// there is a singleton Flash object
STATIC const mp_obj_base_t spi_flash_obj = {&spi_flash_type};

STATIC mp_obj_t spi_flash_obj_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *args) {
    // check arguments
    mp_arg_check_num(n_args, n_kw, 0, 0, false);

    // return singleton object
    return (mp_obj_t)&spi_flash_obj;
}

STATIC mp_obj_t spi_flash_obj_readblocks(mp_obj_t self, mp_obj_t block_num, mp_obj_t buf) {
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(buf, &bufinfo, MP_BUFFER_WRITE);
    mp_uint_t ret = spi_flash_read_blocks(bufinfo.buf, mp_obj_get_int(block_num), bufinfo.len / FLASH_BLOCK_SIZE);
    return MP_OBJ_NEW_SMALL_INT(ret);
}
STATIC MP_DEFINE_CONST_FUN_OBJ_3(spi_flash_obj_readblocks_obj, spi_flash_obj_readblocks);

STATIC mp_obj_t spi_flash_obj_writeblocks(mp_obj_t self, mp_obj_t block_num, mp_obj_t buf) {
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(buf, &bufinfo, MP_BUFFER_READ);
    mp_uint_t ret = spi_flash_write_blocks(bufinfo.buf, mp_obj_get_int(block_num), bufinfo.len / FLASH_BLOCK_SIZE);
    return MP_OBJ_NEW_SMALL_INT(ret);
}
STATIC MP_DEFINE_CONST_FUN_OBJ_3(spi_flash_obj_writeblocks_obj, spi_flash_obj_writeblocks);

STATIC mp_obj_t spi_flash_obj_ioctl(mp_obj_t self, mp_obj_t cmd_in, mp_obj_t arg_in) {
    mp_int_t cmd = mp_obj_get_int(cmd_in);
    switch (cmd) {
        case BP_IOCTL_INIT: spi_flash_init(); return MP_OBJ_NEW_SMALL_INT(0);
        case BP_IOCTL_DEINIT: spi_flash_flush(); return MP_OBJ_NEW_SMALL_INT(0); // TODO properly
        case BP_IOCTL_SYNC: spi_flash_flush(); return MP_OBJ_NEW_SMALL_INT(0);
        case BP_IOCTL_SEC_COUNT: return MP_OBJ_NEW_SMALL_INT(spi_flash_get_block_count());
        case BP_IOCTL_SEC_SIZE: return MP_OBJ_NEW_SMALL_INT(spi_flash_get_block_size());
        default: return mp_const_none;
    }
}
STATIC MP_DEFINE_CONST_FUN_OBJ_3(spi_flash_obj_ioctl_obj, spi_flash_obj_ioctl);

STATIC const mp_map_elem_t spi_flash_obj_locals_dict_table[] = {
    { MP_OBJ_NEW_QSTR(MP_QSTR_readblocks), (mp_obj_t)&spi_flash_obj_readblocks_obj },
    { MP_OBJ_NEW_QSTR(MP_QSTR_writeblocks), (mp_obj_t)&spi_flash_obj_writeblocks_obj },
    { MP_OBJ_NEW_QSTR(MP_QSTR_ioctl), (mp_obj_t)&spi_flash_obj_ioctl_obj },
};

STATIC MP_DEFINE_CONST_DICT(spi_flash_obj_locals_dict, spi_flash_obj_locals_dict_table);

const mp_obj_type_t spi_flash_type = {
    { &mp_type_type },
    .name = MP_QSTR_SPIFlash,
    .make_new = spi_flash_obj_make_new,
    .locals_dict = (mp_obj_t)&spi_flash_obj_locals_dict,
};

void flash_init_vfs(fs_user_mount_t *vfs) {
    vfs->flags |= FSUSER_NATIVE | FSUSER_HAVE_IOCTL | FSUSER_USB_WRITEABLE;
    vfs->readblocks[0] = (mp_obj_t)&spi_flash_obj_readblocks_obj;
    vfs->readblocks[1] = (mp_obj_t)&spi_flash_obj;
    vfs->readblocks[2] = (mp_obj_t)spi_flash_read_blocks; // native version
    vfs->writeblocks[0] = (mp_obj_t)&spi_flash_obj_writeblocks_obj;
    vfs->writeblocks[1] = (mp_obj_t)&spi_flash_obj;
    vfs->writeblocks[2] = (mp_obj_t)spi_flash_write_blocks; // native version
    vfs->u.ioctl[0] = (mp_obj_t)&spi_flash_obj_ioctl_obj;
    vfs->u.ioctl[1] = (mp_obj_t)&spi_flash_obj;
}
