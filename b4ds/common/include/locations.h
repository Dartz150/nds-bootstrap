#ifndef LOCATIONS_H
#define LOCATIONS_H

#define EXCEPTION_STACK_LOCATION 0x23EFFFC

#define ROM_FILE_LOCATION 0x2715000
#define SAV_FILE_LOCATION 0x2715020

#define LOAD_CRT0_LOCATION 0x06860000 // LCDC_BANK_C

#define IMAGES_LOCATION    0x02350000

#define CARDENGINE_ARM7_LOCATION_BUFFERED  0x023EF000
#define CARDENGINE_ARM9_LOCATION_BUFFERED1 0x023E0000
#define CARDENGINE_ARM9_LOCATION_BUFFERED2 0x023E5000

#define CARDENGINE_ARM7_LOCATION           0x023FE800
#define CARDENGINE_ARM9_LOCATION_DLDI_8KB  0x023DC000
#define CARDENGINE_ARM9_LOCATION_DLDI_12KB 0x023DB000

#define CARDENGINE_SHARED_ADDRESS 0x027FFB0C

#define RESET_PARAM      0x27FFC20
#define RESET_PARAM_SDK5 0x2FFFC20

//#define TEMP_MEM 0x02FFE000 // __DSiHeader

#define NDS_HEADER         0x027FFE00
#define NDS_HEADER_SDK5    0x02FFFE00 // __NDSHeader
#define NDS_HEADER_POKEMON 0x027FF000

#define ARM9_START_ADDRESS_LOCATION      (NDS_HEADER + 0x1F4) //0x027FFFF4
#define ARM9_START_ADDRESS_SDK5_LOCATION (NDS_HEADER_SDK5 + 0x1F4) //0x02FFFFF4

#endif // LOCATIONS_H
