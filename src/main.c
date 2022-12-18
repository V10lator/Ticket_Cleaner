/***************************************************************************
 * This file is part of Ticket Cleaner.                                    *
 * Copyright (c) 2022 V10lator <v10lator@myway.de>                         *
 *                                                                         *
 * This program is free software; you can redistribute it and/or modify    *
 * it under the terms of the GNU General Public License as published by    *
 * the Free Software Foundation; either version 3 of the License, or       *
 * (at your option) any later version.                                     *
 *                                                                         *
 * This program is distributed in the hope that it will be useful,         *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of          *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the           *
 * GNU General Public License for more details.                            *
 *                                                                         *
 * You should have received a copy of the GNU General Public License along *
 * with this program; if not, If not, see <http://www.gnu.org/licenses/>.  *
 ***************************************************************************/

#include <list.h>
#include <ticket.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <coreinit/filesystem_fsa.h>
#include <coreinit/foreground.h>
#include <coreinit/mcp.h>
#include <coreinit/memdefaultheap.h>
#include <coreinit/memory.h>
#include <coreinit/title.h>
#include <mocha/mocha.h>
#include <proc_ui/procui.h>
#include <sysapp/launch.h>
#include <vpad/input.h>
#include <whb/log.h>
#include <whb/log_console.h>

#define COLOR_BACKGROUND 0x000033FF
#define COLOR_RED        0x990000FF

#define TICKET_BUCKET    "/vol/slc/sys/rights/ticket/apps/"
#define SD_PATH          "/vol/external01/wiiu/tickets"
#define TICKET_LIST_PATH "/vol/slc/sys/rights/sys/title.list"
#define FS_ALIGN(x)      ((x + 0x3F) & ~(0x3F))
#define isDLC(tid)       (((uint32_t)(tid >> 32)) == 0x0005000C)
#define WRITE_BUFSIZE    (1024 * 1024) // 1 MB
#define MAX_LINES        16

typedef struct
{
    uint8_t *start;
    size_t size;
} TICKET_SECTION;

typedef struct TITLE_LIST_ENTRY TITLE_LIST_ENTRY;
struct TITLE_LIST_ENTRY
{
    uint64_t tid;
    TITLE_LIST_ENTRY *next;
};

typedef enum
{
    LOOP_STATE_MAIN_MENU,
    LOOP_STATE_DELETING,
    LOOP_STATE_DELETED,
    LOOP_STATE_BACKING_UP,
    LOOP_STATE_BACKUPED,
    LOOP_STATE_INVALID,
} LOOP_STATE;

static FSAClientHandle fsaClient;
static int mcpHandle;

static FSAFileHandle fileHandle;
static uint8_t *writeBuffer;
static size_t writeBufferFill = 0;

static size_t arg0;
static size_t arg1;
static bool error = false;

static void clearScreen()
{
    for(int i = 0; i < MAX_LINES; ++i)
        WHBLogPrint("");
}

static bool isSystemTitle(uint64_t tid)
{
    uint32_t th = (uint32_t)(tid >> 32);
    switch(th)
    {
        case 0x00050010:
        case 0x0005001B:
        case 0x00050030:
            return true;
    }

    return false;
}

static FSError readFile(const char *path, void **buffer, size_t size)
{
    FSAFileHandle handle;
    FSError err = FSAOpenFileEx(fsaClient, path, "r", 0x000, 0, 0, &handle);
    if(err == FS_ERROR_OK)
    {
        *buffer = MEMAllocFromDefaultHeapEx(FS_ALIGN(size), 0x40);
        if(*buffer != NULL)
        {
            err = FSAReadFile(fsaClient, *buffer, size, 1, handle, 0);
            if(err == 1)
            {
                FSACloseFile(fsaClient, handle);
                return FS_ERROR_OK;
            }

            MEMFreeToDefaultHeap(*buffer);
        }
        else
            err = FS_ERROR_OUT_OF_RESOURCES;

        FSACloseFile(fsaClient, handle);
    }

    return err;
}

