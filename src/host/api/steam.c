// Steam bridge: replaces the game's i386 libsteam_api.dylib (Steamworks SDK
// ~1.19/1.20, 2012) with host thunks that talk to the real, running Steam
// client through the host-architecture steamclient.dylib.
//
// - dlopen("…/libsteam_api.dylib") is redirected by dyld.c to a host pseudo
//   handle, so dlsym() finds the _SteamAPI_* / _Steam* thunks below.
// - SteamAPI_Init loads steamclient.dylib from the Steam install, gets
//   ISteamClient012 through CreateInterface and connects to the global user,
//   exactly like the old libsteam_api does (same interface version strings,
//   so the vtable layouts are those of the game's SDK).
// - Interface getters (SteamUser(), …) return guest proxy objects: 8 bytes of
//   guest memory {vtable, proxy slot}. Every vtable entry is a dynamic thunk
//   that marshals the i386 cdecl call (this, args…) to the same vtable index of
//   the host interface via libffi, driven by a per-method signature string.
// - SteamAPI_RunCallbacks pumps Steam_BGetCallback/Steam_FreeLastCallback and
//   dispatches to guest CCallbackBase objects (call results through
//   Steam_GetAPICallResult on SteamAPICallCompleted_t).
//
// Method lists transcribed from open-steamworks (github.com/SteamRE/
// open-steamworks, "Open Steamworks/ISteam*.h", non-Windows overload order),
// cross-checked against the vtable offsets used by the game's libsteam_api.
//
// ABI notes (verified by disassembling the game's i386 libsteam_api):
// - C++ member calls are cdecl with `this` as the first stack argument.
// - CSteamID (trivially copyable 8-byte class) is passed as 8 stack bytes and
//   returned in edx:eax; on the host (x86_64/arm64) it is passed/returned like
//   a uint64 in one integer register.
// - CCallbackBase: vtable {Run(void*), Run(void*, bool, SteamAPICall_t),
//   GetCallbackSizeBytes()}, uint8 m_nCallbackFlags at +4 (1 = registered,
//   2 = game server), int m_iCallback at +8.
// - Callback structs: i386 natural alignment == pack(4) used by the 64-bit
//   client on Apple, so they are copied byte for byte.
#include "common.h"
#include "../runtime.h"
#include "../macho.h"
#include "../strmap.h"

#include <dlfcn.h>
#include <ffi/ffi.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>

#define DEFAULT_APPID 238010

typedef int32_t HSteamPipe;
typedef int32_t HSteamUser;

// --- method tables -----------------------------------------------------------
//
// Signature "R:ARGS" (`this` implied). Argument letters (guest stack slots):
//   i int32/uint32/enum (4)   b bool (4)   h uint16 (4)   f float (4)
//   q uint64/CSteamID/CGameID (8)   d double (8)
//   p pointer to plain data (4, passed as g2h)   s const char * (4, g2h)
// Return letters: v void, i int32, b bool, c uint8, q uint64/CSteamID,
//   f float, S const char * (copied to guest), P interface pointer (wrapped
//   into a guest proxy by the version string, the last 's' argument).
// sig NULL: not bridged (logs the call and returns 0).

struct siface;
typedef void (*custom_fn)(struct guest_regs *r, void *host, int idx);
struct smethod {
    const char *name;
    const char *sig;
    custom_fn custom;
};

static void m_noop(struct guest_regs *r, void *host, int idx) { RET(0); }
static void m_ugc_details(struct guest_regs *r, void *host, int idx);

#define M(n, s) {#n, s, NULL}
#define MC(n, f) {#n, NULL, f}
#define MX(n) {#n, NULL, NULL}

static const struct smethod client012[] = {
    M(CreateSteamPipe, "i:"), M(BReleaseSteamPipe, "b:i"), M(ConnectToGlobalUser, "i:i"),
    M(CreateLocalUser, "i:pi"), M(ReleaseUser, "v:ii"), M(GetISteamUser, "P:iis"),
    M(GetISteamGameServer, "P:iis"), M(SetLocalIPBinding, "v:ih"), M(GetISteamFriends, "P:iis"),
    M(GetISteamUtils, "P:is"), M(GetISteamMatchmaking, "P:iis"), M(GetISteamMatchmakingServers, "P:iis"),
    M(GetISteamGenericInterface, "P:iis"), M(GetISteamUserStats, "P:iis"), M(GetISteamGameServerStats, "P:iis"),
    M(GetISteamApps, "P:iis"), M(GetISteamNetworking, "P:iis"), M(GetISteamRemoteStorage, "P:iis"),
    M(GetISteamScreenshots, "P:iis"), M(RunFrame, "v:"), M(GetIPCCallCount, "i:"),
    MC(SetWarningMessageHook, m_noop), M(BShutdownIfAllPipesClosed, "b:"), M(GetISteamHTTP, "P:iis"),
    M(GetISteamUnifiedMessages, "P:iis"), M(GetISteamController, "P:iis"),
};

static const struct smethod user016[] = {
    M(GetHSteamUser, "i:"), M(BLoggedOn, "b:"), M(GetSteamID, "q:"),
    M(InitiateGameConnection, "i:piqihb"), M(TerminateGameConnection, "v:ih"), M(TrackAppUsageEvent, "v:qis"),
    M(GetUserDataFolder, "b:pi"), M(StartVoiceRecording, "v:"), M(StopVoiceRecording, "v:"),
    M(GetAvailableVoice, "i:ppi"), M(GetVoice, "i:bpipbpipi"), M(DecompressVoice, "i:pipipi"),
    M(GetVoiceOptimalSampleRate, "i:"), M(GetAuthSessionTicket, "i:pip"), M(BeginAuthSession, "i:piq"),
    M(EndAuthSession, "v:q"), M(CancelAuthTicket, "v:i"), M(UserHasLicenseForApp, "i:qi"),
    M(BIsBehindNAT, "b:"), M(AdvertiseGame, "v:qih"), M(RequestEncryptedAppTicket, "q:pi"),
    M(GetEncryptedAppTicket, "b:pip"),
};

