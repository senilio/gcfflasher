/*
 * Copyright (c) 2021 dresden elektronik ingenieurtechnik gmbh.
 * All rights reserved.
 *
 * The software in this package is published under the terms of the BSD
 * style license a copy of which has been included with this distribution in
 * the LICENSE.txt file.
 *
 */

/* This file implements the platform independend part of GCFFlasher.
 */

#define APP_VERSION "v4.0.0-beta"

#define __STDC_FORMAT_MACROS
#include <inttypes.h> /* printf types */
#include <stdio.h>
#include <stdlib.h>
#include <string.h> /* memset */
#include <assert.h>
#include "buffer_helper.h"
#include "gcf.h"
#include "protocol.h"

#define MAX_DEVICES 4

#define GCF_HEADER_SIZE 14
#define GCF_MAGIC 0xCAFEFEED

#define FW_VERSION_PLATFORM_MASK 0x0000FF00
#define FW_VERSION_PLATFORM_R21  0x00000700 /* 0x26120700*/
#define FW_VERSION_PLATFORM_AVR  0x00000500 /* 0x26390500*/

/* Bootloader V3.x serial protocol */
#define BTL_MAGIC              0x81
#define BTL_ID_REQUEST         0x02
#define BTL_ID_RESPONSE        0x82
#define BTL_FW_UPDATE_REQUEST  0x03
#define BTL_FW_UPDATE_RESPONSE 0x83
#define BTL_FW_DATA_REQUEST    0x04
#define BTL_FW_DATA_RESPONSE   0x84

/* Bootloader V1 */
#define V1_PAGESIZE 256

typedef void (*state_handler_t)(GCF*, Event);

typedef enum
{
    T_NONE,
    T_RESET,
    T_PROGRAM,
    T_LIST,
    T_CONNECT,
    T_HELP
} Task;

typedef enum
{
    DEV_UNKNOWN,
    DEV_RASPBEE_1,
    DEV_RASPBEE_2,
    DEV_CONBEE_1,
    DEV_CONBEE_2
} DeviceType;

typedef struct GCF_File_t
{
    char fname[64];
    size_t fsize;

    uint32_t fwVersion; /* taken from file name */

    /* parsed GCF file header */
    uint8_t gcfFileType;
    uint32_t gcfTargetAddress;
    uint32_t gcfFileSize;
    uint8_t gcfCrc;

    uint8_t fcontent[MAX_GCF_FILE_SIZE];
} GCF_File;

/* The GCF struct holds the complete state as well as GCF file data. */
typedef struct GCF_t
{
    int argc;
    char **argv;
    uint16_t wp;     /* ascii[] write pointer */
    char ascii[512]; /* buffer for raw data */
    state_handler_t state;
    state_handler_t substate;

    int retry;

    Task task;

    PROT_RxState rxstate;

    uint64_t startTime;
    uint64_t maxTime;

    uint8_t devCount;
    Device devices[MAX_DEVICES];

    DeviceType devType;

    char devpath[MAX_DEV_PATH_LENGTH];
    GCF_File file;
} GCF;


static DeviceType gcfGetDeviceType(const char *devPath);
static void gcfRetry(GCF *gcf);
static void gcfPrintHelp();
static GCF_Status gcfProcessCommandline(GCF *gcf);
static void gcfGetDevices(GCF *gcf);
static void gcfCommandResetUart();
static void gcfCommandQueryStatus();
static void gcfCommandQueryFirmwareVersion();
static void ST_Void(GCF *gcf, Event event);
static void ST_Init(GCF *gcf, Event event);


static void ST_Program(GCF *gcf, Event event);
static void ST_V1ProgramSync(GCF *gcf, Event event);
static void ST_V1ProgramWriteHeader(GCF *gcf, Event event);
static void ST_V1ProgramUpload(GCF *gcf, Event event);
static void ST_V1ProgramValidate(GCF *gcf, Event event);

static void ST_V3ProgramSync(GCF *gcf, Event event);
static void ST_V3ProgramUpload(GCF *gcf, Event event);

static void ST_BootloaderConnect(GCF *gcf, Event event);
static void ST_BootloaderQuery(GCF *gcf, Event event);

static void ST_Connect(GCF *gcf, Event event);
static void ST_Connected(GCF *gcf, Event event);

static void ST_Reset(GCF *gcf, Event event);
static void ST_ResetUart(GCF *gcf, Event event);
static void ST_ResetFtdi(GCF *gcf, Event event);
static void ST_ResetRaspBee(GCF *gcf, Event event);

