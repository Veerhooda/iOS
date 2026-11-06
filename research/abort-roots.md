# Abort root-cause hunt: slides, chains, toolchain (run105 era)

## 1. TRUE cache slides (dual-anchor verified)

Prior slides were all +8 too high (page-alignment violation went unnoticed).
Ground truth anchors from cache bytes:
- `__pthread_kill` mov x16,#0x148 at 0x180a630d4, SVC at 0x180a630d8;
  reaped threads sit at **SVC+4 = 0x180a630dc** (EINTR return).
- libdyld `_start` = 0x1809168dc (local symbols); ESTK holds **_start+4 = 0x1809168e0**.

`tools/op2slide.py` requires BOTH anchors to agree on a page-aligned slide:

| run | slide (TRUE) | old (wrong) |
|-----|-------------|-------------|
| 101 | 0x11AD8000 | 0x11ad8008 |
| 102 | 0xB5F6C000 | 0xb5f6c008 |
| 103 | 0x1F2A8000 | 0x1f2a8008 |
| 104 | 0x7F520000 | 0x7f520008 |
| 105 | 0x288B0000 | 0x28ab008/0x28ab000 |

All previously published unslid PCs are 8 too LOW (add 8 to fileoffs).

## 2. SpringBoard abort chain (run105 #15, slide 0x288B0000) — COMPLETE

SBControlCenterController -init+0x574
-> SBWindow initWithScreen:debugName:rootViewController:+0x58
-> SBWindow _initWithScreen:...scene:+0x318
-> UIWindow setRootViewController:+0x1dc
-> UIWindow addRootViewControllerViewIfPossible+0x88
-> UIViewController view+0x1c -> loadViewIfRequired+0x3f4
-> CCUIModularControlCenterOverlayViewController viewDidLoad+0x6d8
-> _reparent (+0x1c) -> _reparentCollectionViewController (+0x30)
-> +[CCUIModularControlCenterViewController _sharedCollectionViewController]+0x38
   (dispatch_once; block = 0x1a3bb27d0)
-> [[CCUIModuleCollectionViewController alloc] initWithModuleInstanceManager:
    [CCUIModuleInstanceManager sharedInstance]]
-> -[CCUIModuleInstanceManager _initWithSystemAgent:repository:]
-> -[CCUIModuleInstanceManager _updateModuleInstances]
-> per-module loop: -[CCSModuleRepository moduleMetadataForModuleIdentifier:]
   -> -[CCUIModuleInstanceManager _instantiateModuleWithMetadata:]
-> ??? dict literal with objects[4]==nil → NSInvalidArgumentException

CCUI contains NO direct dictionaryWithObjects/initWithObjects/dictionaryWithObjectsAndKeys/
arrayWithObjects calls (proven by tools/op2calls.py selector+selref scan).
The dict is built in a callee image (ControlCenterServices? module bundle? Foundation
via IMP-cached call?). 28 module bundles present on image.
NOTE: ESTK mixes stale slots (SBControlCenterWindow hitTest:withEvent:+0xc4 in same
dump is from an unrelated earlier event) — tracev3 backtraces are authoritative.

## 3. suggestd abort chain (run105 #16) — COMPLETE to dispatch_sync

suggestd main (fileoff 0x37e0) -> +[SGSqlEntityStore defaultStore]+0x88
-> storeWithDefaultLocationAndExecuteJournals:+0xdc
-> sqlStoreWithEntityDbPath:...+0xdc -> initWithEntityDbPath:...:noMigrate:+0xe70
-> -[SGSqlEntityStore migrateDatabases]+0xcc (= 0x1a407551c, after dispatch_sync call)
-> dispatch_sync(queue, migrator-block @ 0x1a407dc3c)  [stub chain resolved]
-> block: prepMigrator, maxKnownDbVersion, -[SGSqlEntityStore migrate]
-> ??? dict literal objects[4]==nil
CSI contains NO dictionaryWithObjects/initWithObjects selrefs either.

## 4. itunescloudd abort chain (run105 #42) — COMPLETE to _init callees

-[SagaCloudAddItemToPlaylistOperation logCloudAddRequestDescription]+0x6d4
-> +[CloudServer server]+0x4 (dispatch_once; block=[[CloudServer alloc] _init])
-> -[CloudServer _init] (0x10008e324, fully read: NO dict literal)
-> sync callees: _initSerialQueues, HSCloudClient*, NSUserDefaults+observers,
   +[ICDeviceInfo currentDeviceInfo] (→ defaultInfo → _init; all read, NO dict),
   -isAppleTV (→ _gestaltDeviceClass cmp; NO dict),
   SSVSubscriptionStatusCoordinator, MPStoreDownloadManager (MediaPlayer!),
   HSCloudAvailabilityController, ICSecurityInfo
Hypothesis: itunescloudd = Crash B (MediaPlayer routingContextUID assertion),
not Crash A. Awaiting fresh-tracev3 confirmation (reason+process mapping).

## 5. Toolchain (all in tools/)

- dyld_extract.py: v1-cache single-image extractor. Handles stale fileoff/vmaddr
  (source bytes via mapping(vmaddr)), drops LINKEDIT, patches section offsets
  (+48!), zeroes LINKEDIT load commands, strips high-bit pointer tags (bit41/42,
  33-bit vmaddrs) in pointer-only __DATA sections. Output works with otool -tV/-oV.