static FSError writeTicket(const uint8_t *buffer, size_t size)
{
    size_t newBufSize = writeBufferFill + size;
    if(newBufSize < WRITE_BUFSIZE)
    {
        OSBlockMove(writeBuffer + writeBufferFill, buffer, size, false);
        writeBufferFill = newBufSize;
        return FS_ERROR_OK;
    }

    newBufSize -= WRITE_BUFSIZE;
    OSBlockMove(writeBuffer + writeBufferFill, buffer, size - newBufSize, false);
    FSError ret = FSAWriteFile(fsaClient, writeBuffer, WRITE_BUFSIZE, 1, fileHandle, 0);
    if(ret != 1)
    {
        FSACloseFile(fsaClient, fileHandle);
        return ret;
    }

    writeBufferFill = 0;
    return newBufSize != 0 ? writeTicket(buffer + (size - newBufSize), newBufSize) : FS_ERROR_OK;
}

static FSError closeTicket()
{
    if(writeBufferFill != 0)
    {
        FSError ret = FSAWriteFile(fsaClient, writeBuffer, writeBufferFill, 1, fileHandle, 0);
        if(ret != 1)
        {
            FSACloseFile(fsaClient, fileHandle);
            return ret;
        }

        writeBufferFill = 0;
    }

    return FSACloseFile(fsaClient, fileHandle);
}

static void cleanTitleList()
{
    char path[FS_ALIGN(FS_MAX_PATH)] __attribute__((__aligned__(0x40))) = TICKET_LIST_PATH;
    FSStat stat;
    FSError ret = FSAGetStat(fsaClient, path, &stat);
    if(ret != FS_ERROR_OK)
    {
        WHBLogPrintf("Error stating %s", path);
        WHBLogPrint(FSAGetStatusStr(ret));
        error = true;
        return;
    }

    uint64_t *file;
    ret = readFile(path, (void **)&file, stat.size);
    if(ret != FS_ERROR_OK)
    {
        WHBLogPrintf("Error reading %s", path);
        WHBLogPrint(FSAGetStatusStr(ret));
        error = true;
        return;
    }

    TITLE_LIST_ENTRY *titleList = NULL;
    TITLE_LIST_ENTRY *cur = NULL;
    TITLE_LIST_ENTRY *last = NULL;
    arg1 = 0;
    MCPTitleListType titleEntry __attribute__((__aligned__(0x40)));
    for(size_t i = 0; i < stat.size / sizeof(uint64_t); ++i)
    {
        if(isSystemTitle(file[i]) || MCP_GetTitleInfo(mcpHandle, file[i], &titleEntry) == 0)
        {
            cur = MEMAllocFromDefaultHeap(sizeof(TITLE_LIST_ENTRY));
            if(cur == NULL)
            {
                WHBLogPrint("EOM!");
                error = true;
                break;
            }

            cur->tid = file[i];
            cur->next = NULL;

            if(last == NULL)
                titleList = cur;
            else
            {
                last->next = cur;
                last = cur;
            }

            last = cur;
        }
        else
            ++arg1;
    }

    MEMFreeToDefaultHeap(file);

    if(arg1 != 0)
    {
        ret = FSAOpenFileEx(fsaClient, path, "w", 0x660, FS_OPEN_FLAG_NONE, 0, &fileHandle);
        if(ret == FS_ERROR_OK)
        {
            for(TITLE_LIST_ENTRY *cur = titleList; cur != NULL; cur = cur->next)
            {
                ret = writeTicket((uint8_t *)&(cur->tid), sizeof(uint64_t));
                if(ret != FS_ERROR_OK)
                {
                    WHBLogPrintf("Error writing %s", path);
                    WHBLogPrint(FSAGetStatusStr(ret));
                    error = true;
                    break;
                }
            }

            if(!error)
            {
                ret = closeTicket();
                if(ret != FS_ERROR_OK)
                {
                    WHBLogPrintf("Error closing %s", path);
                    WHBLogPrint(FSAGetStatusStr(ret));
                    error = true;
                }
            }
        }
        else
        {
            WHBLogPrintf("Error opening %s", path);
            WHBLogPrint(FSAGetStatusStr(ret));
            error = true;
        }
    }

    for(; titleList != NULL; titleList = titleList->next)
        MEMFreeToDefaultHeap(titleList);
}