static void ST_ListDevices(GCF *gcf, Event event);

static GCF *gcfInstance = NULL;


static const char hex_lookup[16] =
{
    '0', '1', '2', '3', '4', '5', '6', '7', '8', '9',
    'A', 'B', 'C', 'D', 'E', 'F',
};

void put_hex(uint8_t ch, char *buf)
{
    buf[0] = hex_lookup[(ch >> 4) & 0xF];
    buf[1] = hex_lookup[(ch & 0x0F)];
}

static void ST_Void(GCF *gcf, Event event)
{
    (void)event;
}

static void ST_Init(GCF *gcf, Event event)
{
    if (event == EV_PL_STARTED || event == EV_TIMEOUT)
    {
        if (gcfProcessCommandline(gcf) == GCF_FAILED)
        {
            PL_ShutDown();
        }
        else
        {
            gcf->state(gcf, EV_ACTION);
        }
    }
}

static void ST_Reset(GCF *gcf, Event event)
{
    if (event == EV_ACTION)
    {
        gcf->substate = ST_ResetUart;
        gcf->substate(gcf, EV_ACTION);
    }
    else if (event == EV_UART_RESET_SUCCESS || event == EV_FTDI_RESET_SUCCESS || event == EV_RASPBEE_RESET_SUCCESS)
    {
        gcf->substate = ST_Void;

        if (gcf->task == T_RESET)
        {
            PL_ShutDown();
        }
        else if (gcf->task == T_PROGRAM)
        {
            gcf->state = ST_Program;
            gcf->state(gcf, EV_RESET_SUCCESS);
        }
    }
    else if (event == EV_UART_RESET_FAILED)
    {
        if (gcf->devType == DEV_CONBEE_1)
        {
            gcf->substate = ST_ResetFtdi;
            gcf->substate(gcf, EV_ACTION);
        }
        else if (gcf->devType == DEV_RASPBEE_1 || gcf->devType == DEV_RASPBEE_2)
        {
            gcf->substate = ST_ResetRaspBee;
            gcf->substate(gcf, EV_ACTION);
        }
        else
        {
            /* pretent it worked and jump to bootloader detection */
            PL_SetTimeout(500); /* for connect bootloader */
            gcf->state(gcf, EV_UART_RESET_SUCCESS);
        }
    }
    else if (event == EV_FTDI_RESET_FAILED)
    {
        /* pretent it worked and jump to bootloader detection */
        PL_SetTimeout(1); /* for connect bootloader */
        gcf->state(gcf, EV_FTDI_RESET_SUCCESS);
    }
    else if (event == EV_RASPBEE_RESET_FAILED)
    {
        /* pretent it worked and jump to bootloader detection */
        PL_SetTimeout(1); /* for connect bootloader */
        gcf->state(gcf, EV_RASPBEE_RESET_SUCCESS);
    }
    else
    {
        gcf->substate(gcf, event);
    }
}

static void ST_ResetUart(GCF *gcf, Event event)
{
    if (event == EV_ACTION)
    {
        PL_SetTimeout(3000);

        if (PL_Connect(gcf->devpath) == GCF_SUCCESS)
        {
            gcfCommandQueryFirmwareVersion();
            gcfCommandResetUart();
        }
    }
    else if (event == EV_DISCONNECTED)
    {
        PL_ClearTimeout();
        PL_SetTimeout(500); /* for connect bootloader */
        gcf->state(gcf, EV_UART_RESET_SUCCESS);
    }
    else if (event == EV_PKG_UART_RESET)
    {
        PL_Printf(DBG_INFO, "command reset done\n");
    }
    else if (event == EV_TIMEOUT)
    {
        PL_Printf(DBG_INFO, "command reset timeout\n");
        gcf->substate = ST_Void;
        PL_Disconnect();
        gcf->state(gcf, EV_UART_RESET_FAILED);
    }
}

/*! FTDI reset applies only to ConBee I */
static void ST_ResetFtdi(GCF *gcf, Event event)
{
    if (event == EV_ACTION)
    {
        if (PL_ResetFTDI(0) == 0)
        {
            PL_Printf(DBG_DEBUG, "FTDI reset done\n");
            PL_SetTimeout(1);
            gcf->state(gcf, EV_FTDI_RESET_SUCCESS);
        }
        else
        {
            PL_Printf(DBG_INFO, "FTDI reset failed\n");
            gcf->state(gcf, EV_FTDI_RESET_FAILED);
        }
    }
}