- op2slide.py: dual-anchor slide verification (SVC+4 & _start+4 must agree).
- op2methods.py: full ObjC method dump / IMP→method matching from cache bytes.
- op2calls.py: selector/selref → __TEXT call-site finder (direct + indirect refs).
- Local-symbols table: cache_off 0x40200000 (nlistOffset/Count, stringsOffset/Size,
  entriesOffset/Count); 2.8M entries; statics only (no exported symbols).

## 6. itunescloudd SOLVED (missing DB, not a dict bug)

Fresh-tracev3 full stack (boot slide 0x1B9EC000, 11 frames, process confirmed
by adjacent `itunescloudd` + `ROC_EXIT_CRASH` markers):

logCloudAddRequestDescription+0x6d4 → +[CloudServer server] → dispatch_once
→ [[CloudServer alloc] _init] → HSCloudClientGetPreferredVideoQuality
→ -[ML3MusicLibrary valueForDatabaseProperty:@"MLCloudDatabasePreferredVideoQuality"]
→ -[ML3MusicLibrary databaseConnectionAllowingWrites:withBlock:]
→ block → sqlite open → `os_unix.c: No such file or directory`
→ NSGenericException (errno=2) → SIGABRT.

f0 of every First-throw stack is ___exceptionPreprocess+0xE4 (CF+0x119180),
NOT the collection init — the init/CFThrow/objc_throw frames tail-call away.
Fix: tools/mkop2root.py tweak_medialibrary() seeds
/private/var/mobile/Media/iTunes_Control/iTunes/MediaLibrary.sqlitedb with an
empty _MLDatabaseProperties(key TEXT PRIMARY KEY, value BLOB) table. The only
query on the path returns 0 rows → nil → caller defaults to -1 (verified
nil-tolerant in HomeSharing+0x2bba4: cbz → mov x20,#-1). Matches port's
501:20/0644 DB convention (cf. FrontBoard applicationState.db).

## 7. SpringBoard SOLVED (Crash B, not Crash A!)

run109 fresh tracev3 full stack (slide 0xB8CC000, fragments reconstructed):
loadViewIfRequired → -[CCUIModuleInstanceManager _instantiateModuleWithMetadata:]+0x334
→ MediaControlsModule -init:
   MRAVOutputContextGetSharedAudioPresentationContext() -> nil (NO AUDIO HW)
   -> [config setRoutingContextUID:nil]
→ -[MediaControlsEndpointsManager initWithConfiguration:]+0x2e0
→ -[MPAVEndpointRoutingDataSource setRoutingContextUID:]+0x228
→ `cbz x0` (nil UID) -> NSAssert "Invalid parameter" (MPAVEndpointRoutingDataSource.m:62).
ESTK's CCUI-singleton frames were STALE (the live chain goes through module
instantiation, not _sharedCollectionViewController).
Fix: tools/mkop2root.py tweak_controlcenter() removes
com.apple.mediaremote.controlcenter.nowplaying from ControlCenterServices'
ModuleWhitelist.plist (31->30). iOS tolerates missing (user-removable) modules.
AirPlayMirroring (also mediaremote) is the prime suspect if SB still crashes.

## 8. itunescloudd iteration 2 (worker "no such table")

run109 stack 1: NSInternalInconsistency "no such table" on
  SELECT 1 FROM item JOIN item_store USING (item_pid)
  WHERE in_my_library AND cloud_status = ? LIMIT 1
(empty seeded DB lacks item/item_store). Fix: seed minimal shells; the exact
query verified to return [] (empty-OK). item_store uses the real iOS 12
CREATE TABLE + cloud_status (migration-added); item is minimal
(item_pid, in_my_library) since its base CREATE is dynamic.

## 9. run110: ALL CRASHERS SILENCED (SB survives!)

run110 600s census: SB 0 (spawned + alive all run, was 4), ITC 0, SG 0,
ndoagent 0. SB main thread at deadline is decoding UI assets
(ImageIO + libate + dispatch) — forward progress past CC init. Setup
Assistant not yet launched at 600s (single-CPU boot still slow).
ITC/SG never launched in run110 (variance; their fixes await a launching run).

## 10. run111 (2h): HOME SCREEN + new background crashers

User photo confirms the iOS home screen (icons, status bar, clock) on the
OnePlus 2. Census: SB 0/2h, ndoagent 0, ITC 1 (same worker site as run109 —
item shells did not silence it; reason still unknown, stack in compressed
chunks), suggestd 0 (never launched).
New: ~30 Spotlight importer crashes (ContactsCore/Reminders/MobileSMS/
Calendar/FPSpotlightIndex, all IDENTICAL stacks): PKService main → run →
_prepareToRun → checkEnvironment:+0x170 → NSInternalInconsistency
"plug-in has invalid sandbox or container: error 2 path ?" (missing plugin
container dirs — Setup normally creates them). Background-only; UI unaffected.

## 11. Pending

- Touchscreen (user-reported dead): next user-visible frontier (Synaptics
  driver + IOHID + BackBoard touch pipeline).
- Background noise (non-blocking): ITC worker reason; Spotlight container
  seeding (or disable mdworker jobs).

## 8. Build gotcha (run106/107 lesson)

`out/kernel.development.msm8994` goes STALE: the real build output is
`xnu/xnu-4903.270.47/BUILD/obj/kernel.development.msm8994` (symlink into
DEVELOPMENT_ARM64_MSM8994/). Rebuilding the payload/boot.img from `out/`
silently produced a 78KB-smaller image (Oct-31 kernel, missing run104/105
features) that dies ~200s in with "Halt/Restart Timed Out". Always refresh
`out/` from `BUILD/obj/` before mkpayload (payload must be 9862288 bytes
for the run105 feature set). Verified: backup run105 image boots fine on
the DB-seeded reflash.
