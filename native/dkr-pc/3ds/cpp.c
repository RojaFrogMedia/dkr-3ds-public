// Circle Pad Pro on an Old 3DS: the accessory's second pad and its ZL, ZR
// and R buttons, read over infrared through ir:USER. (A New 3DS has the
// same controls built in and reports them through ir:rst, which libctru
// folds into the ordinary key state: see input.c.)
//
// The transport follows the Circle Pad Pro protocol as documented by
// rust3ds/ctru-rs (ir_user.rs) and libctru pull request 568: the service
// shares a 4 KB block that receives the accessory's 10-byte input packets,
// and polling is renewed once a second. Nothing here waits for the
// accessory, so a missing or sleeping one costs nothing.
#include <3ds.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>

static Handle sService, sBlock;
static volatile u8 *sMemory;
static bool sInitialized, sLive;
static u64 sLastPacket, sLastRequest;
static u32 sLastHeld;

static bool decode(const u8 *p, u32 *held) {
    u8 crc = 0;
    int i, b, x, y;

    *held = 0;
    if (p[0] != 0xA5 || p[2] != 6 || p[3] != 0x10) {
        return false;
    }
    for (i = 0; i < 9; i++) {
        crc ^= p[i];
        for (b = 0; b < 8; b++) {
            crc = (u8) ((crc << 1) ^ ((crc & 0x80) ? 7 : 0));
        }
    }
    if (crc != p[9]) {
        return false;
    }
    x = (p[4] | ((p[5] & 15) << 8)) - 2048;
    y = ((p[5] >> 4) | (p[6] << 4)) - 2048;
    if (x > 400) *held |= KEY_CSTICK_RIGHT;
    if (x < -400) *held |= KEY_CSTICK_LEFT;
    if (y > 400) *held |= KEY_CSTICK_UP;
    if (y < -400) *held |= KEY_CSTICK_DOWN;
    if (!(p[7] & 0x20)) *held |= KEY_ZL;
    if (!(p[7] & 0x40)) *held |= KEY_ZR;
    if (!(p[7] & 0x80)) *held |= KEY_R;
    return true;
}

static Result send_command(void) {
    Result r = svcSendSyncRequest(sService);

    return R_FAILED(r) ? r : (Result) getThreadCommandBuffer()[1];
}

static Result simple(unsigned command, bool argument, unsigned value) {
    u32 *c = getThreadCommandBuffer();

    c[0] = IPC_MakeHeader(command, argument ? 1 : 0, 0);
    c[1] = value;
    return send_command();
}

void cpp_close(void) {
    if (sInitialized) {
        simple(9, false, 0);
        simple(2, false, 0);
    }
    if (sService) svcCloseHandle(sService);
    if (sBlock) svcCloseHandle(sBlock);
    free((void *) sMemory);
    sService = sBlock = 0;
    sMemory = NULL;
    sInitialized = sLive = false;
    sLastHeld = 0;
    sLastPacket = sLastRequest = 0;
}

bool cpp_open(void) {
    u32 *c;

    cpp_close();
    if (R_FAILED(srvGetServiceHandle(&sService, "ir:USER"))) {
        sService = 0;
        return false;
    }
    sMemory = memalign(0x1000, 0x1000);
    if (sMemory == NULL) {
        cpp_close();
        return false;
    }
    memset((void *) sMemory, 0, 0x1000);
    if (R_FAILED(svcCreateMemoryBlock(&sBlock, (u32) sMemory, 0x1000, MEMPERM_READ, MEMPERM_READWRITE))) {
        sBlock = 0;
        cpp_close();
        return false;
    }
    c = getThreadCommandBuffer();
    c[0] = IPC_MakeHeader(0x18, 6, 2);      // InitializeIrNopShared
    c[1] = 0x1000; c[2] = 1024; c[3] = 16; c[4] = 1024; c[5] = 16; c[6] = 4;
    c[7] = IPC_Desc_SharedHandles(1);
    c[8] = sBlock;
    if (R_FAILED(send_command())) {
        cpp_close();
        return false;
    }
    sInitialized = true;
    if (R_FAILED(simple(6, true, 1))) {     // RequireConnection, device 1: the Circle Pad Pro
        cpp_close();
        return false;
    }
    return true;
}

static u32 word(unsigned offset) {
    return sMemory[offset] | ((u32) sMemory[offset + 1] << 8) | ((u32) sMemory[offset + 2] << 16) | ((u32) sMemory[offset + 3] << 24);
}

bool cpp_connected(void) {
    return sLive;
}

// The accessory's controls as libctru key bits (KEY_ZL, KEY_ZR, KEY_R,
// KEY_CSTICK_*); 0 while it is absent.
u32 cpp_poll(void) {
    u64 now;
    u32 start, count, i;

    if (!sInitialized) {
        return 0;
    }
    now = osGetTime();
    if (sMemory[8] != 2) {                  // not connected
        sLive = false;
        sLastHeld = 0;
        sLastPacket = 0;
        if (sMemory[8] == 0 && (sLastRequest == 0 || now - sLastRequest >= 1000)) {
            simple(6, true, 1);
            sLastRequest = now;
        }
        return 0;
    }
    if (sLastRequest == 0 || now - sLastRequest >= 1000) {
        // Ask for input reports for the next while: every 8 ms.
        static u8 request[] = { 1, 8, 40 };
        u32 *c = getThreadCommandBuffer();

        c[0] = IPC_MakeHeader(0xD, 1, 2);   // SendIrNop
        c[1] = 3;
        c[2] = IPC_Desc_StaticBuffer(3, 0);
        c[3] = (u32) request;
        if (R_FAILED(send_command())) {
            sLive = false;
            sLastHeld = 0;
            return 0;
        }
        sLastRequest = now;
    }
    __sync_synchronize();
    start = word(0x10);
    count = word(0x18);
    if (start >= 16 || count > 16) {
        sLive = false;
        sLastHeld = 0;
        simple(3, false, 0);
        return 0;
    }
    for (i = 0; i < count; i++) {
        unsigned info = 0x20 + ((start + i) % 16) * 8;
        u32 offset = word(info), length = word(info + 4);
        u8 packet[10];
        u32 held;
        int j;

        if (offset >= 896 || length != 10) {
            continue;
        }
        for (j = 0; j < 10; j++) {
            packet[j] = sMemory[0xA0 + (offset + j) % 896];
        }
        if (decode(packet, &held)) {
            sLastHeld = held;
            sLastPacket = now;
            sLive = true;
        }
    }
    if (count != 0 && R_FAILED(simple(0x19, true, count))) {    // ReleaseReceivedData
        sLive = false;
        sLastHeld = 0;
    }
    if (sLastPacket == 0 || now - sLastPacket > 250) {
        sLive = false;
        sLastHeld = 0;
    }
    return sLastHeld;
}