/*! RaspBee reset applies only to RaspBee I & II */
static void ST_ResetRaspBee(GCF *gcf, Event event)
{
    if (event == EV_ACTION)
    {
        if (PL_ResetRaspBee() == 0)
        {
            PL_Printf(DBG_DEBUG, "RaspBee reset done\n");
            PL_SetTimeout(1);
            gcf->state(gcf, EV_RASPBEE_RESET_SUCCESS);
        }
        else
        {
            PL_Printf(DBG_INFO, "RaspBee reset failed\n");
            gcf->state(gcf, EV_RASPBEE_RESET_FAILED);
        }
    }
}

static void gcfGetDevices(GCF *gcf)
{
    int n = PL_GetDevices(&gcf->devices[0], MAX_DEVICES);
    gcf->devCount = n > 0 ? (uint8_t)n : 0;
}

static void ST_ListDevices(GCF *gcf, Event event)
{
    if (event == EV_ACTION)
    {
        gcfGetDevices(gcf);

        PL_Printf(DBG_INFO, "%d devices found\n", gcf->devCount);

        for (unsigned i = 0; i < gcf->devCount; i++)
        {
            Device *dev = &gcf->devices[i];
            PL_Printf(DBG_DEBUG, "DEV [%u]: name: %s (%s),path: %s --> %s\n", i, dev->name, dev->serial, dev->path, dev->stablepath);
        }

        PL_ShutDown();
    }
}

static void ST_Program(GCF *gcf, Event event)
{
    if (event == EV_ACTION)
    {
        PL_Printf(DBG_DEBUG, "flash firmware\n");
        gcf->state = ST_Reset;
        gcf->state(gcf, event);
    }
    else if (event == EV_RESET_SUCCESS)
    {
        gcf->state = ST_BootloaderConnect;
    }
    else if (event == EV_RESET_FAILED)
    {
        PL_ShutDown();
    }
}

static void ST_BootloaderConnect(GCF *gcf, Event event)
{
    if (event == EV_TIMEOUT)
    {
        if (PL_Connect(gcf->devpath) == GCF_SUCCESS)
        {
            gcf->state = ST_BootloaderQuery;
            gcf->state(gcf, EV_ACTION);
        }
        else
        {
            // todo retry, a couple of times and revert to gcfRetry()
            PL_SetTimeout(500);
            PL_Printf(DBG_DEBUG, "retry connect bootloader %s\n", gcf->devpath);
        }
    }
}

static void ST_BootloaderQuery(GCF *gcf, Event event)
{
    if (event == EV_ACTION)
    {
        gcf->retry = 0;
        gcf->wp = 0;
        gcf->ascii[0] = '\0';
        memset(gcf->ascii, 0, sizeof(gcf->ascii));

        /* 1) wait for ConBee I and RaspBee I, which send ID on their own */
        PL_SetTimeout(200);
    }
    else if (event == EV_TIMEOUT)
    {
        if (++gcf->retry == 3)
        {
            PL_Printf(DBG_DEBUG, "query bootloader failed\n");
            gcfRetry(gcf);
        }
        else
        {
            /* 2) V1 Bootloader of ConBee II
                  Query the id here, after initial timeout. This also
                  catches cases where no firmware was installed.
            */
            PL_Printf(DBG_DEBUG, "query bootloader id\n");

            uint8_t buf[2] = { 'I', 'D' };

            PROT_Write(buf, sizeof(buf));
            PL_SetTimeout(200);
        }
    }
    else if (event == EV_RX_ASCII)
    {
        if (gcf->wp > 52 && gcf->ascii[gcf->wp - 1] == '\n' && strstr(gcf->ascii, "Bootloader"))
        {
            PL_ClearTimeout();
            PL_Printf(DBG_DEBUG, "bootloader detected (%u)\n", gcf->wp);

            gcf->state = ST_V1ProgramSync;
            gcf->state(gcf, EV_ACTION);
        }
    }
    else if (event == EV_RX_BTL_PKG_DATA)
    {

        if ((uint8_t)gcf->ascii[1] == BTL_ID_RESPONSE)
        {
            uint32_t btlVersion;
            uint32_t appCrc;

            get_u32_le((uint8_t*)&gcf->ascii[2], &btlVersion);
            get_u32_le((uint8_t*)&gcf->ascii[6], &appCrc);

            PL_Printf(DBG_DEBUG, "bootloader version 0x%08X, app crc 0x%08X\n", btlVersion, appCrc);

            gcf->state = ST_V3ProgramSync;
            gcf->state(gcf, EV_ACTION);
        }
    }
    else if (event == EV_DISCONNECTED)
    {
        gcfRetry(gcf);
    }
}