static void backupTickets()
{
    char sdPath[FS_ALIGN(FS_MAX_PATH)] __attribute__((__aligned__(0x40))) = SD_PATH;
    char *inSD = sdPath + strlen(SD_PATH);
    FSAMakeDir(fsaClient, sdPath, 0x660);
    FSADirectoryHandle dir;
    FSError ret = FSAOpenDir(fsaClient, sdPath, &dir);
    if(ret != FS_ERROR_OK)
    {
        WHBLogPrintf("Error opening %s", sdPath);
        WHBLogPrint(FSAGetStatusStr(ret));
        error = true;
        return;
    }

    // Loop through all the folders to find a free slot
    uint16_t slot = 0;
    uint16_t current;
    FSADirectoryEntry entry;
    while(!error && FSAReadDir(fsaClient, dir, &entry) == FS_ERROR_OK)
    {
        if(entry.name[0] == '.' || !(entry.info.flags & FS_STAT_DIRECTORY) || strlen(entry.name) != 4)
            continue;

        current = strtol(entry.name, NULL, 16);
        if(++current > slot)
            slot = current;
    }

    FSACloseDir(fsaClient, dir);
    sprintf(inSD, "/%04X", slot);
    ret = FSAMakeDir(fsaClient, sdPath, 0x660);
    if(ret != FS_ERROR_OK)
    {
        WHBLogPrintf("Error creating %s", sdPath);
        WHBLogPrint(FSAGetStatusStr(ret));
        error = true;
        return;
    }

    inSD += 5;
    *inSD = '/';
    ++inSD;

    char path[FS_ALIGN(FS_MAX_PATH)] __attribute__((__aligned__(0x40))) = TICKET_BUCKET;
    char *inSentence = path + strlen(TICKET_BUCKET);
    void *file;
    ret = FSAOpenDir(fsaClient, path, &dir);
    if(ret == FS_ERROR_OK)
    {
        FSADirectoryHandle dir2;
        char *fileName;
        char *inSD2 = inSD + 4;
        arg0 = 0;

        // Loop through all the folder inside of the ticket bucket
        while(!error && FSAReadDir(fsaClient, dir, &entry) == FS_ERROR_OK)
        {
            if(entry.name[0] == '.' || !(entry.info.flags & FS_STAT_DIRECTORY) || strlen(entry.name) != 4)
                continue;

            strcpy(inSentence, entry.name);
            ret = FSAOpenDir(fsaClient, path, &dir2);
            if(ret != FS_ERROR_OK)
            {
                WHBLogPrintf("Error opening %s", path);
                WHBLogPrint(FSAGetStatusStr(ret));
                error = true;
                break;
            }

            OSBlockMove(inSD, entry.name, 5, false);
            ret = FSAMakeDir(fsaClient, sdPath, 0x660);
            if(ret != FS_ERROR_OK)
            {
                WHBLogPrintf("Error creating %s", sdPath);
                WHBLogPrint(FSAGetStatusStr(ret));
                error = true;
                break;
            }

            strcat(inSentence, "/");
            fileName = inSentence + strlen(inSentence);
            // Loop through all the subfolders
            while(!error && FSAReadDir(fsaClient, dir2, &entry) == FS_ERROR_OK)
            {
                if(entry.name[0] == '.' || (entry.info.flags & FS_STAT_DIRECTORY) || strlen(entry.name) != 12)
                    continue;

                strcpy(fileName, entry.name);
                ret = readFile(path, &file, entry.info.size);
                if(ret == FS_ERROR_OK)
                {
                    *inSD2 = '/';
                    strcpy(inSD2 + 1, fileName);
                    ret = FSAOpenFileEx(fsaClient, sdPath, "w", 0x660, FS_OPEN_FLAG_NONE, 0, &fileHandle);
                    if(ret == FS_ERROR_OK)
                    {
                        ret = FSAWriteFile(fsaClient, file, entry.info.size, 1, fileHandle, 0);
                        if(ret != 1)
                        {
                            WHBLogPrintf("Error writing %s", sdPath);
                            WHBLogPrint(FSAGetStatusStr(ret));
                            error = true;
                        }
                        ret = FSACloseFile(fsaClient, fileHandle);
                        if(ret != FS_ERROR_OK)
                        {
                            WHBLogPrintf("Error closing %s", sdPath);
                            WHBLogPrint(FSAGetStatusStr(ret));
                            error = true;
                        }

                        ++arg0;
                    }
                    else
                    {
                        WHBLogPrintf("Error creating %s", sdPath);
                        WHBLogPrint(FSAGetStatusStr(ret));
                        error = true;
                    }
                }
                else
                {
                    WHBLogPrintf("Error reading %s", path);
                    WHBLogPrint(FSAGetStatusStr(ret));
                    error = true;
                }

                MEMFreeToDefaultHeap(file);
            }

            FSACloseDir(fsaClient, dir2);
        }

        FSACloseDir(fsaClient, dir);
    }
    else
    {
        WHBLogPrintf("Error opening %s", path);
        WHBLogPrint(FSAGetStatusStr(ret));
        error = true;
    }

    if(error)
        return;

    strcpy(path, TICKET_LIST_PATH);
    FSStat stat;
    ret = FSAGetStat(fsaClient, path, &stat);
    if(ret != FS_ERROR_OK)
    {
        WHBLogPrintf("Error stating %s", path);
        WHBLogPrint(FSAGetStatusStr(ret));
        error = true;
        return;
    }

    ret = readFile(path, &file, stat.size);
    if(ret != FS_ERROR_OK)
    {
        WHBLogPrintf("Error opening %s", path);
        WHBLogPrint(FSAGetStatusStr(ret));
        error = true;
    }

    strcpy(inSD, "title.list");
    ret = FSAOpenFileEx(fsaClient, sdPath, "w", 0x660, FS_OPEN_FLAG_NONE, 0, &fileHandle);
    if(ret == FS_ERROR_OK)
    {
        ret = FSAWriteFile(fsaClient, file, stat.size, 1, fileHandle, 0);
        if(ret != 1)
        {
            WHBLogPrintf("Error writing %s", sdPath);
            WHBLogPrint(FSAGetStatusStr(ret));
            error = true;
        }

        ret = FSACloseFile(fsaClient, fileHandle);
        if(ret != FS_ERROR_OK)
        {
            WHBLogPrintf("Error closing %s", sdPath);
            WHBLogPrint(FSAGetStatusStr(ret));
            error = true;
        }
    }

    MEMFreeToDefaultHeap(file);
}

