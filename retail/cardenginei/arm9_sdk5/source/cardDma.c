/*
    NitroHax -- Cheat tool for the Nintendo DS
    Copyright (C) 2008  Michael "Chishm" Chisholm

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <string.h>
#include <nds/ndstypes.h>
#include <nds/arm9/exceptions.h>
#include <nds/arm9/cache.h>
#include <nds/system.h>
#include <nds/dma.h>
#include <nds/interrupts.h>
#include <nds/ipc.h>
#include <nds/timers.h>
#include <nds/fifomessages.h>
#include <nds/memory.h> // tNDSHeader
#include "module_params.h"
#include "ndma.h"
#include "tonccpy.h"
#include "hex.h"
#include "igm_text.h"
#include "nds_header.h"
#include "cardengine.h"
#include "locations.h"
#include "cardengine_header_arm9.h"
#include "unpatched_funcs.h"

#define cacheFlushFlag BIT(7)
#define cardReadFix BIT(8)
#define cacheDisabled BIT(9)

//#ifdef DLDI
#include "my_fat.h"
#include "card.h"
//#endif

extern cardengineArm9* volatile ce9;

extern vu32* volatile sharedAddr;

extern tNDSHeader* ndsHeader;
extern aFile* romFile;

extern int cacheDescriptor[];
extern int cacheCounter[];
extern int accessCounter;

bool isDma = false;

void endCardReadDma() {
    if(ce9->patches->cardEndReadDmaRef) {
        volatile void (*cardEndReadDmaRef)() = ce9->patches->cardEndReadDmaRef;
        (*cardEndReadDmaRef)();
    } else if(ce9->thumbPatches->cardEndReadDmaRef) {
        callEndReadDmaThumb();
    }    
}

#ifndef DLDI
#ifdef ASYNCPF
static u32 asyncSector = 0;
//static u32 asyncQueue[5];
//static int aQHead = 0;
//static int aQTail = 0;
//static int aQSize = 0;
#endif

bool dmaOn = true;
bool dmaReadOnArm7 = false;
bool dmaReadOnArm9 = false;

extern int allocateCacheSlot(void);
extern int getSlotForSector(u32 sector);
//extern int getSlotForSectorManual(int i, u32 sector);
extern vu8* getCacheAddress(int slot);
extern void updateDescriptor(int slot, u32 sector);

/*#ifdef ASYNCPF
void addToAsyncQueue(sector) {
	asyncQueue[aQHead] = sector;
	aQHead++;
	aQSize++;
	if(aQHead>4) {
		aQHead=0;
	}
	if(aQSize>5) {
		aQSize=5;
		aQTail++;
		if(aQTail>4) aQTail=0;
	}
}

u32 popFromAsyncQueueHead() {	
	if(aQSize>0) {
	
		aQHead--;
		if(aQHead == -1) aQHead = 4;
		aQSize--;
		
		return asyncQueue[aQHead];
	} else return 0;
}
#endif*/

static void waitForArm7(bool ipc) {
	extern void sleepMs(int ms);

	if (!ipc) {
		IPC_SendSync(0x4);
	}
	while (sharedAddr[3] != (vu32)0) {
		if (ipc) {
			IPC_SendSync(0x4);
			sleepMs(1);
		}
	}
}

#ifdef ASYNCPF
void triggerAsyncPrefetch(sector) {	
	if(asyncSector == 0) {
		int slot = getSlotForSector(sector);
		// read max 32k via the WRAM cache
		// do it only if there is no async command ongoing
		if(slot==-1) {
			//addToAsyncQueue(sector);
			// send a command to the arm7 to fill the main RAM cache
			u32 commandRead = (isDma ? 0x020FF80A : 0x020FF808);

			slot = allocateCacheSlot();

			vu8* buffer = getCacheAddress(slot);

			cacheDescriptor[slot] = sector;
			cacheCounter[slot] = 0x0FFFFFFF; // async marker
			asyncSector = sector;		

			// write the command
			sharedAddr[0] = buffer;
			sharedAddr[1] = ce9->cacheBlockSize;
			sharedAddr[2] = sector;
			sharedAddr[3] = commandRead;

			IPC_SendSync(0x4);

			// do it asynchronously
			/*waitForArm7();*/
		}	
	}	
}