static const struct smethod friends013[] = {
    M(GetPersonaName, "S:"), M(SetPersonaName, "q:s"), M(GetPersonaState, "i:"),
    M(GetFriendCount, "i:i"), M(GetFriendByIndex, "q:ii"), M(GetFriendRelationship, "i:q"),
    M(GetFriendPersonaState, "i:q"), M(GetFriendPersonaName, "S:q"), M(GetFriendGamePlayed, "b:qp"),
    M(GetFriendPersonaNameHistory, "S:qi"), M(HasFriend, "b:qi"), M(GetClanCount, "i:"),
    M(GetClanByIndex, "q:i"), M(GetClanName, "S:q"), M(GetClanTag, "S:q"),
    M(GetClanActivityCounts, "b:qppp"), M(DownloadClanActivityCounts, "q:pi"), M(GetFriendCountFromSource, "i:q"),
    M(GetFriendFromSourceByIndex, "q:qi"), M(IsUserInSource, "b:qq"), M(SetInGameVoiceSpeaking, "v:qb"),
    M(ActivateGameOverlay, "v:s"), M(ActivateGameOverlayToUser, "v:sq"), M(ActivateGameOverlayToWebPage, "v:s"),
    M(ActivateGameOverlayToStore, "v:ii"), M(SetPlayedWith, "v:q"), M(ActivateGameOverlayInviteDialog, "v:q"),
    M(GetSmallFriendAvatar, "i:q"), M(GetMediumFriendAvatar, "i:q"), M(GetLargeFriendAvatar, "i:q"),
    M(RequestUserInformation, "b:qb"), M(RequestClanOfficerList, "q:q"), M(GetClanOwner, "q:q"),
    M(GetClanOfficerCount, "i:q"), M(GetClanOfficerByIndex, "q:qi"), M(GetUserRestrictions, "i:"),
    M(SetRichPresence, "b:ss"), M(ClearRichPresence, "v:"), M(GetFriendRichPresence, "S:qs"),
    M(GetFriendRichPresenceKeyCount, "i:q"), M(GetFriendRichPresenceKeyByIndex, "S:qi"),
    M(RequestFriendRichPresence, "v:q"), M(InviteUserToGame, "b:qs"), M(GetCoplayFriendCount, "i:"),
    M(GetCoplayFriend, "q:i"), M(GetFriendCoplayTime, "i:q"), M(GetFriendCoplayGame, "i:q"),
    M(JoinClanChatRoom, "q:q"), M(LeaveClanChatRoom, "b:q"), M(GetClanChatMemberCount, "i:q"),
    M(GetChatMemberByIndex, "q:qi"), M(SendClanChatMessage, "b:qs"), M(GetClanChatMessage, "i:qipipp"),
    M(IsClanChatAdmin, "b:qq"), M(IsClanChatWindowOpenInSteam, "b:q"), M(OpenClanChatWindowInSteam, "b:q"),
    M(CloseClanChatWindowInSteam, "b:q"), M(SetListenForFriendsMessages, "b:b"), M(ReplyToFriendMessage, "b:qs"),
    M(GetFriendMessage, "i:qipip"), M(GetFollowerCount, "q:q"), M(IsFollowing, "q:q"),
    M(EnumerateFollowingList, "q:i"),
};

static const struct smethod utils005[] = {
    M(GetSecondsSinceAppActive, "i:"), M(GetSecondsSinceComputerActive, "i:"), M(GetConnectedUniverse, "i:"),
    M(GetServerRealTime, "i:"), M(GetIPCountry, "S:"), M(GetImageSize, "b:ipp"),
    M(GetImageRGBA, "b:ipi"), M(GetCSERIPPort, "b:pp"), M(GetCurrentBatteryPower, "c:"),
    M(GetAppID, "i:"), M(SetOverlayNotificationPosition, "v:i"), M(IsAPICallCompleted, "b:qp"),
    M(GetAPICallFailureReason, "i:q"), M(GetAPICallResult, "b:qpiip"), M(RunFrame, "v:"),
    M(GetIPCCallCount, "i:"), MC(SetWarningMessageHook, m_noop), M(IsOverlayEnabled, "b:"),
    M(BOverlayNeedsPresent, "b:"), M(CheckFileSignature, "q:s"), M(ShowGamepadTextInput, "b:iisi"),
    M(GetEnteredGamepadTextLength, "i:"), M(GetEnteredGamepadTextInput, "b:pi"),
};

static const struct smethod apps005[] = {
    M(BIsSubscribed, "b:"), M(BIsLowViolence, "b:"), M(BIsCybercafe, "b:"),
    M(BIsVACBanned, "b:"), M(GetCurrentGameLanguage, "S:"), M(GetAvailableGameLanguages, "S:"),
    M(BIsSubscribedApp, "b:i"), M(BIsDlcInstalled, "b:i"), M(GetEarliestPurchaseUnixTime, "i:i"),
    M(BIsSubscribedFromFreeWeekend, "b:"), M(GetDLCCount, "i:"), M(BGetDLCDataByIndex, "b:ipppi"),
    M(InstallDLC, "v:i"), M(UninstallDLC, "v:i"), M(RequestAppProofOfPurchaseKey, "v:i"),
    M(GetCurrentBetaName, "b:pi"), M(MarkContentCorrupt, "b:b"), M(GetInstalledDepots, "i:pi"),
    M(GetAppInstallDir, "i:ipi"), M(BIsAppInstalled, "b:i"),
};

static const struct smethod userstats011[] = {
    M(RequestCurrentStats, "b:"), M(GetStat_int, "b:sp"), M(GetStat_float, "b:sp"),
    M(SetStat_int, "b:si"), M(SetStat_float, "b:sf"), M(UpdateAvgRateStat, "b:sfd"),
    M(GetAchievement, "b:sp"), M(SetAchievement, "b:s"), M(ClearAchievement, "b:s"),
    M(GetAchievementAndUnlockTime, "b:spp"), M(StoreStats, "b:"), M(GetAchievementIcon, "i:s"),
    M(GetAchievementDisplayAttribute, "S:ss"), M(IndicateAchievementProgress, "b:sii"),
    M(GetNumAchievements, "i:"), M(GetAchievementName, "S:i"), M(RequestUserStats, "q:q"),
    M(GetUserStat_int, "b:qsp"), M(GetUserStat_float, "b:qsp"), M(GetUserAchievement, "b:qsp"),
    M(GetUserAchievementAndUnlockTime, "b:qspp"), M(ResetAllStats, "b:b"), M(FindOrCreateLeaderboard, "q:sii"),
    M(FindLeaderboard, "q:s"), M(GetLeaderboardName, "S:q"), M(GetLeaderboardEntryCount, "i:q"),
    M(GetLeaderboardSortMethod, "i:q"), M(GetLeaderboardDisplayType, "i:q"), M(DownloadLeaderboardEntries, "q:qiii"),
    M(DownloadLeaderboardEntriesForUsers, "q:qpi"),
    MX(GetDownloadedLeaderboardEntry), // LeaderboardEntry_t layout differs (uint64 at +20 vs +24)
    M(UploadLeaderboardScore, "q:qiipi"), M(AttachLeaderboardUGC, "q:qq"), M(GetNumberOfCurrentPlayers, "q:"),
    M(RequestGlobalAchievementPercentages, "q:"), M(GetMostAchievedAchievementInfo, "i:pipp"),
    M(GetNextMostAchievedAchievementInfo, "i:ipipp"), M(GetAchievementAchievedPercent, "b:sp"),
    M(RequestGlobalStats, "q:i"), M(GetGlobalStat_int64, "b:sp"), M(GetGlobalStat_double, "b:sp"),
    M(GetGlobalStatHistory_int64, "i:spi"), M(GetGlobalStatHistory_double, "i:spi"),
};