static void ST_V1ProgramSync(GCF *gcf, Event event)
{
    if (event == EV_ACTION)
    {
        gcf->wp = 0;
        gcf->ascii[0] = '\0';

        uint8_t buf[4] = { 0x1A, 0x1C, 0xA9, 0xAE };

        PROT_Write(buf, sizeof(buf));

        PL_SetTimeout(500);
    }
    else if (event == EV_RX_ASCII)
    {
        if (gcf->wp > 4 && strstr(gcf->ascii, "READY"))
        {
            PL_ClearTimeout();
            PL_Printf(DBG_DEBUG, "bootloader syned: %s\n", gcf->ascii);
            gcf->state = ST_V1ProgramWriteHeader;
            gcf->state(gcf, EV_ACTION);
        }
        else
        {
            PL_SetTimeout(10);
        }
    }
    else if (event == EV_TIMEOUT)
    {
        PL_Printf(DBG_DEBUG, "failed to sync bootloader (%u) %s\n", gcf->wp, gcf->ascii);
        gcfRetry(gcf);
    }
}

static void ST_V1ProgramWriteHeader(GCF *gcf, Event event)
{
    if (event == EV_ACTION)
    {
        gcf->wp = 0;
        gcf->ascii[0] = '\0';

        uint8_t buf[10];

        uint8_t *p = buf;
        p = put_u32_le(p, &gcf->file.gcfFileSize);
        p = put_u32_le(p, &gcf->file.gcfTargetAddress);
        *p++ = gcf->file.gcfFileType;
        *p++ = gcf->file.gcfCrc;

        gcf->state = ST_V1ProgramUpload;

        PROT_Write(buf, sizeof(buf));

        PL_SetTimeout(1000);
    }
}

static void ST_V1ProgramUpload(GCF *gcf, Event event)
{
    if (event == EV_RX_ASCII)
    {
        /* Firmware GET requests (6 bytes)
           "GET" U16 page ";"
        */
        if (gcf->wp < 6 || gcf->ascii[0] != 'G' || gcf->ascii[5] != ';')
        {
            return;
        }

        uint32_t pageNumber;
        pageNumber = gcf->ascii[4];
        pageNumber <<= 8;
        pageNumber |= gcf->ascii[3] & 0xFF;

        uint8_t *page = &gcf->file.fcontent[GCF_HEADER_SIZE] + pageNumber * V1_PAGESIZE;
        uint8_t *end = &gcf->file.fcontent[GCF_HEADER_SIZE + gcf->file.gcfFileSize];

        Assert(page < end);
        if (page >= end)
        {
            gcfRetry(gcf);
        }

        size_t remaining = (end - page);
        uint16_t size = remaining > V1_PAGESIZE ? V1_PAGESIZE : remaining;

        if (pageNumber % 20 == 0 || remaining < V1_PAGESIZE)
        {
            PL_Printf(DBG_DEBUG, "GET 0x%04X (page %u)\n", pageNumber, pageNumber);
        }

        gcf->wp = 0;
        gcf->ascii[0] = '\0';

        PROT_Write(page, size);

        if ((remaining - size) == 0)
        {
            gcf->state = ST_V1ProgramValidate;
            PL_Printf(DBG_DEBUG, "done, wait validation...\n");
            PL_SetTimeout(25600);
        }
        else
        {
            PL_SetTimeout(2000);
        }
    }
    else if (event == EV_TIMEOUT)
    {
        gcfRetry(gcf);
    }
}

static void ST_V1ProgramValidate(GCF *gcf, Event event)
{
    if (event == EV_RX_ASCII)
    {
        PL_Printf(DBG_DEBUG, "VLD %s (%u)\n", gcf->ascii, gcf->wp);

        if (gcf->wp > 6 && strstr(gcf->ascii, "#VALID CRC"))
        {
            PL_Printf(DBG_DEBUG, FMT_GREEN "firmware successful written\n" FMT_RESET, gcf->ascii);
            PL_ShutDown();
        }
        else
        {
            PL_SetTimeout(1000);
        }

    }
    else if (event == EV_TIMEOUT)
    {
        gcfRetry(gcf);
    }
}

