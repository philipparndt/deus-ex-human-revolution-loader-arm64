// CoreServices legacy APIs: Gestalt, Memory Manager, queues, File Manager
// (FSRef + emulated FSSpec), fixed point, timing, Component Manager, AppleEvents.
#include <CoreServices/CoreServices.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

// Not in the modern SDK headers.
extern Boolean OTAtomicSetBit(UInt8 *bytePtr, UInt32 bitNumber);
extern long SetCurrentA5(void);
extern OSErr NMInstall(void *nmReqPtr);
extern OSErr NMRemove(void *nmReqPtr);

_Static_assert(sizeof(FSCatalogInfo) == 144, "i386 FSCatalogInfo");
_Static_assert(offsetof(FSCatalogInfo, dataLogicalSize) == 104, "i386 FSCatalogInfo.dataLogicalSize");
_Static_assert(sizeof(FSVolumeInfo) == 126, "i386 FSVolumeInfo");
_Static_assert(sizeof(FSSpec) == 70 && offsetof(FSSpec, name) == 6, "i386 FSSpec");
_Static_assert(sizeof(QHdr) == 10 && offsetof(QHdr, qTail) == 6, "i386 QHdr");
_Static_assert(sizeof(AEDesc) == 8, "i386 AEDesc");

static int failures;
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                 \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

struct myq {
    QElemPtr qLink;
    short qType;
    int value;
};

static void test_gestalt(void) {
    SInt32 v = 0, ma = 0, mi = 0, bf = 0, ram = 0;
    CHECK(Gestalt(gestaltSystemVersion, &v) == noErr);
    Gestalt(gestaltSystemVersionMajor, &ma);
    Gestalt(gestaltSystemVersionMinor, &mi);
    Gestalt(gestaltSystemVersionBugFix, &bf);
    CHECK(Gestalt(gestaltPhysicalRAMSizeInMegabytes, &ram) == noErr && ram > 0);
    printf("Gestalt: sysv=%#x %d.%d.%d ram=%d MB\n", (int)v, (int)ma, (int)mi, (int)bf, (int)ram);
    CHECK(ma >= 10 && v >= 0x1000);
    CHECK(Gestalt('zzzz', &v) == gestaltUndefSelectorErr);
}

static void test_memory(void) {
    char *p = NewPtrClear(100);
    CHECK(p && MemError() == noErr);
    CHECK(p[0] == 0 && p[99] == 0);
    strcpy(p, "hello");
    DisposePtr(p);
    CHECK(MemError() == noErr);

    QHdr q = {0};
    struct myq a = {0, 0, 1}, b = {0, 0, 2}, c = {0, 0, 3};
    Enqueue((QElemPtr)&a, &q);
    Enqueue((QElemPtr)&b, &q);
    Enqueue((QElemPtr)&c, &q);
    CHECK(q.qHead == (QElemPtr)&a && q.qTail == (QElemPtr)&c);
    CHECK(Dequeue((QElemPtr)&b, &q) == noErr);
    CHECK(a.qLink == (QElemPtr)&c);
    CHECK(Dequeue((QElemPtr)&c, &q) == noErr && q.qTail == (QElemPtr)&a);
    CHECK(Dequeue((QElemPtr)&c, &q) == qErr);

    unsigned char bits[2] = {0, 0};
    BitSet(bits, 9);
    CHECK(bits[1] == 0x40 && BitTst(bits, 9) && !BitTst(bits, 8));
    BitClr(bits, 9);
    CHECK(bits[1] == 0);
    UInt8 ot = 0;
    CHECK(!OTAtomicSetBit(&ot, 3) && ot == 8 && OTAtomicSetBit(&ot, 3));
    CHECK(SetCurrentA5() == 0);
    char nm[36] = {0};
    CHECK(NMInstall(nm) == noErr && NMRemove(nm) == noErr);
}