static void deleteTickets()
{
    LIST *handledIds = createList();
    if(handledIds == NULL)
    {
        WHBLogPrint("EOM!");
        error = true;
        return;
    }

    LIST *ticketList = createList();
    if(ticketList != NULL)
    {
        char path[FS_ALIGN(FS_MAX_PATH)] __attribute__((__aligned__(0x40))) = TICKET_BUCKET;
        char *inSentence = path + strlen(TICKET_BUCKET);
        FSADirectoryHandle dir;
        FSError ret = FSAOpenDir(fsaClient, path, &dir);
        if(ret == FS_ERROR_OK)
        {
            FSADirectoryEntry entry;
            FSADirectoryHandle dir2;
            char *fileName;
            void *file;
            TICKET *ticket;
            TICKET_SECTION *sec;
            bool keep;
            bool modified;
            uint64_t *tid;
            uint8_t *fileEnd;
            uint8_t *ptr;
            MCPTitleListType titleEntry __attribute__((__aligned__(0x40)));
            arg0 = 0;

            // Loop through all the folder inside of the ticket bucket
            while(!error && FSAReadDir(fsaClient, dir, &entry) == FS_ERROR_OK)
            {
                if(entry.name[0] == '.' || !(entry.info.flags & FS_STAT_DIRECTORY) || strlen(entry.name) != 4)
                    continue;

                strcpy(inSentence, entry.name);
                ret = FSAOpenDir(fsaClient, path, &dir2);
                if(ret != FS_ERROR_OK)
                {
                    WHBLogPrintf("Error opening %s", path);
                    WHBLogPrint(FSAGetStatusStr(ret));
                    error = true;
                    break;
                }

                strcat(inSentence, "/");
                fileName = inSentence + strlen(inSentence);
                // Loop through all the subfolders
                while(!error && FSAReadDir(fsaClient, dir2, &entry) == FS_ERROR_OK)
                {
                    if(entry.name[0] == '.' || (entry.info.flags & FS_STAT_DIRECTORY) || strlen(entry.name) != 12)
                        continue;

                    strcpy(fileName, entry.name);
                    ret = readFile(path, &file, entry.info.size);
                    if(ret != FS_ERROR_OK)
                    {
                        WHBLogPrintf("Error reading %s", path);
                        WHBLogPrint(FSAGetStatusStr(ret));
                        error = true;
                        break;
                    }

                    ticket = (TICKET *)file;
                    fileEnd = ((uint8_t *)file) + entry.info.size;
                    modified = false;
                    // Loop through all the tickets inside of a file
                    while(!error)
                    {
                        ptr = ((uint8_t *)ticket) + sizeof(TICKET);
                        if(ticket->total_hdr_size > 0x14)
                            ptr += ticket->total_hdr_size - 0x14;

                        keep = true;
                        // Check that title is installed
                        if(MCP_GetTitleInfo(mcpHandle, ticket->tid, &titleEntry) != 0)
                            keep = false;
                        // Check for duplicated tickets (ignoring DLC tickets)
                        else if(!isDLC(ticket->tid))
                        {
                            forEachListEntry(handledIds, tid)
                            {
                                if(ticket->tid == *tid)
                                {
                                    keep = false;
                                    break;
                                }
                            }
                        }

                        if(keep)
                        {
                            if(!isDLC(ticket->tid))
                            {
                                tid = MEMAllocFromDefaultHeap(sizeof(uint64_t));
                                if(!tid)
                                {
                                    WHBLogPrint("EOM!");
                                    error = true;
                                    break;
                                }
                                if(!addToListEnd(handledIds, tid))
                                {
                                    MEMFreeToDefaultHeap(tid);
                                    WHBLogPrint("EOM!");
                                    error = true;
                                    break;
                                }

                                *tid = ticket->tid;
                            }

                            sec = MEMAllocFromDefaultHeap(sizeof(TICKET_SECTION));
                            if(!sec)
                            {
                                WHBLogPrint("EOM!");
                                error = true;
                                break;
                            }
                            if(!addToListEnd(ticketList, sec))
                            {
                                MEMFreeToDefaultHeap(sec);
                                WHBLogPrint("EOM!");
                                error = true;
                                break;
                            }

                            sec->start = (uint8_t *)ticket;
                            sec->size = ptr - sec->start;
                        }
                        else
                        {
                            ++arg0;
                            modified = true;
                        }

                        if(ptr == fileEnd)
                            break;
                        if(ptr > fileEnd)
                        {
                            WHBLogPrintf("Filesize missmatch at %s!", path);
                            error = true;
                            break;
                        }

                        ticket = (TICKET *)ptr;
                    }

                    // In case there was a matching ticket inside of the file either delete or recreate it with the remembered tickets only
                    if(!error && modified)
                    {
                        if(getListSize(ticketList) == 0)
                        {
                            ret = FSARemove(fsaClient, path);
                            if(ret != FS_ERROR_OK)
                            {
                                WHBLogPrintf("Error removing %s", path);
                                WHBLogPrint(FSAGetStatusStr(ret));
                                error = true;
                            }
                        }
                        else
                        {
                            ret = FSAOpenFileEx(fsaClient, path, "w", 0x660, FS_OPEN_FLAG_NONE, 0, &fileHandle);
                            if(ret == FS_ERROR_OK)
                            {
                                forEachListEntry(ticketList, sec)
                                {
                                    ret = writeTicket(sec->start, sec->size);
                                    if(ret != FS_ERROR_OK)
                                    {
                                        WHBLogPrintf("Error writing %s", path);
                                        WHBLogPrint(FSAGetStatusStr(ret));
                                        error = true;
                                    }
                                }

                                if(!error)
                                {
                                    ret = closeTicket();
                                    if(ret != FS_ERROR_OK)
                                    {
                                        WHBLogPrintf("Error writing %s", path);
                                        WHBLogPrint(FSAGetStatusStr(ret));
                                        error = true;
                                    }
                                }
                            }
                            else
                            {
                                WHBLogPrintf("Error opening %s", path);
                                WHBLogPrint(FSAGetStatusStr(ret));
                                error = true;
                            }
                        }
                    }

                    clearList(ticketList, true);
                    MEMFreeToDefaultHeap(file);
                }

                FSACloseDir(fsaClient, dir2);
            }

            FSACloseDir(fsaClient, dir);
        }
        else
        {
            WHBLogPrintf("Error opening %s", path);
            WHBLogPrint(FSAGetStatusStr(ret));
            error = true;
        }

        destroyList(ticketList, true);
    }
    else
    {
        WHBLogPrint("EOM!");
        error = true;
    }

    destroyList(handledIds, true);
    if(!error)
        cleanTitleList();
}