static const struct smethod remotestorage010[] = {
    M(FileWrite, "b:spi"), M(FileRead, "i:spi"), M(FileForget, "b:s"),
    M(FileDelete, "b:s"), M(FileShare, "q:s"), M(SetSyncPlatforms, "b:si"),
    M(FileWriteStreamOpen, "q:s"), M(FileWriteStreamWriteChunk, "i:qpi"), M(FileWriteStreamClose, "i:q"),
    M(FileWriteStreamCancel, "i:q"), M(FileExists, "b:s"), M(FilePersisted, "b:s"),
    M(GetFileSize, "i:s"), M(GetFileTimestamp, "q:s"), M(GetSyncPlatforms, "i:s"),
    M(GetFileCount, "i:"), M(GetFileNameAndSize, "S:ip"), M(GetQuota, "b:pp"),
    M(IsCloudEnabledForAccount, "b:"), M(IsCloudEnabledForApp, "b:"), M(SetCloudEnabledForApp, "v:b"),
    M(UGCDownload, "q:qi"), M(GetUGCDownloadProgress, "b:qpp"), MC(GetUGCDetails, m_ugc_details),
    M(UGCRead, "i:qpii"), M(GetCachedUGCCount, "i:"), M(GetCachedUGCHandle, "q:i"),
    MX(PublishWorkshopFile), // SteamParamStringArray_t (pointer array) not translated
    M(CreatePublishedFileUpdateRequest, "q:q"), M(UpdatePublishedFileFile, "b:qs"),
    M(UpdatePublishedFilePreviewFile, "b:qs"), M(UpdatePublishedFileTitle, "b:qs"),
    M(UpdatePublishedFileDescription, "b:qs"), M(UpdatePublishedFileVisibility, "b:qi"),
    MX(UpdatePublishedFileTags), M(CommitPublishedFileUpdate, "q:q"), M(GetPublishedFileDetails, "q:q"),
    M(DeletePublishedFile, "q:q"), M(EnumerateUserPublishedFiles, "q:i"), M(SubscribePublishedFile, "q:q"),
    M(EnumerateUserSubscribedFiles, "q:i"), M(UnsubscribePublishedFile, "q:q"),
    M(UpdatePublishedFileSetChangeDescription, "b:qs"), M(GetPublishedItemVoteDetails, "q:q"),
    M(UpdateUserPublishedItemVote, "q:qb"), M(GetUserPublishedItemVoteDetails, "q:q"),
    MX(EnumerateUserSharedWorkshopFiles), MX(PublishVideo), M(SetUserPublishedFileAction, "q:qi"),
    M(EnumeratePublishedFilesByUserAction, "q:ii"), MX(EnumeratePublishedWorkshopFiles),
    M(UGCDownloadToLocation, "q:qsi"),
};

static const struct smethod networking005[] = {
    M(SendP2PPacket, "b:qpiii"), M(IsP2PPacketAvailable, "b:pi"), M(ReadP2PPacket, "b:pippi"),
    M(AcceptP2PSessionWithUser, "b:q"), M(CloseP2PSessionWithUser, "b:q"), M(CloseP2PChannelWithUser, "b:qi"),
    M(GetP2PSessionState, "b:qp"), M(AllowP2PPacketRelay, "b:b"), M(CreateListenSocket, "i:iihb"),
    M(CreateP2PConnectionSocket, "i:qiib"), M(CreateConnectionSocket, "i:ihi"), M(DestroySocket, "b:ib"),
    M(DestroyListenSocket, "b:ib"), M(SendDataOnSocket, "b:ipib"), M(IsDataAvailableOnSocket, "b:ip"),
    M(RetrieveDataFromSocket, "b:ipip"), M(IsDataAvailable, "b:ipp"), M(RetrieveData, "b:ipipp"),
    M(GetSocketInfo, "b:ipppp"), M(GetListenSocketInfo, "b:ipp"), M(GetSocketConnectionType, "i:i"),
    M(GetMaxPacketSize, "i:i"),
};

static const struct smethod matchmaking009[] = {
    M(GetFavoriteGameCount, "i:"), M(GetFavoriteGame, "b:ipppppp"), M(AddFavoriteGame, "i:iihhii"),
    M(RemoveFavoriteGame, "b:iihhi"), M(RequestLobbyList, "q:"), M(AddRequestLobbyListStringFilter, "v:ssi"),
    M(AddRequestLobbyListNumericalFilter, "v:sii"), M(AddRequestLobbyListNearValueFilter, "v:si"),
    M(AddRequestLobbyListFilterSlotsAvailable, "v:i"), M(AddRequestLobbyListDistanceFilter, "v:i"),
    M(AddRequestLobbyListResultCountFilter, "v:i"), M(AddRequestLobbyListCompatibleMembersFilter, "v:q"),
    M(GetLobbyByIndex, "q:i"), M(CreateLobby, "q:ii"), M(JoinLobby, "q:q"),
    M(LeaveLobby, "v:q"), M(InviteUserToLobby, "b:qq"), M(GetNumLobbyMembers, "i:q"),
    M(GetLobbyMemberByIndex, "q:qi"), M(GetLobbyData, "S:qs"), M(SetLobbyData, "b:qss"),
    M(GetLobbyDataCount, "i:q"), M(GetLobbyDataByIndex, "b:qipipi"), M(DeleteLobbyData, "b:qs"),
    M(GetLobbyMemberData, "S:qqs"), M(SetLobbyMemberData, "v:qss"), M(SendLobbyChatMsg, "b:qpi"),
    M(GetLobbyChatEntry, "i:qippip"), M(RequestLobbyData, "b:q"), M(SetLobbyGameServer, "v:qihq"),
    M(GetLobbyGameServer, "b:qppp"), M(SetLobbyMemberLimit, "b:qi"), M(GetLobbyMemberLimit, "i:q"),
    M(SetLobbyType, "b:qi"), M(SetLobbyJoinable, "b:qb"), M(GetLobbyOwner, "q:q"),
    M(SetLobbyOwner, "b:qq"), M(SetLinkedLobby, "b:qq"),
};