static void ST_V3ProgramSync(GCF *gcf, Event event)
{
    if (event == EV_ACTION)
    {
        PL_MSleep(50);
        PL_SetTimeout(1000);

        uint8_t cmd[] = {
            BTL_MAGIC,
            BTL_FW_UPDATE_REQUEST,
            0x00, 0x0C, 0x00, 0x00, /* data size */
            0x00, 0x00, 0x00, 0x00, /* target address */
            0x00,                   /* file type */
            0xAA, 0xAA, 0xAA, 0xAA  /* crc32 todo */
        };

        uint8_t *p = &cmd[2];

        p = put_u32_le(p, &gcf->file.gcfFileSize);
        p = put_u32_le(p, &gcf->file.gcfTargetAddress);
        p = put_u8_le(p, &gcf->file.gcfFileType);
        (void)p;

        PROT_SendFlagged(cmd, sizeof(cmd));
    }
    else if (event == EV_RX_BTL_PKG_DATA)
    {
        if ((uint8_t)gcf->ascii[1] == BTL_FW_UPDATE_RESPONSE)
        {
            if (gcf->ascii[2] == 0x00) /* success */
            {
                PL_SetTimeout(1000);
                gcf->state = ST_V3ProgramUpload;
            }
        }
    }
    else if (event == EV_TIMEOUT)
    {
        gcfRetry(gcf);
    }
}

static void ST_V3ProgramUpload(GCF *gcf, Event event)
{
    if (event == EV_RX_BTL_PKG_DATA)
    {
        if (gcf->ascii[1] == BTL_FW_DATA_REQUEST && gcf->wp == 8)
        {
            PL_SetTimeout(5000);

            uint32_t offset;
            uint16_t length;

            get_u32_le((uint8_t*)&gcf->ascii[2], &offset);
            get_u16_le((uint8_t*)&gcf->ascii[6], &length);


            PL_Printf(DBG_DEBUG, "BTL data request, offset: 0x%08X, length: %u\n", offset, length);


            uint8_t *buf = (uint8_t*)&gcf->ascii[0];
            uint8_t *p = buf;

            *p++ = BTL_MAGIC;
            *p++ = BTL_FW_DATA_RESPONSE;

            uint8_t status = 0; // success
            uint32_t remaining = 0;

            if ((offset + length) > gcf->file.gcfFileSize)
            {
                status = 1; /* error */
            }
            else if (length > (sizeof(gcf->ascii) - 32))
            {
                status = 2; /* error */
            }
            else if (length == 0)
            {
                status = 3; /* error */
            }
            else
            {
                Assert(gcf->file.gcfFileSize > offset);
                remaining = gcf->file.gcfFileSize - offset;
                length = length < remaining ? length : (uint16_t)remaining;
                Assert(length > 0);
            }

            p = put_u8_le(p, &status);
            p = put_u32_le(p, &offset);
            p = put_u16_le(p, &length);

            if (status == 0)
            {
                Assert(length > 0);
                memcpy(p, &gcf->file.fcontent[GCF_HEADER_SIZE + offset], length);
                p += length;
            }
            else
            {
                PL_Printf(DBG_DEBUG, "failed to handle data request, status: %u\n", status);
            }

            Assert(p > buf);
            Assert(p < buf + sizeof(gcf->ascii));

            PROT_SendFlagged(buf, p - buf);
        }
    }
    else if (event == EV_TIMEOUT)
    {
        gcfRetry(gcf);
    }
}

static void ST_Connect(GCF *gcf, Event event)
{
    if (event == EV_ACTION)
    {
        if (PL_Connect(gcf->devpath) == GCF_SUCCESS)
        {
            gcf->state = ST_Connected;
            PL_SetTimeout(1000);
        }
        else
        {
            gcf->state = ST_Init;
            PL_Printf(DBG_DEBUG, "failed to connect\n");
            PL_SetTimeout(10000);
        }
    }
}

static void ST_Connected(GCF *gcf, Event event)
{
    if (event == EV_TIMEOUT)
    {
        gcfCommandQueryStatus();
        PL_SetTimeout(10000);
    }
    else if (event == EV_DISCONNECTED)
    {
        PL_ClearTimeout();
        gcf->state = ST_Init;
        PL_Printf(DBG_DEBUG, "disconnected\n");
        PL_SetTimeout(1000);
    }
}

GCF *GCF_Init(int argc, char *argv[])
{
    Assert(gcfInstance == NULL);

    GCF *gcf;
    gcf = (GCF*)PL_Malloc(sizeof(GCF));

    if (gcf)
    {
        gcfInstance = gcf;
        memset(&gcf->rxstate, 0, sizeof(gcf->rxstate));
        gcf->startTime = PL_Time();
        gcf->maxTime = 0;
        gcf->devCount = 0;
        gcf->task = T_NONE;
        gcf->state = ST_Init;
        gcf->substate = ST_Void;
        gcf->argc = argc;
        gcf->argv = argv;
        gcf->wp = 0;
        gcf->ascii[0] = '\0';
    }

    return gcf;
}