static uint32_t homeCallback(void *ctx)
{
    uint64_t tid = OSGetTitleID();
    if(tid == 0x0005000013374842 || (tid & 0xFFFFFFFFFFFFFCFF) == 0x000500101004A000) // HBL
        SYSRelaunchTitle(0, NULL);
    else
        SYSLaunchMenu();

// TODO: This causes a blackscreen in the Wii U menu
//    WHBLogConsoleFree();
    return 0;
}

static bool procLoop()
{
    switch(ProcUIProcessMessages(true))
    {
        case PROCUI_STATUS_EXITING:
            return false;
        case PROCUI_STATUS_RELEASE_FOREGROUND:
            ProcUIDrawDoneRelease();
        default:
            return true;
    }
}

int readInput()
{
    VPADReadError vError;
    VPADStatus vpad;
    VPADRead(VPAD_CHAN_0, &vpad, 1, &vError);
    if(vError == VPAD_READ_SUCCESS && vpad.trigger)
    {
        vpad.trigger &= ~(VPAD_STICK_R_EMULATION_LEFT | VPAD_STICK_R_EMULATION_RIGHT | VPAD_STICK_R_EMULATION_UP | VPAD_STICK_R_EMULATION_DOWN | VPAD_BUTTON_HOME);
        return vpad.trigger;
    }

    return 0;
}