// Server browser: takes guest response objects (called back by the client) -
// not bridged.
static const struct smethod matchmakingservers002[] = {
    MX(RequestInternetServerList), MX(RequestLANServerList), MX(RequestFriendsServerList),
    MX(RequestFavoritesServerList), MX(RequestHistoryServerList), MX(RequestSpectatorServerList),
    MX(ReleaseRequest), MX(GetServerDetails), MX(CancelQuery), MX(RefreshQuery), MX(IsRefreshing),
    MX(GetServerCount), MX(RefreshServer), MX(PingServer), MX(PlayerDetails), MX(ServerRules),
    MX(CancelServerQuery),
};

static const struct smethod screenshots001[] = {
    M(WriteScreenshot, "i:piii"), M(AddScreenshotToLibrary, "i:ssii"), M(TriggerScreenshot, "v:"),
    M(HookScreenshots, "v:b"), M(SetLocation, "b:is"), M(TagUser, "b:iq"),
};

static const struct smethod http002[] = {
    M(CreateHTTPRequest, "i:is"), M(SetHTTPRequestContextValue, "b:iq"),
    M(SetHTTPRequestNetworkActivityTimeout, "b:ii"), M(SetHTTPRequestHeaderValue, "b:iss"),
    M(SetHTTPRequestGetOrPostParameter, "b:iss"), M(SendHTTPRequest, "b:ip"),
    M(SendHTTPRequestAndStreamResponse, "b:ip"), M(DeferHTTPRequest, "b:i"), M(PrioritizeHTTPRequest, "b:i"),
    M(GetHTTPResponseHeaderSize, "b:isp"), M(GetHTTPResponseHeaderValue, "b:ispi"),
    M(GetHTTPResponseBodySize, "b:ip"), M(GetHTTPResponseBodyData, "b:ipi"),
    M(GetHTTPStreamingResponseBodyData, "b:iipi"), M(ReleaseHTTPRequest, "b:i"),
    M(GetHTTPDownloadProgressPct, "b:ip"), M(SetHTTPRequestRawPostBody, "b:ispi"),
};

struct siface {
    const char *version;
    const char *short_name;
    const struct smethod *methods;
    int nmethods;
    gaddr_t vtable; // built lazily
};

#define IFACE(ver, name, tbl) {ver, name, tbl, (int)(sizeof tbl / sizeof tbl[0]), 0}
static struct siface ifaces[] = {
    IFACE("SteamClient012", "ISteamClient", client012),
    IFACE("SteamUser016", "ISteamUser", user016),
    IFACE("SteamFriends013", "ISteamFriends", friends013),
    IFACE("SteamUtils005", "ISteamUtils", utils005),
    IFACE("SteamMatchMaking009", "ISteamMatchmaking", matchmaking009),
    IFACE("SteamMatchMakingServers002", "ISteamMatchmakingServers", matchmakingservers002),
    IFACE("STEAMUSERSTATS_INTERFACE_VERSION011", "ISteamUserStats", userstats011),
    IFACE("STEAMAPPS_INTERFACE_VERSION005", "ISteamApps", apps005),
    IFACE("SteamNetworking005", "ISteamNetworking", networking005),
    IFACE("STEAMREMOTESTORAGE_INTERFACE_VERSION010", "ISteamRemoteStorage", remotestorage010),
    IFACE("STEAMSCREENSHOTS_INTERFACE_VERSION001", "ISteamScreenshots", screenshots001),
    IFACE("STEAMHTTP_INTERFACE_VERSION002", "ISteamHTTP", http002),
};
#define NIFACES ((int)(sizeof ifaces / sizeof ifaces[0]))

static struct siface *iface_by_version(const char *v) {
    for (int i = 0; i < NIFACES; i++)
        if (strcmp(ifaces[i].version, v) == 0) return &ifaces[i];
    return NULL;
}

// --- guest proxies --------------------------------------------------------------

static pthread_mutex_t steam_lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER;

#define MAX_PROXIES 256
static struct proxy {
    void *host;
    struct siface *iface;
    gaddr_t guest;
} proxies[MAX_PROXIES];
static int nproxies;

// Thunk name doubles as the method reference: thunk_name(id) returns the
// pointer passed to thunk_register_dynamic, i.e. &mref->name.
struct mref {
    char name[96];
    struct siface *iface;
    int idx;
    bool seen;
};

static void method_thunk(struct guest_regs *r);

static gaddr_t iface_vtable(struct siface *f) {
    if (f->vtable) return f->vtable;
    gaddr_t vt = gheap_calloc((uint32_t)f->nmethods + 1, 4);
    for (int i = 0; i < f->nmethods; i++) {
        struct mref *ref = calloc(1, sizeof *ref);
        snprintf(ref->name, sizeof ref->name, "%s::%s", f->short_name, f->methods[i].name);
        ref->iface = f;
        ref->idx = i;
        gwrite32(vt + 4 * (uint32_t)i, thunk_register_dynamic(ref->name, method_thunk));
    }
    f->vtable = vt;
    return vt;
}

static gaddr_t wrap_iface(void *host, const char *version) {
    if (!host) return 0;
    struct siface *f = iface_by_version(version);
    if (!f) {
        LOGE("steam: no method table for interface %s; returning NULL to the guest", version);
        return 0;
    }
    pthread_mutex_lock(&steam_lock);
    for (int i = 0; i < nproxies; i++)
        if (proxies[i].host == host && proxies[i].iface == f) {
            pthread_mutex_unlock(&steam_lock);
            return proxies[i].guest;
        }
    if (nproxies >= MAX_PROXIES) fatal("steam: too many interface proxies");
    gaddr_t g = gheap_calloc(1, 16);
    gwrite32(g, iface_vtable(f));
    gwrite32(g + 4, (uint32_t)nproxies);
    proxies[nproxies].host = host;
    proxies[nproxies].iface = f;
    proxies[nproxies].guest = g;
    nproxies++;
    pthread_mutex_unlock(&steam_lock);
    LOGI("steam: %s -> host %p, guest proxy %#x", version, host, g);
    return g;
}