void GCF_Exit(GCF *gcf)
{
    Assert(gcf != NULL);
    Assert(gcfInstance != NULL);

    if (gcf)
    {
        PL_Free(gcf);
        gcfInstance = NULL;
    }
}

void GCF_HandleEvent(GCF *gcf, Event event)
{
    gcf->state(gcf, event);
}

int GCF_ParseFile(GCF_File *file)
{
    if (file->fsize < 14)
    {
        return -1;
    }

    Assert(file->fname[0] != '\0');

    {
        const char *version = strstr(file->fname, "0x");
        if (!version)
        {
            return -2;
        }

        file->fwVersion = strtoul(version, NULL, 16);
        Assert(file->fwVersion != 0);
    }

    /* process GCF header (14-bytes, little-endian)

       U32 magic hex: CA FE FE ED
       U8  file type
       U32 target address
       U32 file size
       U8  checksum (Dallas CRC-8)
    */

    const uint8_t *p = file->fcontent;

    uint32_t magic;
    p = get_u32_le(p, &magic);
    p = get_u8_le(p, &file->gcfFileType);
    p = get_u32_le(p, &file->gcfTargetAddress);
    p = get_u32_le(p, &file->gcfFileSize);
    get_u8_le(p, &file->gcfCrc);

    if (magic != GCF_MAGIC)
    {
        return -2;
    }

    if (file->gcfFileSize != (file->fsize - GCF_HEADER_SIZE))
    {
        return -3;
    }

    return 0;
}


void GCF_Received(GCF *gcf, const uint8_t *data, int len)
{
    Assert(len > 0);

    if (gcf->state == ST_BootloaderQuery ||
        gcf->state == ST_V1ProgramSync ||
        gcf->state == ST_V1ProgramWriteHeader ||
        gcf->state == ST_V1ProgramUpload ||
        gcf->state == ST_V1ProgramValidate)
    {
        for (int i = 0; i < len; i++)
        {
            if (gcf->wp < sizeof(gcf->ascii) - 2)
            {
                gcf->ascii[gcf->wp++] = data[i];
                gcf->ascii[gcf->wp] = '\0';
            }
            else
            {
                /* sanity rollback */
                gcf->wp = 0;
                gcf->ascii[gcf->wp] = '\0';
                PL_Printf(DBG_DEBUG, "data buffer full\n");
            }
        }

        gcf->state(gcf, EV_RX_ASCII);
    }
#ifndef NDEBUG
    else
    {
        char *p = &gcf->ascii[0];
        for (int i = 0; i < len; i++, p += 2)
        {
            put_hex(data[i], p);
        }
        *p = '\0';

        PL_Printf(DBG_INFO, FMT_GREEN "recv:" FMT_RESET " %d bytes, %s\n", len, gcf->ascii);
    }
#endif

    PROT_ReceiveFlagged(&gcf->rxstate, data, len);
}

void PROT_Packet(const uint8_t *data, uint16_t len)
{
    Assert(len > 0);

    GCF *gcf = gcfInstance;

    if (data[0] != BTL_MAGIC)
    {
        char *p = &gcf->ascii[0];
        for (int i = 0; i < len; i++, p += 2)
        {
            put_hex(data[i], p);
        }
        *p = '\0';
        PL_Printf(DBG_DEBUG, "packet: %d bytes, %s\n", len, gcf->ascii);
    }

    if (data[0] == 0x0B && len >= 8) /* write parameter response */
    {
        switch (data[7])
        {
            case 0x26: /* param: watchdog timeout */
            {
                gcf->state(gcfInstance, EV_PKG_UART_RESET);
            } break;

            default:
                break;
        }
    }
    else if (data[0] == BTL_MAGIC)
    {
        if (len < sizeof(gcf->ascii))
        {
            memcpy(&gcf->ascii[0], data, len);
            gcf->wp = len;
            gcf->state(gcfInstance, EV_RX_BTL_PKG_DATA);
        }
    }

}