void mainLoop()
{
    WHBLogConsoleSetColor(COLOR_BACKGROUND);

    ProcUIInit(OSSavesDone_ReadyToRelease);
    ProcUIRegisterCallback(PROCUI_CALLBACK_HOME_BUTTON_DENIED, homeCallback, NULL, 100);
    OSEnableHomeButtonMenu(false);

    LOOP_STATE state = LOOP_STATE_MAIN_MENU;
    LOOP_STATE oldState = LOOP_STATE_INVALID;
    int buttons;
    while(!error && procLoop())
    {
        if(state != oldState)
        {
            oldState = state;
            clearScreen();

            switch(state)
            {
                case LOOP_STATE_MAIN_MENU:
                    WHBLogPrint("Special thanks to: Ingunar");
                    WHBLogPrint("");
                    WHBLogPrint("");
                    WHBLogPrint("Press (A) to delete unused tickets.");
                    WHBLogPrint("Press (B) to backup all tickets.");
                    WHBLogPrint("Press (HOME) to exit.");
                    break;
                case LOOP_STATE_DELETING:
                    WHBLogPrint("Deleting tickets, this might take some time...");
                    break;
                case LOOP_STATE_DELETED:
                    WHBLogPrintf("%u tickets deleted and %u entries removed from title.list!", arg0, arg1);
                    WHBLogPrint("");
                    WHBLogPrint("Press (B) to go back.");
                    WHBLogPrint("Press (HOME) to exit.");
                    break;
                case LOOP_STATE_BACKING_UP:
                    WHBLogPrint("Creating backup, this might take some time...");
                    break;
                case LOOP_STATE_BACKUPED:
                    WHBLogPrintf("%u ticket files saved!", arg0);
                    WHBLogPrint("");
                    WHBLogPrint("Press (B) to go back.");
                    WHBLogPrint("Press (HOME) to exit.");
                    break;
                default:
                    WHBLogPrint("0xDEADCODE");
                    break;
            }

            WHBLogConsoleDraw();
        }

        buttons = readInput();
        switch(state)
        {
            case 0:
                if(buttons & VPAD_BUTTON_A)
                    state = LOOP_STATE_DELETING;
                else if(buttons & VPAD_BUTTON_B)
                    state = LOOP_STATE_BACKING_UP;
                break;
            case LOOP_STATE_DELETING:
                deleteTickets();
                state = LOOP_STATE_DELETED;
                break;
            case LOOP_STATE_BACKING_UP:
                backupTickets();
                state = LOOP_STATE_BACKUPED;
                break;
            case LOOP_STATE_DELETED:
            case LOOP_STATE_BACKUPED:
                if(buttons & VPAD_BUTTON_B)
                    state = 0;
                break;
            default:
                break;
        }
    }
}