static void test_fixed_time(void) {
    CHECK(Fix2Long(0x00018000) == 2);
    CHECK(Fix2Long(0x00014000) == 1);
    CHECK(X2Fix(1.5) == 0x00018000);
    CHECK(Fix2X(0x00028000) == 2.5);
    UnsignedWide u1, u2;
    Microseconds(&u1);
    AbsoluteTime t0 = UpTime();
    Delay(3, NULL); // 50 ms
    Microseconds(&u2);
    AbsoluteTime t1 = UpTime();
    unsigned long long d = (((unsigned long long)u2.hi << 32) | u2.lo) - (((unsigned long long)u1.hi << 32) | u1.lo);
    Nanoseconds n0 = AbsoluteToNanoseconds(t0), n1 = AbsoluteToNanoseconds(t1);
    unsigned long long dn = UnsignedWideToUInt64(n1) - UnsignedWideToUInt64(n0);
    printf("Delay(3): %llu us, %llu ns\n", d, dn);
    CHECK(d >= 45000 && d < 1000000);
    CHECK(dn >= 45000000ull && dn < 1000000000ull);
    CFAbsoluteTime at = 0;
    CHECK(UCConvertSecondsToCFAbsoluteTime(3061152000u, &at) == noErr); // 2001-01-01 since 1904
    printf("UCConvertSecondsToCFAbsoluteTime(2001-01-01) = %.0f\n", at);
    CHECK(at > -86400.0 && at < 86400.0); // seconds are local time
}

static void test_files(void) {
    FSRef ref;
    char path[1024];
    Boolean isDir = 0;
    CHECK(FSPathMakeRef((const UInt8 *)"/tmp", &ref, &isDir) == noErr && isDir);
    CHECK(FSRefMakePath(&ref, (UInt8 *)path, sizeof path) == noErr);
    printf("FSRefMakePath(/tmp) = %s\n", path);
    FSCatalogInfo ci;
    CHECK(FSGetCatalogInfo(&ref, kFSCatInfoNodeFlags | kFSCatInfoVolume | kFSCatInfoNodeID, &ci, NULL, NULL, NULL) ==
          noErr);
    CHECK(ci.nodeFlags & kFSNodeIsDirectoryMask);

    FSRef prefs;
    CHECK(FSFindFolder(kUserDomain, kPreferencesFolderType, kDontCreateFolder, &prefs) == noErr);
    FSRefMakePath(&prefs, (UInt8 *)path, sizeof path);
    printf("FSFindFolder(prefs) = %s\n", path);
    CHECK(strstr(path, "Library/Preferences") != NULL);

    FSVolumeInfo vi;
    HFSUniStr255 vname;
    FSVolumeRefNum actual = 0;
    CHECK(FSGetVolumeInfo(kFSInvalidVolumeRefNum, 1, &actual, kFSVolInfoSizes, &vi, &vname, NULL) == noErr);
    printf("volume 1: ref %d, %llu MB total, name len %d\n", actual, vi.totalBytes >> 20, vname.length);
    CHECK(vi.totalBytes > 0);

    // FSSpec emulation: FindFolder + FSMakeFSSpec + FSp* + FSRead/FSWrite.
    short vref = 0;
    SInt32 dirID = 0;
    CHECK(FindFolder(kUserDomain, kTemporaryFolderType, kCreateFolder, &vref, &dirID) == noErr);
    FSSpec spec;
    OSErr e = FSMakeFSSpec(vref, dirID, "\pdxh_cs_test.dat", &spec);
    CHECK(e == fnfErr || e == noErr);
    if (e == noErr) FSpDelete(&spec);
    CHECK(FSpCreate(&spec, 'DXHT', 'TEST', smSystemScript) == noErr);
    CHECK(FSpCreate(&spec, 'DXHT', 'TEST', smSystemScript) == dupFNErr);
    FInfo fi;
    CHECK(FSpGetFInfo(&spec, &fi) == noErr && fi.fdType == 'TEST' && fi.fdCreator == 'DXHT');
    short rn = 0;
    CHECK(FSpOpenDF(&spec, fsRdWrPerm, &rn) == noErr);
    long count = 11;
    CHECK(FSWrite(rn, &count, "hello world") == noErr && count == 11);
    long eof = 0;
    CHECK(GetEOF(rn, &eof) == noErr && eof == 11);
    CHECK(SetFPos(rn, fsFromStart, 6) == noErr);
    char buf[16] = {0};
    count = 10;
    CHECK(FSRead(rn, &count, buf) == eofErr && count == 5 && memcmp(buf, "world", 5) == 0);
    CHECK(FSClose(rn) == noErr);
    CHECK(FSpDelete(&spec) == noErr);
    CHECK(FSMakeFSSpec(vref, dirID, "\pdxh_cs_test.dat", &spec) == fnfErr);

    // Fork API on a host file
    FILE *f = fopen("/tmp/dxh_cs_fork.txt", "w");
    if (f) {
        fputs("forkdata", f);
        fclose(f);
    }
    FSRef fref;
    CHECK(FSPathMakeRef((const UInt8 *)"/tmp/dxh_cs_fork.txt", &fref, NULL) == noErr);
    HFSUniStr255 dfork;
    FSGetDataForkName(&dfork);
    FSIORefNum fork = 0;
    CHECK(FSOpenFork(&fref, dfork.length, dfork.unicode, fsRdPerm, &fork) == noErr);
    ByteCount got = 0;
    memset(buf, 0, sizeof buf);
    CHECK(FSReadFork(fork, fsFromStart, 4, 10, buf, &got) == eofErr && got == 4 && memcmp(buf, "data", 4) == 0);
    CHECK(FSCloseFork(fork) == noErr);
    CHECK(FSGetCatalogInfo(&fref, kFSCatInfoDataSizes, &ci, NULL, NULL, NULL) == noErr && ci.dataLogicalSize == 8);
    remove("/tmp/dxh_cs_fork.txt");
}