static DeviceType gcfGetDeviceType(const char *devPath)
{
    Assert(devPath);

    if (devPath && devPath[0] != '\0')
    {
        if (strstr(devPath, "ttyACM")) { return DEV_CONBEE_2; }

        if (strstr(devPath, "ConBee_II")) { return DEV_CONBEE_2; }

        if (strstr(devPath, "cu.usbmodemDE")) { return DEV_CONBEE_2; }

        if (strstr(devPath, "ttyUSB")) { return DEV_CONBEE_1; }

        if (strstr(devPath, "usb-FTDI")) { return DEV_CONBEE_1; }

        if (strstr(devPath, "cu.usbserial")) { return DEV_CONBEE_1; }

        if (strstr(devPath, "ttyAMA")) { return DEV_RASPBEE_1; }

        if (strstr(devPath, "ttyS")) { return DEV_RASPBEE_1; }

        if (strstr(devPath, "/serial")) { return DEV_RASPBEE_1; }
    }

    return DEV_UNKNOWN;
}


static void gcfRetry(GCF *gcf)
{
    uint64_t now = PL_Time();
    if (gcf->maxTime > now)
    {
        PL_Printf(DBG_DEBUG, "retry: %d seconds left\n", (int)(gcf->maxTime - now) / 1000);

        gcf->state = ST_Init;
        gcf->substate = ST_Void;
        PL_SetTimeout(250);
    }
    else
    {
        PL_ShutDown();
    }
}

static void gcfPrintHelp()
{
    const char *usage =

    "GCFFlasher " APP_VERSION " copyright dresden elektronik ingenieurtechnik gmbh\n"
    "usage: GCFFlasher <options>\n"
    "options:\n"
    " -r              force device reset without programming\n"
    " -f <firmware>   flash firmware file\n"
    " -d <device>     device number or path to use, e.g. 0, /dev/ttyUSB0 or RaspBee\n"
    " -c              connect and debug serial protocol\n"
//    " -s <serial>     serial number to use\n"
    " -t <timeout>    retry until timeout (seconds) is reached\n"
    " -l              list devices\n"
//    " -x <loglevel>   debug log level 0, 1, 3\n"
    " -h -?           print this help\n";


    PL_Printf(DBG_INFO, "%s\n", usage);
}