int main()
{
    bool initted = false;
    WHBLogConsoleInit();
    writeBuffer = MEMAllocFromDefaultHeapEx(FS_ALIGN(WRITE_BUFSIZE), 0x40);
    if(writeBuffer != NULL)
    {
        FSAInit();
        fsaClient = FSAAddClient(NULL);
        if(fsaClient)
        {
            MochaUtilsStatus ret = Mocha_InitLibrary();
            if(ret == MOCHA_RESULT_SUCCESS)
            {
                ret = Mocha_UnlockFSClientEx(fsaClient);
                if(ret == MOCHA_RESULT_SUCCESS)
                {
                    FSError err = FSAMount(fsaClient, "/dev/slc01", "/vol/slc", FSA_MOUNT_FLAG_LOCAL_MOUNT, NULL, 0);
                    if(err == FS_ERROR_OK)
                    {
                        mcpHandle = MCP_Open();
                        if(mcpHandle != 0)
                        {
                            initted = true;
                            WHBLogConsoleSetColor(COLOR_BACKGROUND);
                            mainLoop();
                            MCP_Close(mcpHandle);
                        }
                        else
                        {
                            WHBLogPrint("Error opening MCP!");
                            error = true;
                        }

                        FSAUnmount(fsaClient, "/vol/slc", FSA_UNMOUNT_FLAG_NONE);
                    }
                    else
                    {
                        WHBLogPrintf("Error mounting SLC: %s!", FSAGetStatusStr(err));
                        error = true;
                    }
                }
                else
                {
                    WHBLogPrintf("Error unlocking FSAClient: -0x%04X!", -ret);
                    error = true;
                }

                Mocha_DeInitLibrary();
            }
            else
            {
                WHBLogPrintf("Libmocha error: -0x%04X!", -ret);
                error = true;
            }

            FSADelClient(fsaClient);
        }
        else
        {
            WHBLogPrint("No FSA client!");
            error = true;
        }

        FSAShutdown();
        MEMFreeToDefaultHeap(writeBuffer);
    }
    else
    {
        WHBLogPrint("EOM!");
        error = true;
    }

    if(error)
    {
        if(!initted)
        {
            ProcUIInit(OSSavesDone_ReadyToRelease);
            ProcUIRegisterCallback(PROCUI_CALLBACK_HOME_BUTTON_DENIED, homeCallback, NULL, 100);
            OSEnableHomeButtonMenu(false);
        }

        WHBLogPrint("");
        WHBLogPrint("Press HOME to exit");
        WHBLogConsoleSetColor(COLOR_RED);
        WHBLogConsoleDraw();

        while(procLoop())
            OSSleepTicks(OSMillisecondsToTicks(1000 / 60));
    }

    return 0;
}
