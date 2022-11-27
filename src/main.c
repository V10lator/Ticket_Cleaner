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

#include <coreinit/filesystem_fsa.h>
#include <coreinit/foreground.h>
#include <coreinit/mcp.h>
#include <coreinit/memdefaultheap.h>
#include <coreinit/memory.h>
#include <coreinit/title.h>
#include <mocha/mocha.h>

#include <sysapp/launch.h>
#include <proc_ui/procui.h>
#include <whb/log.h>
#include <whb/log_console.h>

#include <list.h>
#include <ticket.h>

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define COLOR_BACKGROUND 0x000033FF
#define COLOR_RED        0x990000FF

#define TICKET_BUCKET "/vol/slc/sys/rights/ticket/apps/"
#define FS_ALIGN(x)   ((x + 0x3F) & ~(0x3F))
#define isDLC(tid)    (((uint32_t)(tid >> 32)) == 0x0005000C)
#define WRITE_BUFSIZE (1024 * 1024) // 1 MB

typedef struct
{
    uint8_t *start;
    size_t size;
} TICKET_SECTION;

static FSAClientHandle fsaClient;
static int mcpHandle;

static FSAFileHandle fileHandle;
static uint8_t *writeBuffer;
static size_t writeBufferFill = 0;

static bool error = false;

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

static void deleteTickets()
{
    LIST *handledIds = createList();
    if(handledIds == NULL)
    {
        WHBLogPrint("EOM!");
        error = true;
        return;
    }

    size_t deletedTickets = 0;
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
                            ++deletedTickets;
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
                            FSARemove(fsaClient, path);
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
    WHBLogPrintf("%u tickets deleted!", deletedTickets);
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

int main(void)
{
    WHBLogConsoleInit();
    WHBLogConsoleSetColor(COLOR_BACKGROUND);
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
                            WHBLogPrint("Deleting tickets, this might take some time...");
                            WHBLogConsoleDraw();
                            deleteTickets();
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

    ProcUIInit(OSSavesDone_ReadyToRelease);
    ProcUIRegisterCallback(PROCUI_CALLBACK_HOME_BUTTON_DENIED, homeCallback, NULL, 100);
    OSEnableHomeButtonMenu(false);

    WHBLogPrint("");
    WHBLogPrint("Press HOME to exit");
    if(error)
        WHBLogConsoleSetColor(COLOR_RED);
    WHBLogConsoleDraw();

    while(procLoop()) {}
    return 0;
}
