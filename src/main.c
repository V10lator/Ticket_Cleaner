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

#include <coreinit/mcp.h>
#include <coreinit/memdefaultheap.h>
#include <coreinit/memory.h>
#include <coreinit/filesystem_fsa.h>
#include <mocha/mocha.h>

#include <sysapp/launch.h>
#include <whb/proc.h>
#include <whb/log.h>
#include <whb/log_udp.h>

#include <stdio.h>

#include <list.h>
#include <ticket.h>

#include <string.h>

#define TICKET_BUCKET "/vol/slc/sys/rights/ticket/apps/"
#define FS_ALIGN(x)   ((x + 0x3F) & ~(0x3F))
#define isDLC(tid)    (((uint32_t)(tid >> 32)) == 0x0005000C)

typedef struct
{
    uint8_t *start;
    size_t size;
} TICKET_SECTION;

static FSAClientHandle fsaClient;
static int mcpHandle;

static bool readFile(const char *path, void **buffer, size_t size)
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
                return true;
            }

            MEMFreeToDefaultHeap(*buffer);
        }

        FSACloseFile(fsaClient, handle);
    }

    *buffer = NULL;
    return false;
}

static void deleteTickets()
{
    LIST *handledIds = createList();
    if(handledIds == NULL)
    {
        WHBLogPrint("EOM!");
        return;
    }

    size_t deletedTickets = 0;
    LIST *ticketList = createList();
    if(ticketList != NULL)
    {
        char *path = MEMAllocFromDefaultHeapEx(FS_ALIGN(FS_MAX_PATH), 0x40);
        OSBlockMove(path, TICKET_BUCKET, strlen(TICKET_BUCKET) + 1, false);

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
            bool emgBrk = false;
            void *tmpBuffer;
            size_t tmpBufSize = 0;
            MCPTitleListType titleEntry __attribute__((__aligned__(0x40)));
            // Loop through all the folder inside of the ticket bucket
            while(!emgBrk && FSAReadDir(fsaClient, dir, &entry) == FS_ERROR_OK)
            {
                if(entry.name[0] == '.' || !(entry.info.flags & FS_STAT_DIRECTORY) || strlen(entry.name) != 4)
                    continue;

                strcpy(inSentence, entry.name);
                ret = FSAOpenDir(fsaClient, path, &dir2);
                if(ret == FS_ERROR_OK)
                {
                    strcat(inSentence, "/");
                    fileName = inSentence + strlen(inSentence);
                    // Loop through all the subfolders
                    while(!emgBrk && FSAReadDir(fsaClient, dir2, &entry) == FS_ERROR_OK)
                    {
                        if(entry.name[0] == '.' || (entry.info.flags & FS_STAT_DIRECTORY) || strlen(entry.name) != 12)
                            continue;

                        strcpy(fileName, entry.name);

                        if(readFile(path, &file, entry.info.size))
                        {
                            ticket = (TICKET *)file;
                            fileEnd = ((uint8_t *)file) + entry.info.size;
                            modified = false;
                            // Loop through all the tickets inside of a file
                            while(!emgBrk)
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
                                    uint64_t *tid = MEMAllocFromDefaultHeap(sizeof(uint64_t));
                                    if(!tid)
                                    {
                                        emgBrk = true;
                                        break;
                                    }

                                    *tid = ticket->tid;
                                    addToListEnd(handledIds, tid);
                                    sec = MEMAllocFromDefaultHeap(sizeof(TICKET_SECTION));
                                    if(!sec)
                                    {
                                        WHBLogPrint("EOF!");
                                        emgBrk = true;
                                        break;
                                    }

                                    sec->start = (uint8_t *)ticket;
                                    sec->size = ptr - sec->start;

                                    if(!addToListEnd(ticketList, sec))
                                    {
                                        WHBLogPrint("EOF!");
                                        MEMFreeToDefaultHeap(sec);
                                        emgBrk = true;
                                        break;
                                    }
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
                                    WHBLogPrint("Filesize missmatch!");
                                    emgBrk = true;
                                    break;
                                }

                                ticket = (TICKET *)ptr;
                            }

                            // In case there was a matching ticket inside of the file either delete or recreate it with the remembered tickets only
                            if(modified)
                            {
                                if(getListSize(ticketList) == 0)
                                    FSARemove(fsaClient, path);
                                else
                                {
                                    FSAFileHandle fh;
                                    if(FSAOpenFileEx(fsaClient, path, "w", 0x660, FS_OPEN_FLAG_NONE, 0, &fh) != FS_ERROR_OK)
                                    {
                                        WHBLogPrintf("Error opening %s", path);
                                        emgBrk == true;
                                        break;
                                    }

                                    forEachListEntry(ticketList, sec)
                                    {
                                        if(sec->size > tmpBufSize)
                                        {
                                            if(tmpBufSize != 0)
                                                MEMFreeToDefaultHeap(tmpBuffer);

                                            tmpBufSize = sec->size;
                                            tmpBuffer = MEMAllocFromDefaultHeapEx(FS_ALIGN(tmpBufSize), 0x40);
                                            if(tmpBuffer == NULL)
                                            {
                                                WHBLogPrint("EOF!");
                                                FSACloseFile(fsaClient, fh);
                                                emgBrk = true;
                                                break;
                                            }
                                        }

                                        OSBlockMove(tmpBuffer, sec->start, sec->size, false);
                                        FSAWriteFile(fsaClient, tmpBuffer, 1, sec->size, fh, 0);
                                    }

                                    FSACloseFile(fsaClient, fh);
                                }
                            }

                            clearList(ticketList, true);
                            MEMFreeToDefaultHeap(file);
                        }
                        else
                            WHBLogPrintf("Error reading %s", path);
                    }

                    FSACloseDir(fsaClient, dir2);
                }
                else
                    WHBLogPrintf("Error opening %s", path);
            }

            FSACloseDir(fsaClient, dir);
        }
        else
            WHBLogPrintf("Error opening %s", path);

        destroyList(ticketList, true);
    }
    else
        WHBLogPrint("EOM!");

    destroyList(handledIds, true);
    WHBLogPrintf("%u tickets deleted!", deletedTickets);
}

int main(void)
{
    FSAInit();
    fsaClient = FSAAddClient(NULL);
    WHBLogUdpInit();
    if(fsaClient)
    {
        if(Mocha_InitLibrary() == MOCHA_RESULT_SUCCESS)
        {
            Mocha_UnlockFSClientEx(fsaClient);
            if(FSAMount(fsaClient, "/dev/slc01", "/vol/slc", FSA_MOUNT_FLAG_LOCAL_MOUNT, NULL, 0) == FS_ERROR_OK)
            {
                mcpHandle = MCP_Open();
                if(mcpHandle != 0)
                {
                    WHBLogPrint("Deleting tickets, this might take some time");
                    deleteTickets();
                    MCP_Close(mcpHandle);
                }
                else
                    WHBLogPrint("Error opening MCP!");

                FSAUnmount(fsaClient, "/vol/slc", FSA_UNMOUNT_FLAG_NONE);
            }
            else
                WHBLogPrint("Error mounting SLC!");

            Mocha_DeInitLibrary();
        }
        else
            WHBLogPrint("Libmocha error!");

        FSADelClient(fsaClient);
    }
    else
        WHBLogPrint("No FSA client!");

    FSAShutdown();

    WHBProcInit();
    SYSLaunchMenu();
    while(WHBProcIsRunning()) {}
    return 0;
}