static void *proxy_host(gaddr_t g) {
    uint32_t slot = gread32(g + 4);
    if (slot >= (uint32_t)nproxies || proxies[slot].guest != g) fatal("steam: bad interface proxy %#x", g);
    return proxies[slot].host;
}

// Host strings handed to the guest: interned (stable guest copies).
static struct strmap interned;
static gaddr_t intern_string(const char *s) {
    if (!s) return 0;
    pthread_mutex_lock(&steam_lock);
    if (!interned.slots) strmap_init(&interned);
    uint64_t v;
    gaddr_t g;
    if (strmap_get(&interned, s, &v)) g = (gaddr_t)v;
    else {
        g = gheap_strdup(s);
        strmap_put(&interned, s, g, false);
    }
    pthread_mutex_unlock(&steam_lock);
    return g;
}

static void *host_method(void *host, int idx) { return (*(void ***)host)[idx]; }

static void generic_call(struct guest_regs *r, void *host, const struct mref *ref, const char *sig) {
    ffi_type *types[24];
    void *vals[24];
    union {
        uint64_t u64;
        int32_t i32;
        uint8_t u8;
        uint16_t u16;
        float f;
        double d;
        void *p;
    } store[24];
    int n = 0, slot = 1; // guest argument 0 is `this`
    const char *version = NULL;
    types[n] = &ffi_type_pointer;
    store[n].p = host;
    n++;
    for (const char *a = sig + 2; *a; a++, n++) {
        switch (*a) {
        case 'i': types[n] = &ffi_type_sint32; store[n].i32 = (int32_t)ARG(slot++); break;
        case 'b': types[n] = &ffi_type_uint8; store[n].u8 = (ARG(slot++) & 0xff) != 0; break;
        case 'h': types[n] = &ffi_type_uint16; store[n].u16 = (uint16_t)ARG(slot++); break;
        case 'f': types[n] = &ffi_type_float; store[n].f = ARGF(slot); slot++; break;
        case 'q': types[n] = &ffi_type_uint64; store[n].u64 = ARG64(slot); slot += 2; break;
        case 'd': types[n] = &ffi_type_double; store[n].d = ARGD(slot); slot += 2; break;
        case 's':
            types[n] = &ffi_type_pointer;
            store[n].p = g2h(ARG(slot++));
            version = store[n].p;
            break;
        case 'p': types[n] = &ffi_type_pointer; store[n].p = g2h(ARG(slot++)); break;
        default: fatal("steam: bad signature %s for %s", sig, ref->name);
        }
        vals[n] = &store[n];
    }
    vals[0] = &store[0];
    ffi_type *rt;
    switch (sig[0]) {
    case 'v': rt = &ffi_type_void; break;
    case 'i': rt = &ffi_type_sint32; break;
    case 'b':
    case 'c': rt = &ffi_type_uint8; break;
    case 'q': rt = &ffi_type_uint64; break;
    case 'f': rt = &ffi_type_float; break;
    case 'S':
    case 'P': rt = &ffi_type_pointer; break;
    default: fatal("steam: bad signature %s for %s", sig, ref->name);
    }
    ffi_cif cif;
    if (ffi_prep_cif(&cif, FFI_DEFAULT_ABI, (unsigned)n, rt, types) != FFI_OK) fatal("steam: ffi_prep_cif failed");
    union {
        ffi_arg a;
        uint64_t u64;
        float f;
        void *p;
    } rv = {0};
    ffi_call(&cif, FFI_FN(host_method(host, ref->idx)), &rv, vals);
    switch (sig[0]) {
    case 'v': RET(0);
    case 'i': LOGD("steam: %s -> %d", ref->name, (int32_t)rv.a); RET((uint32_t)rv.a);
    case 'b':
    case 'c': LOGD("steam: %s -> %u", ref->name, (unsigned)(uint8_t)rv.a); RET((uint8_t)rv.a);
    case 'q': LOGD("steam: %s -> %#llx", ref->name, (unsigned long long)rv.u64); RET64(rv.u64);
    case 'f': RETD(rv.f);
    case 'S': LOGD("steam: %s -> \"%s\"", ref->name, rv.p ? (const char *)rv.p : "(null)"); RET(intern_string(rv.p));
    case 'P': RET(version ? wrap_iface(rv.p, version) : 0);
    }
}

static void method_thunk(struct guest_regs *r) {
    struct mref *ref = (struct mref *)thunk_name(r->thunk_id);
    const struct smethod *m = &ref->iface->methods[ref->idx];
    void *host = proxy_host(ARG(0));
    if (!ref->seen) {
        ref->seen = true;
        LOGI("steam: first call %s (vtable index %d)%s", ref->name, ref->idx,
             m->sig || m->custom ? "" : " - NOT BRIDGED, returning 0");
    }
    LOGT("steam: %s", ref->name);
    if (m->custom) return m->custom(r, host, ref->idx);
    if (!m->sig) {
        LOGD("steam: unbridged %s called, returning 0", ref->name);
        RET64(0);
    }
    generic_call(r, host, ref, m->sig);
}

// bool GetUGCDetails(UGCHandle_t, AppId_t *, char **ppchName, int32 *, CSteamID *)
static void m_ugc_details(struct guest_regs *r, void *host, int idx) {
    typedef bool (*fn_t)(void *, uint64_t, uint32_t *, char **, int32_t *, uint64_t *);
    char *name = NULL;
    bool ok = ((fn_t)host_method(host, idx))(host, ARG64(1), g2h(ARG(3)), &name, g2h(ARG(5)), g2h(ARG(6)));
    if (ARG(4)) gwrite32(ARG(4), intern_string(name));
    RET(ok);
}

// --- steamclient -------------------------------------------------------------

static void *client_lib;
static void *client; // ISteamClient012 (host)
static HSteamPipe hpipe;
static HSteamUser huser;
static gaddr_t g_client, g_user, g_friends, g_utils, g_mm, g_mms, g_stats, g_apps, g_net, g_rs, g_shots, g_http;

