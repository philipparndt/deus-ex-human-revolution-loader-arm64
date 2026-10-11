// Steam bridge: dlopen libsteam_api.dylib (served by host thunks), init against
// the running Steam client, query a few interfaces through their vtables and
// pump callbacks with a hand-made CCallbackBase (2012 SDK layout).
// Needs Steam running and logged in; run with SteamAppId=238010.
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

typedef void **iface; // C++ object: first word is the vtable
#define VFN(obj, idx, type) ((type)(*(void ***)(obj))[idx])

// CCallbackBase: vtable {Run(void*), Run(void*, bool, SteamAPICall_t), GetCallbackSizeBytes()}
struct ccallback {
    void **vtable;
    uint8_t flags;
    int icallback;
    int hits;
    int last_size;
};
static void cb_run(struct ccallback *self, void *param) {
    self->hits++;
    printf("  callback %d delivered (param %p)\n", self->icallback, param);
}
static void cb_run_result(struct ccallback *self, void *param, int io_failure, uint64_t call) {
    self->hits++;
    printf("  call result %llx delivered (io failure %d)\n", (unsigned long long)call, io_failure);
}
static int cb_size(struct ccallback *self) { return self->last_size; }
static void *cb_vtable[3] = {(void *)cb_run, (void *)cb_run_result, (void *)cb_size};

int main(void) {
    void *h = dlopen("@executable_path/../Resources/libsteam_api.dylib", RTLD_NOW);
    if (!h) h = dlopen("libsteam_api.dylib", RTLD_NOW);
    if (!h) {
        printf("FAIL: dlopen: %s\n", dlerror());
        return 1;
    }
    int (*init)(void) = (int (*)(void))dlsym(h, "SteamAPI_Init");
    int (*restart)(unsigned) = (int (*)(unsigned))dlsym(h, "SteamAPI_RestartAppIfNecessary");
    void (*run)(void) = (void (*)(void))dlsym(h, "SteamAPI_RunCallbacks");
    void (*reg)(struct ccallback *, int) = (void (*)(struct ccallback *, int))dlsym(h, "SteamAPI_RegisterCallback");
    void (*unreg)(struct ccallback *) = (void (*)(struct ccallback *))dlsym(h, "SteamAPI_UnregisterCallback");
    void (*reg_result)(struct ccallback *, uint64_t) =
        (void (*)(struct ccallback *, uint64_t))dlsym(h, "SteamAPI_RegisterCallResult");
    void (*shutdown)(void) = (void (*)(void))dlsym(h, "SteamAPI_Shutdown");
    iface (*steam_user)(void) = (iface(*)(void))dlsym(h, "SteamUser");
    iface (*steam_apps)(void) = (iface(*)(void))dlsym(h, "SteamApps");
    iface (*steam_utils)(void) = (iface(*)(void))dlsym(h, "SteamUtils");
    iface (*steam_friends)(void) = (iface(*)(void))dlsym(h, "SteamFriends");
    iface (*steam_stats)(void) = (iface(*)(void))dlsym(h, "SteamUserStats");
    iface (*steam_rs)(void) = (iface(*)(void))dlsym(h, "SteamRemoteStorage");
    if (!init || !run || !reg || !unreg || !steam_user || !steam_apps || !steam_utils || !shutdown) {
        printf("FAIL: dlsym\n");
        return 1;
    }
    printf("RestartAppIfNecessary -> %d\n", restart(238010));
    if (!init()) {
        printf("FAIL: SteamAPI_Init returned false (is Steam running and logged in?)\n");
        return 1;
    }
    iface user = steam_user(), apps = steam_apps(), utils = steam_utils();
    int logged_on = VFN(user, 1, int (*)(iface))(user) & 0xff;
    uint64_t sid = VFN(user, 2, uint64_t(*)(iface))(user); // CSteamID returned in edx:eax
    int subscribed = VFN(apps, 6, int (*)(iface, unsigned))(apps, 238010) & 0xff;
    const char *lang = VFN(apps, 4, const char *(*)(iface))(apps);
    unsigned appid = VFN(utils, 9, unsigned (*)(iface))(utils);
    const char *country = VFN(utils, 4, const char *(*)(iface))(utils);
    unsigned rtime = VFN(utils, 3, unsigned (*)(iface))(utils);
    iface friends = steam_friends();
    const char *name = VFN(friends, 0, const char *(*)(iface))(friends);
    iface rs = steam_rs();
    int cloud_acct = VFN(rs, 18, int (*)(iface))(rs) & 0xff;
    int nfiles = VFN(rs, 15, int (*)(iface))(rs);
    printf("BLoggedOn=%d SteamID=%llx BIsSubscribedApp(238010)=%d language=%s\n", logged_on,
           (unsigned long long)sid, subscribed, lang ? lang : "(null)");
    printf("GetAppID=%u IPCountry=%s ServerRealTime=%u persona=%s cloud=%d files=%d\n", appid,
           country ? country : "(null)", rtime, name ? name : "(null)", cloud_acct, nfiles);

    // UserStatsReceived_t (k_iSteamUserStatsCallbacks + 1 = 1101): {uint64 gameID; EResult; CSteamID} = 20 bytes
    struct ccallback stats_cb = {cb_vtable, 0, 0, 0, 20};
    reg(&stats_cb, 1101);
    printf("registered: flags=%d icallback=%d\n", stats_cb.flags, stats_cb.icallback);
    iface stats = steam_stats();
    int req = VFN(stats, 0, int (*)(iface))(stats) & 0xff; // RequestCurrentStats
    // NumberOfCurrentPlayers_t (1107): {uint8 bSuccess; int32 cPlayers} = 8 bytes, via call result
    struct ccallback players_cb = {cb_vtable, 0, 1107, 0, 8};
    uint64_t call = VFN(stats, 33, uint64_t(*)(iface))(stats);
    if (reg_result && call) reg_result(&players_cb, call);
    printf("RequestCurrentStats=%d GetNumberOfCurrentPlayers call=%llx\n", req, (unsigned long long)call);
    for (int i = 0; i < 50 && (!stats_cb.hits || (call && !players_cb.hits)); i++) {
        run();
        usleep(100000);
    }
    unreg(&stats_cb);
    printf("stats callback hits=%d call result hits=%d flags after unregister=%d\n", stats_cb.hits,
           players_cb.hits, stats_cb.flags);
    if (stats_cb.hits) {
        unsigned nach = VFN(stats, 14, unsigned (*)(iface))(stats);
        printf("achievements: %u\n", nach);
        for (unsigned i = 0; i < nach && i < 3; i++) {
            const char *an = VFN(stats, 15, const char *(*)(iface, unsigned))(stats, i);
            unsigned char got = 0;
            VFN(stats, 6, int (*)(iface, const char *, unsigned char *))(stats, an, &got);
            printf("  %s achieved=%d\n", an, got);
        }
    }
    shutdown();
    int ok = logged_on && sid && subscribed && appid == 238010 && stats_cb.hits;
    printf("%s\n", ok ? "PASS" : "FAIL");
    return !ok;
}