void processAsyncCommand() {
	if(asyncSector != 0) {
		int slot = getSlotForSector(asyncSector);
		if(slot!=-1 && cacheCounter[slot] == 0x0FFFFFFF) {
			// get back the data from arm7
			if(sharedAddr[3] == (vu32)0) {
				updateDescriptor(slot, asyncSector);
				asyncSector = 0;
			}			
		}	
	}
}

void getAsyncSector() {
	if(asyncSector != 0) {
		int slot = getSlotForSector(asyncSector);
		if(slot!=-1 && cacheCounter[slot] == 0x0FFFFFFF) {
			// get back the data from arm7
			waitForArm7(true);

			updateDescriptor(slot, asyncSector);
			asyncSector = 0;
		}	
	}	
}
#endif

static inline bool checkArm7(void) {
    IPC_SendSync(0x4);
	return (sharedAddr[3] == (vu32)0);
}

extern bool IPC_SYNC_hooked;
extern void hookIPC_SYNC(void);
extern void enableIPC_SYNC(void);

static u32 * dmaParams = NULL;
static int currentLen = 0;
//static int currentSlot = 0;

void continueCardReadDmaArm9() {
    if(dmaReadOnArm9) {
		if (ndmaBusy(0)) {
			IPC_SendSync(0x3);
			return;
		}
        dmaReadOnArm9 = false;

		u32 commandRead=0x025FFB0A;

		u32 src = dmaParams[3];
		u8* dst = (u8*)dmaParams[4];
		u32 len = dmaParams[5];

		// Update cardi common
		dmaParams[3] = src + currentLen;
		dmaParams[4] = (vu32)(dst + currentLen);
		dmaParams[5] = len - currentLen;

		src = dmaParams[3];
		dst = (u8*)dmaParams[4];
		len = dmaParams[5];

		int sector = (src/ce9->cacheBlockSize);
		u32 sectorLoc = sector*ce9->cacheBlockSize;

		#ifdef ASYNCPF
		processAsyncCommand();
		#endif

        if (len > 0) {
			accessCounter++;  

            // Read via the main RAM cache
        	//int slot = getSlotForSectorManual(currentSlot+1, sector);
        	int slot = getSlotForSector(sector);
        	vu8* buffer = getCacheAddress(slot);
			#ifdef ASYNCPF
			u32 nextSector = sector+ce9->cacheBlockSize;
			#endif
        	// Read max CACHE_READ_SIZE via the main RAM cache
        	if (slot == -1) {
				#ifdef ASYNCPF
				getAsyncSector();
				#endif

        		// Send a command to the ARM7 to fill the RAM cache
        		slot = allocateCacheSlot();

        		buffer = getCacheAddress(slot);

				//fileRead((char*)buffer, *romFile, sector, ce9->cacheBlockSize, 0);

				/*u32 len2 = (src - sector) + len;
				u16 readLen = ce9->cacheBlockSize;
				if (len2 > ce9->cacheBlockSize*3 && slot+3 < ce9->cacheSlots) {
					readLen = ce9->cacheBlockSize*4;
				} else if (len2 > ce9->cacheBlockSize*2 && slot+2 < ce9->cacheSlots) {
					readLen = ce9->cacheBlockSize*3;
				} else if (len2 > ce9->cacheBlockSize && slot+1 < ce9->cacheSlots) {
					readLen = ce9->cacheBlockSize*2;
				}*/

				// Write the command
				sharedAddr[0] = (vu32)buffer;
				sharedAddr[1] = ce9->cacheBlockSize;
				sharedAddr[2] = sectorLoc;
				sharedAddr[3] = commandRead;

				dmaReadOnArm7 = true;

				IPC_SendSync(0x4);

				updateDescriptor(slot, sector);
				/*if (readLen >= ce9->cacheBlockSize*2) {
					updateDescriptor(slot+1, sector+ce9->cacheBlockSize);
				}
				if (readLen >= ce9->cacheBlockSize*3) {
					updateDescriptor(slot+2, sector+(ce9->cacheBlockSize*2));
				}
				if (readLen >= ce9->cacheBlockSize*4) {
					updateDescriptor(slot+3, sector+(ce9->cacheBlockSize*3));
				}
				currentSlot = slot;*/
                return;
        	}
			#ifdef ASYNCPF
			if(cacheCounter[slot] == 0x0FFFFFFF) {
				// prefetch successfull
				getAsyncSector();

				triggerAsyncPrefetch(nextSector);
			} else {
				int i;
				for(i=0; i<5; i++) {
					if(asyncQueue[i]==sector) {
						// prefetch successfull
						triggerAsyncPrefetch(nextSector);
						break;
					}
				}
			}
			#endif
        	updateDescriptor(slot, sector);	

        	u32 len2 = len;
        	if ((src - sectorLoc) + len2 > ce9->cacheBlockSize) {
        		len2 = sectorLoc - src + ce9->cacheBlockSize;
        	}

        	/*if (len2 > 512) {
        		len2 -= src % 4;
        		len2 -= len2 % 32;
        	}*/

			// Copy via dma
			ndmaCopyWordsAsynch(0, (u8*)buffer+(src-sectorLoc), dst, len2);
			dmaReadOnArm9 = true;
			currentLen = len2;
			//currentSlot = slot;

			IPC_SendSync(0x3);
        } else {
          //disableIrqMask(IRQ_DMA0 << dma);
          //resetRequestIrqMask(IRQ_DMA0 << dma);
          //disableDMA(dma);
		  isDma = false;
          endCardReadDma();
		}
    }
}