static bool (*p_BGetCallback)(HSteamPipe, void *msg, int32_t *call);
static void (*p_FreeLastCallback)(HSteamPipe);
static bool (*p_GetAPICallResult)(HSteamPipe, uint64_t, void *, int, int, bool *);

static void steam_dir(char *out, size_t cap) {
    const char *home = getenv("HOME");
    snprintf(out, cap, "%s/Library/Application Support/Steam", home ? home : "");
}

static bool load_steamclient(void) {
    if (client_lib) return true;
    char path[PATH_MAX];
    const char *env = getenv("DXH_STEAMCLIENT");
    if (env) strlcpy(path, env, sizeof path);
    else {
        steam_dir(path, sizeof path);
        strlcat(path, "/Steam.AppBundle/Steam/Contents/MacOS/steamclient.dylib", sizeof path);
    }
    client_lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!client_lib) {
        LOGE("steam: cannot load %s: %s", path, dlerror());
        return false;
    }
    LOGI("steam: loaded host %s", path);
    p_BGetCallback = (void *)dlsym(client_lib, "Steam_BGetCallback");
    p_FreeLastCallback = (void *)dlsym(client_lib, "Steam_FreeLastCallback");
    p_GetAPICallResult = (void *)dlsym(client_lib, "Steam_GetAPICallResult");
    if (!p_BGetCallback || !p_FreeLastCallback || !p_GetAPICallResult)
        LOGE("steam: steamclient lacks Steam_BGetCallback/FreeLastCallback/GetAPICallResult");
    return true;
}

// SteamPID from Steam's registry.vdf (what the modern libsteam_api checks).
static bool steam_running(void) {
    char path[PATH_MAX];
    steam_dir(path, sizeof path);
    strlcat(path, "/registry.vdf", sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) return false;
    char line[512];
    long pid = 0;
    while (fgets(line, sizeof line, f)) {
        char *p = strstr(line, "\"SteamPID\"");
        if (!p) continue;
        p += 10;
        while (*p && *p != '"') p++;
        if (*p == '"') pid = strtol(p + 1, NULL, 10);
        break;
    }
    fclose(f);
    return pid > 0 && kill((pid_t)pid, 0) == 0;
}

// The client associates the process with a game through SteamAppId; take it
// from steam_appid.txt next to the guest executable (or the cwd).
static void ensure_appid(void) {
    if (getenv("SteamAppId")) return;
    unsigned appid = 0;
    char path[PATH_MAX];
    struct image *exe = runtime_image(0);
    for (int i = 0; i < 2 && !appid; i++) {
        if (i == 0) {
            if (!exe || !exe->path) continue;
            strlcpy(path, exe->path, sizeof path);
            char *slash = strrchr(path, '/');
            if (!slash) continue;
            strlcpy(slash + 1, "steam_appid.txt", sizeof path - (size_t)(slash + 1 - path));
        } else
            strlcpy(path, "steam_appid.txt", sizeof path);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        if (fscanf(f, "%u", &appid) != 1) appid = 0;
        fclose(f);
        if (appid) LOGI("steam: app id %u from %s", appid, path);
    }
    if (!appid) appid = DEFAULT_APPID;
    char buf[16];
    snprintf(buf, sizeof buf, "%u", appid);
    setenv("SteamAppId", buf, 1);
    if (!getenv("SteamGameId")) setenv("SteamGameId", buf, 1);
}

typedef HSteamPipe (*create_pipe_fn)(void *);
typedef HSteamUser (*connect_user_fn)(void *, HSteamPipe);
typedef void *(*get_iface3_fn)(void *, HSteamUser, HSteamPipe, const char *);
typedef void *(*get_iface2_fn)(void *, HSteamPipe, const char *);
typedef bool (*release_pipe_fn)(void *, HSteamPipe);
typedef void (*release_user_fn)(void *, HSteamPipe, HSteamUser);
typedef bool (*bool0_fn)(void *);
typedef uint32_t (*u32_0_fn)(void *);

static gaddr_t get_iface(int idx, const char *version) {
    void *h = ((get_iface3_fn)host_method(client, idx))(client, huser, hpipe, version);
    if (!h) LOGE("steam: client returned no %s", version);
    return wrap_iface(h, version);
}

static void steam_shutdown(void) {
    if (!client) return;
    if (huser) ((release_user_fn)host_method(client, 4))(client, hpipe, huser);
    if (hpipe) ((release_pipe_fn)host_method(client, 1))(client, hpipe);
    ((bool0_fn)host_method(client, 22))(client);
    huser = 0;
    hpipe = 0;
    g_user = g_friends = g_utils = g_mm = g_mms = g_stats = g_apps = g_net = g_rs = g_shots = g_http = 0;
}

static bool steam_init(void) {
    if (hpipe && huser) return true;
    ensure_appid();
    if (!steam_running()) LOGI("steam: Steam does not look like it is running (registry SteamPID); trying anyway");
    if (!load_steamclient()) {
        fputs("[S_API FAIL] SteamAPI_Init() failed; unable to locate a running instance of Steam, or a local "
              "steamclient.dll.\n",
              stderr);
        return false;
    }
    if (!client) {
        void *(*create)(const char *, int *) = (void *)dlsym(client_lib, "CreateInterface");
        client = create ? create("SteamClient012", NULL) : NULL;
        if (!client) {
            LOGE("steam: CreateInterface(SteamClient012) failed");
            return false;
        }
        g_client = wrap_iface(client, "SteamClient012");
    }
    hpipe = ((create_pipe_fn)host_method(client, 0))(client);
    if (!hpipe) {
        LOGE("steam: CreateSteamPipe failed (is Steam running?)");
        fputs("[S_API FAIL] SteamAPI_Init() failed; unable to locate a running instance of Steam.\n", stderr);
        return false;
    }
    huser = ((connect_user_fn)host_method(client, 2))(client, hpipe);
    if (!huser) {
        LOGE("steam: ConnectToGlobalUser failed (not logged in?)");
        steam_shutdown();
        return false;
    }
    // Same order and versions as CSteamAPIContext::Init in the game's SDK.
    g_user = get_iface(5, "SteamUser016");
    g_friends = get_iface(8, "SteamFriends013");
    g_utils = wrap_iface(((get_iface2_fn)host_method(client, 9))(client, hpipe, "SteamUtils005"), "SteamUtils005");
    g_mm = get_iface(10, "SteamMatchMaking009");
    g_mms = get_iface(11, "SteamMatchMakingServers002");
    g_stats = get_iface(13, "STEAMUSERSTATS_INTERFACE_VERSION011");
    g_apps = get_iface(15, "STEAMAPPS_INTERFACE_VERSION005");
    g_net = get_iface(16, "SteamNetworking005");
    g_rs = get_iface(17, "STEAMREMOTESTORAGE_INTERFACE_VERSION010");
    g_shots = get_iface(18, "STEAMSCREENSHOTS_INTERFACE_VERSION001");
    g_http = get_iface(23, "STEAMHTTP_INTERFACE_VERSION002");
    if (!g_user || !g_friends || !g_utils || !g_mm || !g_mms || !g_stats || !g_apps || !g_net || !g_rs ||
        !g_shots || !g_http) {
        LOGE("steam: missing interfaces");
        steam_shutdown();
        return false;
    }
    void *utils = proxy_host(g_utils);
    uint32_t appid = ((u32_0_fn)host_method(utils, 9))(utils);
    char buf[24];
    snprintf(buf, sizeof buf, "%u", appid);
    if (!getenv("SteamAppId")) setenv("SteamAppId", buf, 1);
    if (!getenv("SteamGameId")) setenv("SteamGameId", buf, 1);
    void *user = proxy_host(g_user);
    LOGI("steam: SteamAPI_Init ok: pipe %d user %d appid %u logged on %d", hpipe, huser, appid,
         ((bool0_fn)host_method(user, 1))(user));
    return true;
}