static void test_component(void) {
    ComponentDescription d = {'auou', 'def ', 'appl', 0,
                              0};
    Component c = FindNextComponent(NULL, &d);
    printf("FindNextComponent(default output) = %p\n", (void *)c);
    CHECK(c != NULL);
    ComponentInstance ci = NULL;
    CHECK(OpenAComponent(c, &ci) == noErr && ci != NULL);
    CHECK(CloseComponent(ci) == noErr);
}

static void test_ae(void) {
    AEDesc d = {typeNull, NULL};
    CHECK(AECreateDesc(typeUTF8Text, "abc", 3, &d) == noErr && d.descriptorType == typeUTF8Text);
    CHECK(AEGetDescDataSize(&d) == 3);
    char buf[8] = {0};
    CHECK(AEGetDescData(&d, buf, sizeof buf) == noErr && strcmp(buf, "abc") == 0);
    AppleEvent ev = {typeNull, NULL};
    ProcessSerialNumber psn = {0, 2 /* kCurrentProcess */};
    AEBuildError berr;
    CHECK(AEBuildAppleEvent(kCoreEventClass, kAEOpenDocuments, typeProcessSerialNumber, &psn, sizeof psn,
                            kAutoGenerateReturnID, kAnyTransactionID, &ev, &berr, "'----':@, 'abcd':'TEXT'(@)", &d,
                            "hello", (Size)5) == noErr);
    char out[16] = {0};
    DescType t = 0;
    Size sz = 0;
    CHECK(AEGetParamPtr(&ev, 'abcd', typeChar, &t, out, sizeof out, &sz) == noErr && sz == 5 &&
          strcmp(out, "hello") == 0);
    AEDesc p = {typeNull, NULL};
    CHECK(AEGetParamDesc(&ev, keyDirectObject, typeWildCard, &p) == noErr && AEGetDescDataSize(&p) == 3);
    CHECK(AEPutParamPtr(&ev, 'num ', typeSInt32, &(SInt32){42}, 4) == noErr);
    SInt32 num = 0;
    CHECK(AEGetParamPtr(&ev, 'num ', typeSInt32, NULL, &num, 4, NULL) == noErr && num == 42);
    AEDisposeDesc(&p);
    AEDisposeDesc(&ev);
    AEDisposeDesc(&d);
    CHECK(d.dataHandle == NULL);
}

static void test_collection(void) {
    Collection c = NewCollection();
    CHECK(c != NULL);
    SInt32 v = 1234;
    CHECK(AddCollectionItem(c, 'test', 1, sizeof v, &v) == noErr);
    SInt32 sz = sizeof v, out = 0;
    CHECK(GetTaggedCollectionItem(c, 'test', 1, &sz, &out) == noErr && out == 1234);
    DisposeCollection(c);
}

int main(void) {
    test_gestalt();
    test_memory();
    test_fixed_time();
    test_files();
    test_component();
    test_ae();
    test_collection();
    printf("cf_coreservices: %s (%d failures)\n", failures ? "FAILED" : "OK", failures);
    return failures != 0;
}