static GCF_Status gcfProcessCommandline(GCF *gcf)
{
    GCF_Status ret = GCF_FAILED;

    gcf->state = ST_Void;
    gcf->substate = ST_Void;
    gcf->devpath[0] = '\0';
    gcf->devType = DEV_UNKNOWN;
    gcf->file.fname[0] = '\0';
    gcf->file.fsize = 0;
    gcf->task = T_NONE;

    if (gcf->argc == 1)
    {
        gcf->task = T_HELP;
    }

    for (int i = 1; i < gcf->argc; i++)
    {
        const char *arg = gcf->argv[i];

        if (arg[0] == '-')
        {
            switch (arg[1])
            {
                case 'r':
                {
                    gcf->task = T_RESET;
                } break;

                case 'c':
                {
                    gcf->task = T_CONNECT;
                } break;

                case 'd':
                {
                    if ((i + 1) == gcf->argc || gcf->argv[i + 1][0] == '-')
                    {
                        PL_Printf(DBG_INFO, "missing argument for parameter -d\n");
                        return GCF_FAILED;
                    }

                    i++;
                    arg = gcf->argv[i];

                    size_t arglen = strlen(arg);
                    if (arglen >= sizeof(gcf->devpath))
                    {
                        PL_Printf(DBG_INFO, "invalid argument, %s, for parameter -d\n", arg);
                        return GCF_FAILED;
                    }

                    memcpy(gcf->devpath, arg, arglen + 1);
                    gcf->devType = gcfGetDeviceType(gcf->devpath);
                } break;

                case 'f':
                {
                    gcf->task = T_PROGRAM;

                    if ((i + 1) == gcf->argc || gcf->argv[i + 1][0] == '-')
                    {
                        PL_Printf(DBG_INFO, "missing argument for parameter -f\n");
                        return GCF_FAILED;
                    }

                    i++;
                    arg = gcf->argv[i];

                    size_t arglen = strlen(arg);
                    if (arglen >= sizeof(gcf->file.fname))
                    {
                        PL_Printf(DBG_INFO, "invalid argument, %s, for parameter -f\n", arg);
                        return GCF_FAILED;
                    }

                    memcpy(gcf->file.fname, arg, arglen + 1);
                    int nread = PL_ReadFile(gcf->file.fname, gcf->file.fcontent, sizeof(gcf->file.fcontent));
                    if (nread <= 0)
                    {
                        PL_Printf(DBG_INFO, "failed to read file: %s\n", gcf->file.fname);
                        return GCF_FAILED;
                    }

                    PL_Printf(DBG_INFO, "read file success: %s (%d bytes)\n", gcf->file.fname, nread);
                    gcf->file.fsize = (size_t)nread;

                    if (GCF_ParseFile(&gcf->file) != 0)
                    {
                        PL_Printf(DBG_INFO, "invalid file: %s\n", gcf->file.fname);
                        return GCF_FAILED;
                    }
                } break;

                case 'l':
                {
                    gcf->task = T_LIST;
                    gcf->state = ST_ListDevices;
                    ret = GCF_SUCCESS;
                } break;

                case 't':
                {
                    if ((i + 1) == gcf->argc || gcf->argv[i + 1][0] == '-')
                    {
                        PL_Printf(DBG_INFO, "missing argument for parameter -t\n");
                        return GCF_FAILED;
                    }

                    i++;
                    arg = gcf->argv[i];

                    gcf->maxTime = strtoul(arg, NULL, 10); /* seconds */
                    if (gcf->maxTime > 3600)
                    {
                        PL_Printf(DBG_INFO, "invalid argument, %s, for parameter -t\n", arg);
                        return GCF_FAILED;
                    }

                    gcf->maxTime *= 1000;
                    gcf->maxTime += gcf->startTime;

                } break;

                case '?':
                case 'h':
                {
                    gcf->task = T_HELP;
                    ret = GCF_SUCCESS;
                } break;

                default: {
                    PL_Printf(DBG_INFO, "unknown option: %s\n", arg);
                    ret = GCF_FAILED;
                    return ret;
                } break;
            }
        }
    }

    if (gcf->task == T_PROGRAM)
    {
        if (gcf->devpath[0] == '\0')
        {
            PL_Printf(DBG_INFO, "missing -d argument\n");
            return GCF_FAILED;
        }

        if (gcf->file.fname[0] == '\0')
        {
            PL_Printf(DBG_INFO, "missing -f argument\n");
            return GCF_FAILED;
        }

        /* if no -t parameter was specified, use 10 seconds retry time */
        if (gcf->maxTime < gcf->startTime)
        {
            gcf->maxTime = 10 * 1000;
            gcf->maxTime += gcf->startTime;
        }

        /* The /dev/ttyACM0 and similar doesn't tell if this is RaspBee II,
           the fwVersion of the file is more specific.
        */
        if (gcf->devType == DEV_RASPBEE_1 &&
            (gcf->file.fwVersion & FW_VERSION_PLATFORM_MASK) == FW_VERSION_PLATFORM_R21)
        {
            PL_Printf(DBG_DEBUG, "assume RaspBee II\n");
            gcf->devType = DEV_RASPBEE_2;
        }

        gcf->state = ST_Program;
        ret = GCF_SUCCESS;
    }
    else if (gcf->task == T_CONNECT)
    {
        if (gcf->devpath[0] == '\0')
        {
            PL_Printf(DBG_INFO, "missing -d argument\n");
            return GCF_FAILED;
        }

        gcf->state = ST_Connect;
        ret = GCF_SUCCESS;
    }
    else if (gcf->task == T_RESET)
    {
        if (gcf->devpath[0] == '\0')
        {
            PL_Printf(DBG_INFO, "missing -d argument\n");
            return GCF_FAILED;
        }

        gcf->state = ST_Reset;
        ret = GCF_SUCCESS;
    }
    else if (gcf->task == T_HELP)
    {
        gcfPrintHelp();
        PL_ShutDown();
        ret = GCF_SUCCESS;
    }

    return ret;
}

static void gcfCommandResetUart()
{
    const uint8_t cmd[] = {
        0x0B, // command: write parmater
        0x03, // seq
        0x00, // status
        0x0C, 0x00, // frame length (12)
        0x05, 0x00, // buffer length (5)
        0x26, // param: watchdog timout (2 seconds)
        0x02, 0x00, 0x00, 0x00
    };

    PL_Printf(DBG_DEBUG, "send uart reset\n");

    PROT_SendFlagged(cmd, sizeof(cmd));
}

static void gcfCommandQueryStatus()
{
    const uint8_t cmd[] = {
        0x07, // command: write parmater
        0x02, // seq
        0x00, // status
        0x08, 0x00, // frame length (12)
        0x00, 0x00, 0x00 // dummy bytes
    };

    PROT_SendFlagged(cmd, sizeof(cmd));
}

static void gcfCommandQueryFirmwareVersion()
{
    const uint8_t cmd[] = {
        0x0D, // command: write parmater
        0x05, // seq
        0x00, // status
        0x09, 0x00, // frame length (9)
        0x00, 0x00, 0x00, 0x00 // dummy bytes
    };

    PROT_SendFlagged(cmd, sizeof(cmd));
}