// --- callbacks -----------------------------------------------------------------

#define CB_REGISTERED 1
#define CB_GAMESERVER 2
#define K_ISTEAMAPICALLCOMPLETED 703

static struct gcb {
    gaddr_t obj;
    int icb;
} *callbacks;
static int ncallbacks, capcallbacks;
static struct gcallres {
    gaddr_t obj;
    uint64_t call;
} *callresults;
static int ncallresults, capcallresults;

static uint32_t cb_size(gaddr_t obj) { return gcall1(gread32(gread32(obj) + 8), obj); }

THUNK(SteamAPI_RegisterCallback) {
    gaddr_t obj = ARG(0);
    int icb = ARGI(1);
    gwrite8(obj + 4, gread8(obj + 4) | CB_REGISTERED);
    gwrite32(obj + 8, (uint32_t)icb);
    pthread_mutex_lock(&steam_lock);
    if (ncallbacks == capcallbacks) {
        capcallbacks = capcallbacks ? capcallbacks * 2 : 32;
        callbacks = realloc(callbacks, sizeof *callbacks * (size_t)capcallbacks);
    }
    callbacks[ncallbacks++] = (struct gcb){obj, icb};
    pthread_mutex_unlock(&steam_lock);
    LOGD("steam: RegisterCallback(%#x, %d)", obj, icb);
}

THUNK(SteamAPI_UnregisterCallback) {
    gaddr_t obj = ARG(0);
    pthread_mutex_lock(&steam_lock);
    for (int i = 0; i < ncallbacks; i++)
        if (callbacks[i].obj == obj) {
            callbacks[i] = callbacks[--ncallbacks];
            i--;
        }
    pthread_mutex_unlock(&steam_lock);
    gwrite8(obj + 4, gread8(obj + 4) & ~CB_REGISTERED);
}

THUNK(SteamAPI_RegisterCallResult) {
    gaddr_t obj = ARG(0);
    uint64_t call = ARG64(1);
    pthread_mutex_lock(&steam_lock);
    if (ncallresults == capcallresults) {
        capcallresults = capcallresults ? capcallresults * 2 : 32;
        callresults = realloc(callresults, sizeof *callresults * (size_t)capcallresults);
    }
    callresults[ncallresults++] = (struct gcallres){obj, call};
    pthread_mutex_unlock(&steam_lock);
    LOGD("steam: RegisterCallResult(%#x, %#llx)", obj, (unsigned long long)call);
}

THUNK(SteamAPI_UnregisterCallResult) {
    gaddr_t obj = ARG(0);
    uint64_t call = ARG64(1);
    pthread_mutex_lock(&steam_lock);
    for (int i = 0; i < ncallresults; i++)
        if (callresults[i].obj == obj && callresults[i].call == call) {
            callresults[i] = callresults[--ncallresults];
            break;
        }
    pthread_mutex_unlock(&steam_lock);
}

static bool callback_registered(gaddr_t obj, int icb) {
    bool found = false;
    pthread_mutex_lock(&steam_lock);
    for (int i = 0; i < ncallbacks && !found; i++) found = callbacks[i].obj == obj && callbacks[i].icb == icb;
    pthread_mutex_unlock(&steam_lock);
    return found;
}

static void dispatch_call_result(const uint8_t *param, int cub) {
    uint64_t call;
    memcpy(&call, param, 8);
    gaddr_t obj = 0;
    pthread_mutex_lock(&steam_lock);
    for (int i = 0; i < ncallresults; i++)
        if (callresults[i].call == call) {
            obj = callresults[i].obj;
            callresults[i] = callresults[--ncallresults];
            break;
        }
    pthread_mutex_unlock(&steam_lock);
    if (!obj) return;
    uint32_t gsize = cb_size(obj);
    int expected = (int)gread32(obj + 8);
    // Newer clients report the result size in SteamAPICallCompleted_t.
    uint32_t hsize = gsize;
    if (cub >= 16) {
        uint32_t reported;
        memcpy(&reported, param + 12, 4);
        if (reported > hsize && reported < (1u << 20)) hsize = reported;
    }
    uint8_t *hbuf = calloc(1, hsize);
    bool failed = false;
    bool ok = p_GetAPICallResult && p_GetAPICallResult(hpipe, call, hbuf, (int)hsize, expected, &failed);
    if (!ok && hsize != gsize) ok = p_GetAPICallResult(hpipe, call, hbuf, (int)gsize, expected, &failed);
    LOGD("steam: call result %#llx (callback %d, %u bytes) ok=%d failed=%d", (unsigned long long)call, expected,
         gsize, ok, failed);
    if (ok) {
        gaddr_t gbuf = gheap_calloc(1, gsize ? gsize : 4);
        memcpy(g2h(gbuf), hbuf, gsize);
        uint32_t args[5] = {obj, gbuf, failed, (uint32_t)call, (uint32_t)(call >> 32)};
        cpu_call(gread32(gread32(obj) + 4), 5, args);
        gheap_free(gbuf);
    }
    free(hbuf);
}