void continueCardReadDmaArm7() {
    if(dmaReadOnArm7) {
        if(!checkArm7()) return;
        dmaReadOnArm7 = false;

        vu32* volatile cardStruct = ce9->cardStruct0;

		u32 src = dmaParams[3];
		u8* dst = (u8*)dmaParams[4];
		u32 len = dmaParams[5];

		/*if (ce9->valueBits & cacheDisabled) {
			endCardReadDma();
		} else {*/
			int sector = (src/ce9->cacheBlockSize);
			u32 sectorLoc = sector*ce9->cacheBlockSize;

			u32 len2 = len;
			if ((src - sectorLoc) + len2 > ce9->cacheBlockSize) {
				len2 = sectorLoc - src + ce9->cacheBlockSize;
			}

			/*if (len2 > 512) {
				len2 -= src % 4;
				len2 -= len2 % 32;
			}*/

			//vu8* buffer = getCacheAddress(currentSlot);
			vu8* buffer = getCacheAddress(getSlotForSector(sector));

			// TODO Copy via dma
			ndmaCopyWordsAsynch(0, (u8*)buffer+(src-sectorLoc), dst, len2);
			dmaReadOnArm9 = true;
			currentLen = len2;

			IPC_SendSync(0x3);
		//}
	}
}

void cardSetDma (u32 * params) {
	isDma = true;

	dmaParams = params;
	u32 src = dmaParams[3];
	u8* dst = (u8*)dmaParams[4];
	u32 len = dmaParams[5];

	if (!dmaOn) {
		cardRead(0, dst, src, len);
		endCardReadDma();
		return;
	}

    disableIrqMask(IRQ_CARD);
    disableIrqMask(IRQ_CARD_LINE);

	enableIPC_SYNC();

	u32 commandRead=0x025FFB0A;
	int sector = (src/ce9->cacheBlockSize);
	u32 sectorLoc = sector*ce9->cacheBlockSize;
	//u32 page = (src / 512) * 512;

	accessCounter++;  

	#ifdef ASYNCPF
	processAsyncCommand();
	#endif

	/*if (ce9->valueBits & cacheDisabled) {
		// Write the command
		sharedAddr[0] = (vu32)dst;
		sharedAddr[1] = len;
		sharedAddr[2] = src;
		sharedAddr[3] = commandRead;

		dmaReadOnArm7 = true;

		IPC_SendSync(0x4);
	} else {*/
		// Read via the main RAM cache
		int slot = getSlotForSector(sector);
		vu8* buffer = getCacheAddress(slot);
		#ifdef ASYNCPF
		u32 nextSector = sector+ce9->cacheBlockSize;
		#endif
		// Read max CACHE_READ_SIZE via the main RAM cache
		if (slot == -1) {    
			#ifdef ASYNCPF
			getAsyncSector();
			#endif

			// Send a command to the ARM7 to fill the RAM cache
			slot = allocateCacheSlot();

			buffer = getCacheAddress(slot);

			//fileRead((char*)buffer, *romFile, sector, ce9->cacheBlockSize, 0);

			/*u32 len2 = (src - sector) + len;
			u16 readLen = ce9->cacheBlockSize;
			if (len2 > ce9->cacheBlockSize*3 && slot+3 < ce9->cacheSlots) {
				readLen = ce9->cacheBlockSize*4;
			} else if (len2 > ce9->cacheBlockSize*2 && slot+2 < ce9->cacheSlots) {
				readLen = ce9->cacheBlockSize*3;
			} else if (len2 > ce9->cacheBlockSize && slot+1 < ce9->cacheSlots) {
				readLen = ce9->cacheBlockSize*2;
			}*/

			// Write the command
			sharedAddr[0] = (vu32)buffer;
			sharedAddr[1] = ce9->cacheBlockSize;
			sharedAddr[2] = sectorLoc;
			sharedAddr[3] = commandRead;

			dmaReadOnArm7 = true;

			IPC_SendSync(0x4);

			updateDescriptor(slot, sector);
			/*if (readLen >= ce9->cacheBlockSize*2) {
				updateDescriptor(slot+1, sector+ce9->cacheBlockSize);
			}
			if (readLen >= ce9->cacheBlockSize*3) {
				updateDescriptor(slot+2, sector+(ce9->cacheBlockSize*2));
			}
			if (readLen >= ce9->cacheBlockSize*4) {
				updateDescriptor(slot+3, sector+(ce9->cacheBlockSize*3));
			}
			currentSlot = slot;*/
			return;
		} 
		#ifdef ASYNCPF
		if(cacheCounter[slot] == 0x0FFFFFFF) {
			// prefetch successfull
			getAsyncSector();

			triggerAsyncPrefetch(nextSector);
		} else {
			int i;
			for(i=0; i<5; i++) {
				if(asyncQueue[i]==sector) {
					// prefetch successfull
					triggerAsyncPrefetch(nextSector);
					break;
				}
			}
		}
		#endif
		updateDescriptor(slot, sector);	

		u32 len2 = len;
		if ((src - sectorLoc) + len2 > ce9->cacheBlockSize) {
			len2 = sectorLoc - src + ce9->cacheBlockSize;
		}

		/*if (len2 > 512) {
			len2 -= src % 4;
			len2 -= len2 % 32;
		}*/

		// Copy via dma
		ndmaCopyWordsAsynch(0, (u8*)buffer+(src-sectorLoc), dst, len2);
		dmaReadOnArm9 = true;
		currentLen = len2;
		//currentSlot = slot;

		IPC_SendSync(0x3);
	//}
}
#else
void cardSetDma (u32 * params) {
	u32 src = params[3];
	u8* dst = (u8*)params[4];
	u32 len = params[5];

	cardRead(0, dst, src, len);
	endCardReadDma();
}
#endif

extern bool isNotTcm(u32 address, u32 len);

u32 cardReadDma(u32 dma, u8* dst, u32 src, u32 len) {
	if(dma >= 0 
        && dma <= 3 
        //&& func != NULL
        && len > 0
        && !(((int)dst) & 3)
        && isNotTcm(dst, len)
        // check 512 bytes page alignement 
        && !(((int)len) & 511)
        && !(((int)src) & 511)
	) {
		isDma = true;
		if (ce9->patches->cardEndReadDmaRef || ce9->thumbPatches->cardEndReadDmaRef) {
			cacheFlush();
			return true;
		} /*else {
			dma=4;
            clearIcache();
		}*/
    } /*else {
        dma=4;
        clearIcache();
    }*/

    return false;
}