static void dispatch_callback(int icb, const uint8_t *param, int cub, bool server) {
    if (icb == K_ISTEAMAPICALLCOMPLETED && cub >= 8) dispatch_call_result(param, cub);
    gaddr_t objs[64];
    int n = 0;
    pthread_mutex_lock(&steam_lock);
    for (int i = 0; i < ncallbacks && n < 64; i++)
        if (callbacks[i].icb == icb) objs[n++] = callbacks[i].obj;
    pthread_mutex_unlock(&steam_lock);
    if (n) LOGD("steam: callback %d (%d bytes) -> %d guest listener(s)", icb, cub, n);
    else LOGT("steam: callback %d (%d bytes), no listener", icb, cub);
    for (int i = 0; i < n; i++) {
        gaddr_t obj = objs[i];
        if (!callback_registered(obj, icb)) continue; // unregistered by an earlier Run
        if (!!(gread8(obj + 4) & CB_GAMESERVER) != server) continue;
        uint32_t gsize = cb_size(obj);
        uint32_t size = gsize > (uint32_t)cub ? gsize : (uint32_t)cub;
        gaddr_t gbuf = gheap_calloc(1, size ? size : 4);
        memcpy(g2h(gbuf), param, (size_t)cub);
        gcall2(gread32(gread32(obj)), obj, gbuf);
        gheap_free(gbuf);
    }
}

// CallbackMsg_t { HSteamUser m_hSteamUser; int m_iCallback; uint8 *m_pubParam; int m_cubParam; }
struct callback_msg {
    int32_t user;
    int32_t icb;
    uint8_t *param;
    int32_t cub;
};

static void run_callbacks(HSteamPipe pipe, bool server) {
    static _Thread_local bool running;
    if (!pipe || !p_BGetCallback || running) return;
    running = true;
    struct callback_msg msg;
    int32_t call = 0;
    while (p_BGetCallback(pipe, &msg, &call)) {
        // Copy before freeing: guest code may call back into Steam.
        uint8_t *copy = malloc(msg.cub > 0 ? (size_t)msg.cub : 1);
        memcpy(copy, msg.param, msg.cub > 0 ? (size_t)msg.cub : 0);
        int icb = msg.icb, cub = msg.cub;
        p_FreeLastCallback(pipe);
        dispatch_callback(icb, copy, cub, server);
        free(copy);
    }
    running = false;
}

// --- flat API -------------------------------------------------------------------

THUNK(SteamAPI_Init) { RET(steam_init()); }
THUNK(SteamAPI_InitSafe) { RET(steam_init()); }
THUNK(SteamAPI_Shutdown) { steam_shutdown(); }
THUNK(SteamAPI_IsSteamRunning) { RET(steam_running() || hpipe); }
// We are the launcher: never ask Steam to restart the game.
THUNK(SteamAPI_RestartAppIfNecessary) {
    LOGI("steam: SteamAPI_RestartAppIfNecessary(%u) -> false", ARG(0));
    RET(0);
}
THUNK(SteamAPI_RunCallbacks) { run_callbacks(hpipe, false); }
THUNK(Steam_RunCallbacks) { run_callbacks(ARGI(0), ARG(1) & 0xff); }
THUNK(Steam_RegisterInterfaceFuncs) {}
THUNK(Steam_GetHSteamUserCurrent) { RET(huser); }
THUNK(GetHSteamPipe) { RET(hpipe); }
THUNK(GetHSteamUser) { RET(huser); }
THUNK(SteamAPI_GetHSteamPipe) { RET(hpipe); }
THUNK(SteamAPI_GetHSteamUser) { RET(huser); }
THUNK(SteamAPI_GetSteamInstallPath) {
    char path[PATH_MAX];
    steam_dir(path, sizeof path);
    strlcat(path, "/Steam.AppBundle/Steam/Contents/MacOS", sizeof path);
    RET(intern_string(path));
}

// Breakpad / minidumps: no-ops.
THUNK(SteamAPI_SetMiniDumpComment) {}
THUNK(SteamAPI_WriteMiniDump) {}
THUNK(SteamAPI_UseBreakpadCrashHandler) {}
THUNK(SteamAPI_SetBreakpadAppID) {}
THUNK(SteamAPI_SetTryCatchCallbacks) {}

THUNK(SteamClient) { RET(g_client); }
THUNK(SteamUser) { RET(g_user); }
THUNK(SteamFriends) { RET(g_friends); }
THUNK(SteamUtils) { RET(g_utils); }
THUNK(SteamMatchmaking) { RET(g_mm); }
THUNK(SteamMatchmakingServers) { RET(g_mms); }
THUNK(SteamUserStats) { RET(g_stats); }
THUNK(SteamApps) { RET(g_apps); }
THUNK(SteamNetworking) { RET(g_net); }
THUNK(SteamRemoteStorage) { RET(g_rs); }
THUNK(SteamScreenshots) { RET(g_shots); }
THUNK(SteamHTTP) { RET(g_http); }

// Game server / content server API: not supported (a client game).
#define GS_STUB(sym)                                                                               \
    THUNK(sym) {                                                                                   \
        LOGI("steam: %s not supported, returning 0", #sym);                                         \
        RET64(0);                                                                                  \
    }
GS_STUB(SteamGameServer_Init)
GS_STUB(SteamGameServer_InitSafe)
GS_STUB(SteamGameServer_Shutdown)
GS_STUB(SteamGameServer_RunCallbacks)
GS_STUB(SteamGameServer_BSecure)
GS_STUB(SteamGameServer_GetSteamID)
GS_STUB(SteamGameServer_GetHSteamPipe)
GS_STUB(SteamGameServer_GetHSteamUser)
GS_STUB(SteamGameServer_GetIPCCallCount)
GS_STUB(SteamGameServer)
GS_STUB(SteamGameServerUtils)
GS_STUB(SteamGameServerApps)
GS_STUB(SteamGameServerNetworking)
GS_STUB(SteamGameServerStats)
GS_STUB(SteamGameServerHTTP)
GS_STUB(SteamContentServer)
GS_STUB(SteamContentServerUtils)
GS_STUB(SteamContentServer_Init)
GS_STUB(SteamContentServer_Shutdown)
GS_STUB(SteamContentServer_RunCallbacks)
