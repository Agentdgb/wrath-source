#include <windows.h>
#include "core/module/Module.h"
#include "modules/shared/NativeAccess.h"
#include "modules/combat/silentaim/silentaim.h"
#include "modules/combat/crystal/blast.h"
#include "modules/combat/crystal/explosive_geom.h"
#include "modules/render/blockesp/blockesp.h"
#include "core/social/EntityFriends.h"   // a friend is not a target
#include "vgui/bridge.h"
#include "util/Log.h"
#include <algorithm>
#include <cmath>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

// Auto Crystal / Anchor Macro / Safe Anchor.
//
// Split in two halves on purpose, because Grim judges WHERE in the tick a
// packet was born:
//
//   the pump (GameLoopEvent, after the frame was rendered, i.e. AFTER this
//   tick's flying packet) only CHOOSES: which base, which crystal, which slot,
//   where to aim. It leases the rotation from silentaim and hands over a
//   callback. It never sends a packet and never touches the selected slot --
//   anything sent from here lands in Grim's Post window (interaction after the
//   flying, before the next transaction) and a slot switch here is
//   PacketOrderE.
//
//   the callback runs INSIDE MinecraftClient.tick(), from silentaim's pre-tick
//   anchor (handleInputEvents, where vanilla itself attacks and uses items)
//   with the spoofed yaw/pitch already written into the player. It re-verifies
//   the target by raycasting along the APPLIED rotation, then emits exactly the
//   vanilla sequence -- Attack Swing, or [UpdateSelectedSlot] Interact Swing --
//   and the flying that follows carries the same rotation. The slot goes back
//   on a later, idle tick.
//
// ONE ACTION TYPE PER TICK. Grim's MultiActionsF flags a block placement in
// the same tick as an attack ("Interacting with a block and an entity in the
// same tick", action=place) and PacketOrderJ flags a use item after an attack
// without an entity interact. So a tick carries either the attack or the
// placement, never both: tick N attacks the crystal (+swing), tick N+1 places
// the next one on the same base (+swing) from the SAME aim point -- the lease
// stays, the applied angle does not move between the two ticks, and the aim
// point was drawn so that one ray satisfies both halves (enters the crystal
// box for the attack, hits the base's top face for the placement). execute()
// remembers the tick id of the last attack and of the last block interaction
// and refuses the other kind on the same tick id, whatever the pump planned.
// Client-side the dead crystal lingers until the destroy packet arrives, so
// every crystal this module hit is remembered by entity id and treated as
// gone -- for the occupancy check, for the "client FAIL explained by the
// lingering crystal" reading of the placement result, and so it is never
// attacked twice.
//
// PLACEMENT VALIDATION is Grim's RotationPlace rule, not just "the ray hits
// the support face": for every right-click with a BLOCK ITEM in hand (an
// anchor, glowstone charging an anchor, a shield block, obsidian clicking
// anything) a ray from the eye -- and from the post-velocity eye when moving,
// Grim raycasts from the position of the flying that follows -- along the
// applied yaw/pitch, within the block interaction range, has to intercept the
// box of the PLACED position: the clicked block plus the clicked face's
// normal, or the clicked block itself when it is replaceable. The explode
// prefers a non-block item (sword, totem, tool, food...) so no place rule
// applies to it at all; when only block items are available the same rule
// is enforced for the explode too.
//
// THE WIRE. Grim's RotationPlace has a PRE-flying pass too: the moment the
// placement packet arrives it tests the rotation Grim holds right then --
// the PREVIOUS flying's (yaw,pitch), plus (lastYaw,pitch) and
// (lastYaw,lastPitch) -- against the placed box, and once its flag buffer is
// up (one post-flying miss anywhere, decaying only through post-flying hits)
// a pre-flying miss is flagged, DENIES the placement server-side and skips
// the post-flying pass that would decay the buffer. A module that rotates
// and clicks in the same tick therefore has every later block click denied:
// the anchor never exists in Grim's world, the glowstone a tick later clicks
// air (AirLiquidPlace + RotationPlace again), the shield is denied, nothing
// explodes. So every click Grim treats as a block place -- an anchor or any
// block item placed, glowstone charging the anchor, the shield, an explode
// with a block item in hand -- goes out only when Grim's pre-flying pass,
// run here against the rotation on the wire (ctx.wire_yaw/pitch and the
// prev-yaw combos), intercepts the placed cell, and silentaim forces a
// position flying with the applied rotation behind the click for the
// post-flying pass. Cadence when stationary: tick N the lease lands (look
// sent), tick N+1 the same pair is applied, the wire holds it, the click +
// forced flying go out, tick N+2 the next aim (look), tick N+3 the next
// click. While walking the aim angle to the fixed point shifts every tick
// (a Look each tick); the wire then carries last tick's aim at the same
// point, which still intercepts the cell, so the click is not delayed.
// Crystal placements (an end crystal is not a BlockPlace for Grim) and
// attacks are exempt and act on the tick the aim lands.
//
// Vulcan. The rotations that reach the wire are shaped in two places: the
// aim point is drawn at random inside the valid region for every action (a
// point on the top face away from the edges, or inside the crystal box), so
// the same base never produces the same angle twice, and the stepping in
// silentaim.cpp randomises speed per lease and per tick. When the player's
// own view already validates the action the lease targets the real view,
// i.e. nothing synthetic goes out at all.
//
// Damage. Bases are no longer picked by where the player happens to look
// when there is somebody to hit: every candidate is scored with the
// distance-only reading of the server's explosion formula (blast::estimate,
// pure C++, exposure assumed 1) and the one that hurts the nearest non-friend
// player most wins. The score falls monotonically with the distance from the
// explosion centre to the victim's feet, so the candidates are simply sorted
// by it and the first one the world agrees with (a valid base, a free spot, a
// face the eye reaches -- one ray) is the answer; no per-candidate ray grid,
// no armour, no self-damage veto of any kind: the player decides how close to
// stand. The Anchor Macro only places on its own when such a target is within
// 8 blocks; otherwise the real crosshair is its only source, and there the
// anchor goes wherever a click would put it: into the air block behind the
// looked-at face, top face or side face alike.
//
// Safe Anchor is the Anchor Macro with one more step between the adoption
// and the charge: a SHIELD block goes into the anchor's neighbour on the
// player's side (the axis with the larger player-anchor delta, x when exactly
// diagonal; at the anchor's height, else one lower, else one higher), placed
// against the top face of the block below the spot first, else another
// full-cube neighbour's face pointing into the spot -- never the anchor's own
// faces (they would charge / explode it) and never a block with a block
// entity (chests and the like open their screen instead of placing). The
// spot has to be air or replaceable and clear of the player's bounding box
// (World.canPlace refuses it otherwise). Skipped, logged once, when the spot
// is already filled by a full cube, no shield item is in the hotbar, no spot
// exists or Silent Aim is off (the crosshair cannot be guided to the support
// face); the charge follows either way.
//
// The order is place -> adopt -> shield -> charge -> explode on purpose: the
// shield needs its own aim (a face beside the anchor), the charge and the
// explode both click the anchor, so the explode REUSES the charge's aim point
// -- the lease keeps the same target, the wire never moves, only the slot
// changes to the non-block explode item -- and goes out on the very next
// tick after the charge (two block clicks on consecutive ticks are fine; the
// one-type-per-tick rule is about attacks vs block clicks). The Anchor Macro
// does the same without the shield: charge, then explode next tick from the
// same aim. A charge / explode click accepts ANY visible face of the anchor
// (top first, then the four sides, then the bottom): Grim's RotationPlace
// box for the glowstone click is the cell behind the clicked face whatever
// it contains, and a ray that reaches a face from the outside crosses that
// cell right before the hit, so the intercept holds by construction. When no
// face validates for 300 ms the anchor is abandoned and re-planned instead
// of redrawing the aim for ever.
//
// "Solid" here never means BlockState.isSolidBlock (method_26212): that is
// the redstone-conductor flag and vanilla registers glowstone (sea lanterns,
// ice, glass...) with solidBlock(Blocks::never) -- verified in the Blocks
// static initialiser -- so a glowstone shield never read as "up" and every
// combo waited the whole shield timeout. A shield is up when the spot is no
// longer replaceable (the placed block itself, by identity, when it can be
// told), a support / an existing shield is a non-replaceable full cube
// (BlockState.isFullCube = method_26234, (BlockView,BlockPos)Z on both
// versions; isOpaqueFullCube = method_26216 changes descriptor between 1.21
// and 1.21.11 and is not used).
//
// Legit modes (Auto Crystal Mode=Place / Mode=Break, the anchor macros with
// Silent Aim off) never scan and never lease: the crosshair hit
// (MinecraftClient.crosshairTarget) is the only base / crystal they know, and
// the action goes out from on_tick with the real rotation, i.e. exactly what
// a click would have done.
//
// Every intermediary name and descriptor below was checked against
// tools/genmaps/.cache/{1.21,1.21.11}/yarn_merged.tiny; none differ
// (method_26216 would, which is why isFullCube is used instead of it).
namespace vanta::modules {
namespace {
using native::Vec;
constexpr auto Entity="net/minecraft/class_1297";
constexpr auto Player="net/minecraft/class_1657";
constexpr auto Client="net/minecraft/class_310";
constexpr auto Crystal="net/minecraft/class_1511";
constexpr auto BlockState="net/minecraft/class_4970$class_4971";

using namespace crystal_geom;


// The three modules this TU registers.
enum class Variant { Crystal, Anchor, SafeAnchor };

// What the pump decided. The callback executes it, once, inside the tick.
//   Place  -- interactBlock with the crystal/anchor slot on `block`
//   Use    -- interactBlock on an existing anchor (charge or explode)
//   Attack -- attackEntity on the crystal with id `crystalId`; with `follow`
//             the aim point is drawn so the SAME ray also places the next
//             crystal on `block` (the base under the crystal) on the next tick
//   Shield -- interactBlock with the shield slot on `block` (the support)
//             through the face that leads to `spot`
// `spot` is where an anchor Place expects the anchor to appear: the air block
// behind the face of `block` the ray goes through (base + face normal).
enum class Kind { None, Place, Use, Attack, Shield };
struct Plan { Kind kind=Kind::None; Block block; Block spot; int slot=-1; int charges=0; int crystalId=-1; bool follow=false; ULONGLONG at=0; };


class Explosive : public Module {
protected:
    native::Access a;
    const Variant variant;
    const bool anchor;   // Anchor Macro or Safe Anchor: the block is an anchor, the item a block item
    const bool safe;     // Safe Anchor: the shield step
    // Auto Crystal: Mode, Min Delay, Max Delay, Only On Ground, Pause On Kill,
    // Silent (Both only). Anchor Macro: Min Delay, Max Delay, Silent Aim,
    // Auto Swap, Only Own Anchors. Safe Anchor: the Anchor Macro's plus
    // Shield Block.
    //
    // Default delays. A crystal cycle is bound by the server round trip anyway
    // (the placement waits for the crystal to appear, the attack for its
    // death), so 15..45 ms only spreads the placements enough that Vulcan's
    // FastUse never sees a fixed cadence while the cycle stays a round trip
    // long. An anchor cycle is three interactions (place, charge, explode) on
    // one block; 0..25 ms overlaps the explosion confirmation. Actual
    // interactions still require separate ticks and validated rotations.
    EnumSetting mode{this,"Mode",{"Place","Break","Both"},2};
    IntSetting minDelay{this,"Min Delay",anchor?0:15,0,1000};
    IntSetting maxDelay{this,"Max Delay",anchor?25:45,0,1000};
    BoolSetting onlyGround{this,"Only On Ground",false};
    BoolSetting pauseKill{this,"Pause On Kill",true};
    BoolSetting silent{this,anchor?"Silent Aim":"Silent",true};
    EnumSetting aimMode{this,"Aim Mode",{"Silent","Manual"},0};
    EnumSetting placementMode{this,"Placement",{"Target","Crosshair"},0};
    IntSetting targetRadius{this,"Target Radius",3,1,6};
    // Internal combat tuning; placement always prioritizes damage.
    static constexpr int kTargetRange=8;
    static constexpr int kRecoveryTime=600;
    static constexpr int kRetryDelay=100;
    size_t anchorScanCursor=0;
    Vec anchorScanTarget{}, anchorScanEye{};
    IntSetting materialReserve{this,"Material Reserve",7,0,64};
    int previousPlacementMode=-1;
    int previousTargetRadius=-1;
    int previousAimMode=-1;
    BoolSetting swap{this,"Auto Swap",true};
    BoolSetting onlyOwn{this,"Only Own Anchors",true};
    enum class ShieldBlock : int { Glowstone=0, Obsidian=1, Any=2 };
    EnumSetting shieldBlock{this,"Shield Block",{"Glowstone","Obsidian","Any"},0};
    std::mt19937 rng{std::random_device{}()};
    ULONGLONG next=0, pendingUntil=0, targetScan=0, crystalScan=0, baseScan=0, chosenUntil=0, lastAction=0, placeWaitUntil=0;
    // Two log budgets so a burst of action lines never swallows a status
    // change (the old single 5/s budget hid every idle reason behind a cycle).
    ULONGLONG statusWindow=0, actionWindow=0; int statusBudget=20, actionBudget=10;
    // `pending` is the block the anchor is expected in after a placement,
    // `pendingBase` the block it was placed against: a replaceable base (grass,
    // a snow layer) takes the anchor itself, and adoption accepts either.
    std::optional<Block> own, pending, pendingBase, working, chosen;
    int expectedCharges=-1;
    bool awaitingExplosion=false;
    bool cycleDelayStarted=false;
    ULONGLONG confirmationDeadline=0;
    jobject worldRef=nullptr, victim=nullptr, crystalRef=nullptr;
    std::string status="Ready";
    Plan plan;
    // Crystals this module already hit, by entity id. Client-side they stay
    // alive until the destroy packet lands, so they are treated as gone for
    // targeting and occupancy and are never attacked again. An entry expires
    // after a second: a crystal still standing by then survived the attack
    // (the server dropped it), and a player would click it again too.
    struct Attacked { int id; ULONGLONG at; };
    std::vector<Attacked> attacked;
    // The base the last attack's ray was also drawn to place on: the next
    // placement goes there, from the same aim point, on the next tick.
    std::optional<Block> followBase; ULONGLONG followUntil=0;
    // The aim point is drawn once per action and kept while the plan is the
    // same action, so the lease converges on one point instead of chasing a
    // new random every frame. `aimSeq` bumps after every action.
    Plan aimPlan; unsigned aimSeq=0, aimPlanSeq=~0u; Vec aimPoint; bool haveAim=false;
    float leaseYaw=0.f, leasePitch=0.f; bool haveLease=false, leaseReal=false;   // what lease() last leased (hold() reuses it)
    Plan leasePlan; unsigned leaseSeq=~0u;
    // The cached point is re-checked geometrically every 100 ms (the player
    // moves, the eye drops below a face): a point that no longer validates is
    // redrawn instead of leaving the plan waiting on it forever.
    ULONGLONG aimCheck=0;
    // Slot bookkeeping across ticks: the switch happens in the action tick,
    // the restore on a later, idle one (never in the same tick as a use).
    int restoreSlot=-1, switchedSlot=-1; unsigned actionTick=0; bool acted=false;
    // One action TYPE per tick (MultiActionsF / PacketOrderJ): the tick id of
    // the last attack and of the last block interaction; execute() refuses
    // the other kind on the same id.
    unsigned lastAttackTick=~0u, lastBlockTick=~0u;
    // The "settling" line (a block click waiting for its rotation to be on
    // the wire) at most twice a second.
    ULONGLONG settleLogAt=0;
    bool cacheLogged=false;
    // The last damage-ranked choice that was logged, so the target line goes
    // out once per new base instead of once per scan; `loggedNone` keeps the
    // "nothing found" line to one per dry streak.
    std::optional<Block> loggedBest; bool loggedNone=false;
    // The Anchor Macro's ranked choice: the base to click and the spot the
    // anchor lands in. Kept for 300 ms like `chosen`.
    struct Placement { Block base; Block spot; };
    std::optional<Placement> chosenSpot;
    // Safe Anchor's shield for the anchor `shieldAnchor`: where it goes (`at`)
    // and what it is placed against (`support`, through the face towards
    // `at`). `shieldChosen` = the choice was made (or given up) for this
    // anchor, `shieldDone` = the explode may go, `shieldPending` = placed,
    // waiting for the block to appear.
    struct ShieldPlan { Block at; Block support; };
    std::optional<ShieldPlan> shield; std::optional<Block> shieldAnchor, shieldPending;
    std::optional<Block> protectedBy;
    bool shieldChosen=false, shieldDone=false, shieldBlocked=false; ULONGLONG shieldUntil=0;
    // First moment the shield plan's support face was not reachable from the
    // eye; after 300 ms of that the shield is skipped and the charge goes.
    ULONGLONG shieldAimFail=0;
    // The translation key of the shield item that was clicked ("block.<x>"):
    // the block that appears in the spot is compared to it by its own item.
    std::string shieldKey;
    // First moment the charge aim on the current anchor found no face; after
    // 300 ms of that the anchor is abandoned and re-planned.
    ULONGLONG useAimFail=0;
    std::optional<Block> stalledAnchor;
    ULONGLONG stalledSince=0, observedAction=0;
    struct DeferredAnchor { Block block; ULONGLONG until; };
    std::vector<DeferredAnchor> deferredAnchors;
    silentaim::ActionCallbacks callbacks;

    void updateHighlight() {
        if(!anchor) return;
        // Only adopted/charged blocks are highlighted, never a planned placement.
        std::optional<Block> focus=own?own:working;
        if(focus) blockesp::set_focus_block(focus->x,focus->y,focus->z,true);
        else blockesp::set_focus_block(0,0,0,false);
    }

    // -- mode helpers ---------------------------------------------------------
    // Both is the automatic mode: it finds bases and crystals itself. Place and
    // Break act only on what the real crosshair is on and never lease.
    bool automatic() const { return anchor || mode.index()==2; }
    bool useLease() const { return automatic() && (safe?aimMode.index()==0:silent.value()); }
    bool canBreak() const { return !anchor && mode.index()!=0; }
    bool canPlace() const { return anchor || mode.index()!=1; }

    static bool budget(ULONGLONG& window,int& left,int perSecond) {
        const auto now=GetTickCount64();
        if(now-window>=1000) {window=now; left=perSecond;}
        if(left<=0) return false; --left; return true;
    }
    void setStatus(std::string s) {
        if(s==status) return; status=std::move(s);
        if(budget(statusWindow,statusBudget,20)) VLOG_INFO("explosives","{}: {}",name(),status);
    }
    bool actionLog() { return budget(actionWindow,actionBudget,10); }
    double rnd(double lo,double hi) { return std::uniform_real_distribution<double>(lo,hi)(rng); }
    void dropRefs() {
        jvm::JniScope s;
        if(s) { if(worldRef) s.get()->DeleteGlobalRef(worldRef); if(victim) s.get()->DeleteGlobalRef(victim); if(crystalRef) s.get()->DeleteGlobalRef(crystalRef); }
        worldRef=nullptr; victim=nullptr; crystalRef=nullptr;
    }
    // A new anchor (or none): the shield step starts over.
    void resetShield() { shield.reset(); shieldAnchor.reset(); shieldPending.reset(); protectedBy.reset(); shieldChosen=false; shieldDone=false; shieldBlocked=false; shieldUntil=0; shieldAimFail=0; shieldKey.clear(); }
    // restoreSlot deliberately survives: the on_tick callback puts the slot
    // back on the next tick even after a disable.
    void reset() {
        silentaim::release_action_rotation(this);
        dropRefs();
        expectedCharges=-1; awaitingExplosion=false; cycleDelayStarted=false; confirmationDeadline=0; own.reset(); pending.reset(); pendingBase.reset(); working.reset(); chosen.reset(); chosenSpot.reset();
        next=0; pendingUntil=0; targetScan=0; crystalScan=0; baseScan=0; chosenUntil=0; placeWaitUntil=0; plan=Plan{};
        attacked.clear(); followBase.reset(); followUntil=0; haveAim=false; haveLease=false; leaseReal=false; aimPlanSeq=~0u; aimCheck=0;
        loggedBest.reset(); loggedNone=false; useAimFail=0;
        anchorScanCursor=0; anchorScanTarget={}; anchorScanEye={};
        stalledAnchor.reset(); stalledSince=0; observedAction=0; deferredAnchors.clear();
        resetShield();
        if(anchor) blockesp::set_focus_block(0,0,0,false);
    }
    void expireAttacked(ULONGLONG now) { std::erase_if(attacked,[&](const Attacked& e){return now-e.at>1000;}); }
    bool wasAttacked(int id) const { return std::any_of(attacked.begin(),attacked.end(),[&](const Attacked& e){return e.id==id;}); }
    jobject bp(Block b) { return a.make("net/minecraft/class_2338","(III)V",b.x,b.y,b.z); }
    Block block(jobject b) { return {
        a.call<jint>(b,"net/minecraft/class_2382","method_10263","()I"),
        a.call<jint>(b,"net/minecraft/class_2382","method_10264","()I"),
        a.call<jint>(b,"net/minecraft/class_2382","method_10260","()I")}; }
    jobject state(jobject world,Block b) { return a.call<>(world,"net/minecraft/class_1922","method_8320","(Lnet/minecraft/class_2338;)Lnet/minecraft/class_2680;",bp(b)); }
    bool isBlock(jobject world,Block b,const char* id) {
        auto s=state(world,b);
        auto actual=a.call<>(s,BlockState,"method_26204","()Lnet/minecraft/class_2248;");
        return a.e->IsSameObject(actual,a.constant("net/minecraft/class_2246",id,"Lnet/minecraft/class_2248;"));
    }
    bool isAnchor(jobject world,Block b) { return isBlock(world,b,"field_23152"); }
    // AbstractBlockState.isAir / isReplaceable (what BlockItem.canPlace accepts
    // as the target) / isFullCube (the collision shape fills the cell: a face
    // a placement can be clicked against, a wall the blast cannot pass).
    // Never isSolidBlock (method_26212), the redstone-conductor flag that is
    // false for glowstone -- see the header.
    bool air(jobject world,Block b) { return a.call<jboolean>(state(world,b),BlockState,"method_26215","()Z"); }
    bool replaceable(jobject world,Block b) { auto s=state(world,b); return a.call<jboolean>(s,BlockState,"method_26215","()Z") || a.call<jboolean>(s,BlockState,"method_45474","()Z"); }
    bool fullCube(jobject world,Block b) { return a.call<jboolean>(state(world,b),BlockState,"method_26234","(Lnet/minecraft/class_1922;Lnet/minecraft/class_2338;)Z",world,bp(b)); }
    // Something stands in the cell and fills it: what an existing shield is.
    bool filled(jobject world,Block b) { return !replaceable(world,b) && fullCube(world,b); }
    // The translation key of the item a placed block drops to ("block.<x>"
    // for every block with an item): Block.asItem().getTranslationKey().
    std::string blockKey(jobject world,Block b) {
        auto block=a.call<>(state(world,b),BlockState,"method_26204","()Lnet/minecraft/class_2248;");
        auto i=a.call<>(block,"net/minecraft/class_2248","method_8389","()Lnet/minecraft/class_1792;");
        if(!i) return {};
        return jvm::toStdString(a.e,static_cast<jstring>(a.call<>(i,"net/minecraft/class_1792","method_7876","()Ljava/lang/String;")));
    }
    // Is the shield up at `spot`: the very block that was clicked in (by
    // identity through its item key), or anything else that is no longer
    // replaceable -- client prediction puts the block there the moment the
    // click goes out, a server rejection shows as it vanishing again.
    // `what` names it for the log.
    bool shieldUp(jobject world,Block spot,std::string& what) {
        if(!filled(world,spot)) return false;
        const std::string key=blockKey(world,spot);
        const bool same=!shieldKey.empty() && key==shieldKey;
        const auto dot=key.rfind('.');
        what=same?key.substr(dot==std::string::npos?0:dot+1):std::string("another block: ")+(key.empty()?std::string("?"):key.substr(dot==std::string::npos?0:dot+1));
        return true;
    }
    // A block the shield click may land on: non-replaceable, a full cube
    // (its face is a real face), not a respawn anchor (its use handler runs
    // before the placement: obsidian explodes a charged one, glowstone
    // charges it) and without a block entity (chests, furnaces and crafting
    // tables open their screen instead of placing).
    bool support(jobject world,Block b) {
        if(replaceable(world,b) || isAnchor(world,b) || !fullCube(world,b)) return false;
        return a.call<>(world,"net/minecraft/class_1922","method_8321","(Lnet/minecraft/class_2338;)Lnet/minecraft/class_2586;",bp(b))==nullptr;
    }
    Vec top(Block b) { return {b.x+.5,b.y+1.0,b.z+.5}; }
    jobject vec(Vec p) { return a.make("net/minecraft/class_243","(DDD)V",p.x,p.y,p.z); }
    Vec vec3(jobject v) {
        auto c=a.cls("net/minecraft/class_243");
        Vec r{a.e->GetDoubleField(v,a.field(c,"field_1352","D")),a.e->GetDoubleField(v,a.field(c,"field_1351","D")),a.e->GetDoubleField(v,a.field(c,"field_1350","D"))}; a.check(); return r;
    }
    jobject ray(jobject world,jobject player,Vec eye,Vec end) {
        auto shape=a.constant("net/minecraft/class_3959$class_3960","field_17559","Lnet/minecraft/class_3959$class_3960;");
        auto fluid=a.constant("net/minecraft/class_3959$class_242","field_1348","Lnet/minecraft/class_3959$class_242;");
        auto context=a.make("net/minecraft/class_3959","(Lnet/minecraft/class_243;Lnet/minecraft/class_243;Lnet/minecraft/class_3959$class_3960;Lnet/minecraft/class_3959$class_242;Lnet/minecraft/class_1297;)V",vec(eye),vec(end),shape,fluid,player);
        return a.call<>(world,"net/minecraft/class_1922","method_17742","(Lnet/minecraft/class_3959;)Lnet/minecraft/class_3965;",context);
    }
    bool blockHit(jobject hit) {
        if(!hit || !a.e->IsInstanceOf(hit,a.cls("net/minecraft/class_3965"))) return false;
        auto type=a.call<>(hit,"net/minecraft/class_239","method_17783","()Lnet/minecraft/class_239$class_240;");
        return a.call<jint>(type,"java/lang/Enum","ordinal","()I")==1;
    }
    Block hitBlock(jobject hit) { return block(a.call<>(hit,"net/minecraft/class_3965","method_17777","()Lnet/minecraft/class_2338;")); }
    jobject hitSide(jobject hit) { return a.call<>(hit,"net/minecraft/class_3965","method_17780","()Lnet/minecraft/class_2350;"); }
    Vec hitPos(jobject hit) { return vec3(a.call<>(hit,"net/minecraft/class_239","method_17784","()Lnet/minecraft/class_243;")); }
    Vec normal(jobject side) { return {
        double(a.call<jint>(side,"net/minecraft/class_2350","method_10148","()I")),
        double(a.call<jint>(side,"net/minecraft/class_2350","method_10164","()I")),
        double(a.call<jint>(side,"net/minecraft/class_2350","method_10165","()I"))}; }
    Vec velocity(jobject player) { return vec3(a.call<>(player,Entity,"method_18798","()Lnet/minecraft/class_243;")); }
    int entityId(jobject entity) { return a.call<jint>(entity,Entity,"method_5628","()I"); }
    bool alive(jobject entity) { return a.call<jboolean>(entity,Entity,"method_5805","()Z"); }
    bool onGround(jobject player) { return a.call<jboolean>(player,Entity,"method_24828","()Z"); }
    bool sneaking(jobject player) { return a.call<jboolean>(player,Entity,"method_5715","()Z"); }
    float yawOf(jobject player) { return a.call<jfloat>(player,Entity,"method_36454","()F"); }
    float pitchOf(jobject player) { return a.call<jfloat>(player,Entity,"method_36455","()F"); }
    double blockReach(jobject player) { return std::min(5.0,a.call<jdouble>(player,Player,"method_55754","()D")); }
    double entityReach(jobject player) { return a.call<jdouble>(player,Player,"method_55755","()D"); }
    Box6 boxOf(jobject entity) {
        auto box=a.call<>(entity,Entity,"method_5829","()Lnet/minecraft/class_238;");
        auto c=a.cls("net/minecraft/class_238");
        auto f=[&](const char* id){return a.e->GetDoubleField(box,a.field(c,id,"D"));};
        Box6 b{f("field_1323"),f("field_1322"),f("field_1321"),f("field_1320"),f("field_1325"),f("field_1324")}; a.check();
        a.e->DeleteLocalRef(box); return b;
    }
    // Closest point of the box to the eye; the centre when the eye is inside
    // (an end crystal's box is 2x2x2, so an adjacent player IS inside it, and
    // atan2(0,0) of eye-minus-eye was the yaw 0 / pitch 0 snap).
    static Vec aimInBox(const Box6& b,Vec eye) {
        if(eye.x>b.minX && eye.x<b.maxX && eye.y>b.minY && eye.y<b.maxY && eye.z>b.minZ && eye.z<b.maxZ)
            return {(b.minX+b.maxX)*.5,(b.minY+b.maxY)*.5,(b.minZ+b.maxZ)*.5};
        return {std::clamp(eye.x,b.minX+.02,b.maxX-.02),std::clamp(eye.y,b.minY+.02,b.maxY-.02),std::clamp(eye.z,b.minZ+.02,b.maxZ-.02)};
    }
    // A random point inside the crystal box, near the face that faces the eye:
    // the closest point pushed 0.15..0.45 into the box along each axis that is
    // clamped, so no two attacks on the same crystal share an angle (Vulcan's
    // exact-target repetition) while the ray still enters well inside the box.
    Vec randomInBox(const Box6& b,Vec eye) {
        Vec p=aimInBox(b,eye);
        auto jitter=[&](double v,double lo,double hi){ const double in=rnd(.15,.45); if(v<=lo+.03) return lo+in; if(v>=hi-.03) return hi-in; return std::clamp(v+rnd(-.3,.3),lo+.1,hi-.1); };
        return {jitter(p.x,b.minX,b.maxX),jitter(p.y,b.minY,b.maxY),jitter(p.z,b.minZ,b.maxZ)};
    }
    // Crystal: obsidian or bedrock with air above. Anchor: any solid block
    // that is not itself a respawn anchor (clicking one with a non-charge item
    // runs its use handler: a charged anchor explodes outside the Nether) --
    // which of its faces is used, and whether the block behind that face is
    // free, is the spot's business (aimAtAnchorFace / placeValid).
    bool validBase(jobject world,Block b) {
        if(anchor) return !air(world,b) && !isAnchor(world,b);
        return (isBlock(world,b,"field_10540") || isBlock(world,b,"field_9987")) && air(world,{b.x,b.y+1,b.z});
    }
    // Vanilla EndCrystalItem.useOnBlock: air above the base and no entity in the
    // 1x2x1 box above it, the player included. Nothing about block collision --
    // the old isSpaceEmpty test refused every roofed crystal spot. Crystals
    // this module already attacked (and `ignoreId`, the one it is attacking in
    // this very plan) do not count: the server has them dead by the time the
    // placement arrives. `strict` counts them anyway -- the client's own view,
    // used to tell a rejected placement from one the client merely could not
    // see succeed.
    bool spaceFree(jobject world,Block b,int ignoreId,bool strict=false) {
        auto box=a.make("net/minecraft/class_238","(DDDDDD)V",double(b.x),double(b.y+1),double(b.z),double(b.x+1),double(b.y+3),double(b.z+1));
        auto occupying=a.call<>(world,"net/minecraft/class_1924","method_8335","(Lnet/minecraft/class_1297;Lnet/minecraft/class_238;)Ljava/util/List;",static_cast<jobject>(nullptr),box);
        if(box) a.e->DeleteLocalRef(box);
        if(!occupying) return true;
        const int n=a.call<jint>(occupying,"java/util/List","size","()I");
        bool free=true;
        for(int i=0;i<n;++i) {
            auto e=a.call<>(occupying,"java/util/List","get","(I)Ljava/lang/Object;",i);
            const bool crystal=a.e->IsInstanceOf(e,a.cls(Crystal));
            const int id=crystal?entityId(e):-1;
            a.e->DeleteLocalRef(e);
            if(crystal && !strict && (id==ignoreId || wasAttacked(id))) continue;
            free=false;
            break;
        }
        a.e->DeleteLocalRef(occupying);
        return free;
    }
    // Where to aim on a block from the current eye: a RANDOM point on the top
    // face (0.2..0.8 of the face, nudged 0.01 into the block so the ray
    // certainly crosses the face), verified by the raycast; when the top face
    // is not wanted or not visible, a random interior point reached through
    // whichever face faces the eye. Nothing when no ray reaches the block.
    // Randomising the point per action is what keeps two placements on the
    // same base from ever sending the same rotation (Vulcan repetition).
    std::optional<Vec> aimAtBlock(jobject world,jobject player,Vec eye,Block b,bool topOnly) {
        for(int i=0;i<3;++i) {
            native::Frame f(a.e);
            const Vec p{b.x+rnd(.2,.8),b.y+1-.01,b.z+rnd(.2,.8)};
            auto hit=ray(world,player,eye,p);
            if(!blockHit(hit) || !(hitBlock(hit)==b) || normal(hitSide(hit)).y<.5) continue;
            return p;
        }
        if(topOnly) return {};
        for(int i=0;i<3;++i) {
            native::Frame f(a.e);
            const Vec p{b.x+rnd(.15,.85),b.y+rnd(.15,.85),b.z+rnd(.15,.85)};
            auto hit=ray(world,player,eye,p);
            if(!blockHit(hit) || !(hitBlock(hit)==b)) continue;
            return p;
        }
        return {};
    }
    // Where to aim for a Use on the anchor `b` (the charge, or the explode):
    // ANY face the eye reaches, tried in the order top, the four sides,
    // bottom; the first whose ray validates wins and `face` names it. What
    // the cell behind the face contains does not matter: Grim's RotationPlace
    // box for the glowstone click is that cell whatever is in it, and a ray
    // that reaches the face from outside crosses the cell right before the
    // hit (or starts inside it), so the intercept holds by construction --
    // requiring a replaceable far cell only left a player standing beside
    // the anchor with no face at all. Bounded: 3 + 5 * 2 rays.
    std::optional<Vec> aimAtUseFace(jobject world,jobject player,jobject inv,Vec eye,const Plan& usePlan,Vec& face) {
        // Try the face requiring the least turn from the current lease first.
        // Always preferring the top wastes a large pitch turn after shielding.
        struct FaceChoice { Vec normal; double turn; };
        std::vector<FaceChoice> choices;
        const float fromYaw=haveLease?leaseYaw:yawOf(player);
        const float fromPitch=haveLease?leasePitch:pitchOf(player);
        for(const Vec& n : kFaces) {
            const Vec centre{usePlan.block.x+.5+n.x*.49,usePlan.block.y+.5+n.y*.49,usePlan.block.z+.5+n.z*.49};
            float yaw=0,pitch=0;
            if(!anglesTo(eye,centre,yaw,pitch)) continue;
            const double dy=std::remainder(double(yaw-fromYaw),360.0), dp=pitch-fromPitch;
            choices.push_back({n,dy*dy+dp*dp});
        }
        std::stable_sort(choices.begin(),choices.end(),[](const FaceChoice& l,const FaceChoice& r){return l.turn<r.turn;});
        for(const auto& choice : choices) {
            const Vec n=choice.normal;
            if(auto p=aimAtFace(world,player,eye,usePlan.block,n,5,inv,&usePlan)) { face=n; return p; }
        }
        return {};
    }
    // Grim's PRE-flying RotationPlace pass, as it will run when this tick's
    // placement packet arrives: from the position of the previous flying
    // (the current eye at the pre-tick point; one tick of velocity back as
    // well when moving) a ray along (yaw,pitch), (lastYaw,pitch) or
    // (lastYaw,lastPitch) -- the wire rotation and the one before it --
    // intercepts the placed cell's box within the block reach. Grim accepts
    // ANY of the three; `combo` names the one that hit. A missing prev is
    // only the first combo.
    bool preFlyingHits(const silentaim::ActionContext& c,Vec eye,Vec vel,bool moving,const Box6& box,double reach,const char*& combo) {
        struct Look { float yaw,pitch; const char* name; };
        const Look looks[3]={{c.wire_yaw,c.wire_pitch,"wire yaw/pitch"},{c.wire_prev_yaw,c.wire_pitch,"prev yaw/wire pitch"},{c.wire_prev_yaw,c.wire_prev_pitch,"prev yaw/pitch"}};
        const int n=c.wire_have_prev?3:1;
        for(int i=0;i<n;++i) {
            const Vec d=dirOf(looks[i].yaw,looks[i].pitch); double t=0;
            if(!rayEntersBox(eye,d,box,reach,t) || t>reach-.05) continue;
            if(moving) { double t2=0; if(!rayEntersBox(add(eye,vel,-1.0),d,box,reach,t2) || t2>reach-.05) continue; }
            combo=looks[i].name; return true;
        }
        combo="none"; return false;
    }
    // The block on the far side of `b`'s face with outward normal `n`: where a
    // block placed against that face appears (BlockItem.place: hit pos offset
    // by the side, unless the clicked block is replaceable).
    static Block neighbour(Block b,Vec n) { return {b.x+static_cast<int>(std::lround(n.x)),b.y+static_cast<int>(std::lround(n.y)),b.z+static_cast<int>(std::lround(n.z))}; }
    static Vec faceNormal(Block base,Block spot) { return {double(spot.x-base.x),double(spot.y-base.y),double(spot.z-base.z)}; }
    static bool sameNormal(Vec p,Vec q) { return p.x==q.x && p.y==q.y && p.z==q.z; }
    static const char* faceName(Vec n) { return n.y>0?"top":n.y<0?"bottom":n.x>0?"east":n.x<0?"west":n.z>0?"south":"north"; }
    // A random point on the face of `b` with outward normal `n` (0.2..0.8 of
    // the face, nudged 0.01 into the block), verified by the raycast: the ray
    // has to hit `b` through that very face. Up to `tries` draws, one ray each.
    std::optional<Vec> aimAtFace(jobject world,jobject player,Vec eye,Block b,Vec n,int tries,jobject inv=nullptr,const Plan* validation=nullptr) {
        for(int i=0;i<tries;++i) {
            native::Frame f(a.e);
            Vec p{b.x+rnd(.2,.8),b.y+rnd(.2,.8),b.z+rnd(.2,.8)};
            if(validation && i==0) p={b.x+.5,b.y+.5,b.z+.5};
            if(n.x!=0) p.x=b.x+(n.x>0?1-.01:.01);
            if(n.y!=0) p.y=b.y+(n.y>0?1-.01:.01);
            if(n.z!=0) p.z=b.z+(n.z>0?1-.01:.01);
            auto hit=ray(world,player,eye,p);
            if(!blockHit(hit) || !(hitBlock(hit)==b) || !sameNormal(normal(hitSide(hit)),n)) continue;
            if(validation) {
                float yaw=0,pitch=0;
                if(!anglesTo(eye,p,yaw,pitch)) continue;
                const Vec vel=velocity(player);
                const bool moving=vel.x*vel.x+vel.y*vel.y+vel.z*vel.z>1e-8;
                if(!blockValid(world,player,inv,eye,dirOf(yaw,pitch),vel,moving,*validation)) continue;
            }
            return p;
        }
        return {};
    }
    static constexpr Vec kFaces[6]={{0,1,0},{1,0,0},{-1,0,0},{0,0,1},{0,0,-1},{0,-1,0}};
    // Where to aim for an anchor placement against `b`: the face that leads to
    // `spot` first, then every other face whose far side is air and free of
    // entities, in the order top, the four sides, bottom. `spot` is updated to
    // the face that worked so the plan and the aim agree on where the anchor
    // will appear. Bounded: 3 + 5 * 2 rays at most, once per action (the aim
    // is cached while the plan stands).
    std::optional<Vec> aimAtAnchorFace(jobject world,jobject player,Vec eye,Block b,Block& spot) {
        const Vec first=faceNormal(b,spot);
        if(auto p=aimAtFace(world,player,eye,b,first,3)) return p;
        for(const Vec& n : kFaces) {
            if(sameNormal(n,first)) continue;
            const Block s=neighbour(b,n);
            if(!air(world,s) || !entityFree(world,s)) continue;
            if(auto p=aimAtFace(world,player,eye,b,n,2)) { spot=s; return p; }
        }
        return {};
    }
    // The block under the REAL crosshair when it is a usable crystal base
    // within reach. Free: no scan, no ray. This is the ONLY source of a base
    // in Auto Crystal's Place mode.
    std::optional<Block> crosshairBase(jobject mc,jobject world,Vec eye,double reach) {
        auto hit=a.get(mc,Client,"field_1765","Lnet/minecraft/class_239;");
        if(!blockHit(hit)) return {};
        auto b=hitBlock(hit);
        if(native::distance2(eye,top(b))<=reach*reach && validBase(world,b)) return b;
        return {};
    }
    // The Anchor Macro's crosshair source: an existing anchor under the real
    // crosshair (`existing`), or any solid block whose looked-at face has an
    // air block behind it -- the anchor goes into that block, exactly where a
    // click would put it, top face or side face alike. The hit point has to
    // be within the block reach. Free: no scan, no ray.
    std::optional<Block> crosshairAnchor(jobject mc,jobject world,Vec eye,double reach,Block& spot,bool& existing) {
        existing=false;
        auto hit=a.get(mc,Client,"field_1765","Lnet/minecraft/class_239;");
        if(!blockHit(hit)) return {};
        const Block b=hitBlock(hit);
        if(native::distance2(eye,hitPos(hit))>reach*reach) return {};
        if(isAnchor(world,b)) { existing=true; return b; }
        if(air(world,b)) return {};
        const Block s=neighbour(b,normal(hitSide(hit)));
        if(!air(world,s)) return {};
        spot=s; return b;
    }
    // Auto Crystal's base. The crosshair block is checked every frame; the
    // scan behind it (Both + Silent only -- Place and Break never get here)
    // runs at most every 150 ms, ranks the offsets in pure C++ first, and
    // raycasts only the few that survive the state checks. With a victim in
    // range the candidates are ordered by blast::estimate (the distance-only
    // damage a crystal on them deals to the victim's feet; higher first, the
    // eye distance breaks ties), so the FIRST candidate that is a valid base
    // with a face the eye reaches is the best one and the loop stops there.
    // Any positive damage counts: the player decides whether a crystal is
    // worth it. Without a victim the old look-direction ranking stands. Caps:
    // 48 candidates asked, 8 rays.
    std::optional<Block> chooseBase(jobject mc,jobject world,jobject player,Vec eye,double reach) {
        if(auto b=crosshairBase(mc,world,eye,reach)) return b;
        if(!useLease()) return {};
        const auto now=GetTickCount64();
        if(chosen && now<chosenUntil && native::distance2(eye,top(*chosen))<=reach*reach && validBase(world,*chosen)) return chosen;
        if(now<baseScan) return {};
        baseScan=now+150; chosen.reset();
        const bool haveVictim=victim && alive(victim);
        const Vec vpos=haveVictim?native::position(a,victim):Vec{};
        const Vec dir=dirOf(yawOf(player),pitchOf(player));
        struct Candidate { Block b; double rank; double d2; double damage; };
        std::vector<Candidate> candidates; candidates.reserve(256);
        const int r=static_cast<int>(std::ceil(reach));
        const Block origin{static_cast<int>(std::floor(eye.x)),static_cast<int>(std::floor(eye.y)),static_cast<int>(std::floor(eye.z))};
        for(int x=-r;x<=r;++x) for(int z=-r;z<=r;++z) for(int dy=-r;dy<=r;++dy) {
            Block b{origin.x+x,origin.y+dy,origin.z+z};
            const Vec point=top(b); const double d2=native::distance2(eye,point); if(d2>reach*reach || d2<.01) continue;
            const double d=std::sqrt(d2);
            if(haveVictim) {
                const double damage=blast::estimate(blast::crystalCentre(b.x,b.y,b.z),blast::kCrystalPower,vpos);
                if(damage<=0.0) continue;   // beyond radius 12: nothing to gain
                candidates.push_back({b,-damage,d2,damage});
            } else {
                const double dot=((point.x-eye.x)*dir.x+(point.y-eye.y)*dir.y+(point.z-eye.z)*dir.z)/d;
                candidates.push_back({b,(1-dot)*4+d*.1,d2,0.0});
            }
        }
        std::sort(candidates.begin(),candidates.end(),[](const Candidate& l,const Candidate& c){return l.rank<c.rank || (l.rank==c.rank && l.d2<c.d2);});
        int checked=0, rays=0;
        for(const auto& c : candidates) {
            if(checked>=48 || rays>=8) break;
            native::Frame frame(a.e); ++checked;
            if(!validBase(world,c.b)) continue;
            ++rays;
            if(!aimAtBlock(world,player,eye,c.b,false)) continue;
            chosen=c.b; chosenUntil=now+300; loggedNone=false;
            if(haveVictim && (!loggedBest || !(*loggedBest==c.b))) {
                loggedBest=c.b;
                VLOG_INFO("explosives","{}: target {} at {}, best base {} {} {}, expected damage {} (checked {}, rays {})",name(),targetName(victim),std::sqrt(native::distance2(eye,hitPoint(victim,eye))),c.b.x,c.b.y,c.b.z,c.damage,checked,rays);
            }
            return chosen;
        }
        if(haveVictim && !loggedNone) { loggedNone=true; loggedBest.reset(); VLOG_INFO("explosives","{}: no crystal base for target {}: {} candidates, checked {}, rays {}",name(),targetName(victim),candidates.size(),checked,rays); }
        return {};
    }
    std::string item(jobject inv,int slot) {
        auto stack=a.call<>(inv,"net/minecraft/class_1263","method_5438","(I)Lnet/minecraft/class_1799;",slot);
        auto i=a.call<>(stack,"net/minecraft/class_1799","method_7909","()Lnet/minecraft/class_1792;");
        return jvm::toStdString(a.e,static_cast<jstring>(a.call<>(i,"net/minecraft/class_1792","method_7876","()Ljava/lang/String;")));
    }
    bool empty(jobject inv,int slot) {
        auto stack=a.call<>(inv,"net/minecraft/class_1263","method_5438","(I)Lnet/minecraft/class_1799;",slot);
        return !stack || a.call<jboolean>(stack,"net/minecraft/class_1799","method_7960","()Z");
    }
    int selected(jobject inv) { auto f=a.field(a.cls("net/minecraft/class_1661"),"field_7545","I"); int i=a.e->GetIntField(inv,f); a.check(); return i; }
    // Only the field. interactBlock/attackEntity call syncSelectedSlot themselves
    // as their first instruction, so the UpdateSelectedSlot goes out immediately
    // before the interaction -- the vanilla order (PacketOrderE wants the slot
    // change FIRST in its tick; a slot change after an attack or a use is the
    // flag).
    void setSlot(jobject inv,int slot) { auto f=a.field(a.cls("net/minecraft/class_1661"),"field_7545","I"); a.e->SetIntField(inv,f,slot); a.check(); }
    // The restore, on a later tick: field plus an explicit sync so the packet
    // precedes this tick's flying (what interactionManager.tick does). Only
    // ever called from the pre-tick point of an idle tick -- see execute().
    void restore(jobject inv,jobject manager) {
        if(restoreSlot>=0 && selected(inv)==switchedSlot) { setSlot(inv,restoreSlot); a.call<void>(manager,"net/minecraft/class_636","method_2911","()V"); }
        restoreSlot=-1; switchedSlot=-1;
    }
    static bool safeForExplode(const std::string& key) { return !isGlowstone(key) && !isAnchorItem(key); }
    // First hotbar slot holding an item with `suffix`, the held slot first.
    // The anchor macros with Auto Swap off only ever look at the held slot.
    int findSlot(jobject inv,const std::string& suffix) {
        const int active=selected(inv);
        for(int n=0;n<9;++n) {
            const int i=(active+n)%9; if(anchor && !swap.value() && i!=active) continue;
            if(!empty(inv,i) && item(inv,i).ends_with(suffix)) return i;
        }
        return -1;
    }
    bool isShieldItem(const std::string& k) {
        switch(shieldBlock.asEnum<ShieldBlock>()) {
            case ShieldBlock::Glowstone: return isGlowstone(k);
            case ShieldBlock::Obsidian:  return isObsidian(k);
            default:                     return isFullBlock(k);
        }
    }
    // The shield item: the held slot first (no slot packet at all), then the
    // hotbar (held slot only with Auto Swap off, like every other item here).
    int shieldSlot(jobject inv) {
        const int active=selected(inv);
        for(int n=0;n<9;++n) {
            const int i=(active+n)%9; if(!swap.value() && i!=active) continue;
            if(!empty(inv,i) && isShieldItem(item(inv,i))) return i;
        }
        return -1;
    }
    int glowstoneCount(jobject inv) {
        int total=0;
        const int active=selected(inv);
        for(int i=0;i<9;++i) {
            if(!swap.value() && i!=active) continue;
            native::Frame frame(a.e);
            if(empty(inv,i) || !isGlowstone(item(inv,i))) continue;
            auto stack=a.call<>(inv,"net/minecraft/class_1263","method_5438","(I)Lnet/minecraft/class_1799;",i);
            total+=a.call<jint>(stack,"net/minecraft/class_1799","method_7947","()I");
        }
        return total;
    }
    int hotbarMaterialCount(jobject inv,int kind) {
        int total=0;
        const int active=selected(inv);
        for(int i=0;i<9;++i) {
            if(!swap.value() && i!=active) continue;
            if(empty(inv,i)) continue;
            const std::string key=item(inv,i);
            const bool match=kind==0?isAnchorItem(key):kind==1?isGlowstone(key):isShieldItem(key);
            if(!match) continue;
            auto stack=a.call<>(inv,"net/minecraft/class_1263","method_5438","(I)Lnet/minecraft/class_1799;",i);
            total+=a.call<jint>(stack,"net/minecraft/class_1799","method_7947","()I");
        }
        return total;
    }
    bool combatTargetInRange(Vec eye) {
        return useLease() && victim && alive(victim) &&
            native::distance2(eye,hitPoint(victim,eye))<=double(kTargetRange*kTargetRange);
    }
    bool reserveAllows(jobject inv,Vec eye,int kind,int needed=1) {
        if(!useLease() || materialReserve.value()<=0) return true;
        // A live target in the automatic combat radius authorizes spending the
        // reserve for this attack. The reserve only prevents idle/wasted cycles.
        if(combatTargetInRange(eye)) return true;
        return hotbarMaterialCount(inv,kind)>materialReserve.value()+needed-1;
    }
    bool canStartAnchor(jobject inv,Vec eye) {
        if(!reserveAllows(inv,eye,0)) return false;
        if(!safe) return true;
        const int shield=shieldSlot(inv);
        if(shield<0 || !reserveAllows(inv,eye,2)) return false;
        // Glowstone shield + glowstone charge consumes two stacks/items
        // before the next explosion, so both must remain above the reserve.
        const int glowNeeded=isGlowstone(item(inv,shield))?2:1;
        return reserveAllows(inv,eye,1,glowNeeded);
    }
    // The item that explodes a charged anchor: anything but glowstone (which
    // would add a charge) or an anchor (which would place one). A NON-BLOCK
    // item is preferred (sword, totem, tool, food...): Grim runs no place
    // check on it at all. Block items come next (the RotationPlace rule is
    // then enforced by placeValid), the empty hand last; within a rank the
    // held slot wins so the explode usually costs no slot packet. A full
    // anchor (4 charges) explodes with glowstone too, so it is the fallback.
    int explodeSlot(jobject inv,int charges) {
        const int active=selected(inv);
        int best=-1, bestRank=99;
        for(int n=0;n<9;++n) {
            const int i=(active+n)%9; if(!swap.value() && i!=active) continue;
            const std::string key=item(inv,i);
            if(!safeForExplode(key)) continue;
            const int rank=empty(inv,i)?2:blockItem(key)?1:0;
            if(rank<bestRank) { best=i; bestRank=rank; if(rank==0) break; }
        }
        if(best>=0) return best;
        if(charges>=4 && !isAnchorItem(item(inv,active)) &&
           (isGlowstone(item(inv,active)) ? hotbarMaterialCount(inv,1)>materialReserve.value() : true)) return active;
        return -1;
    }
    // Delay between consecutive cycles: a uniform draw between Min and Max
    // Delay (Min alone when the pair is reversed). Attacks never wait for it;
    // placements do.
    void cooldown() {
        const int lo=minDelay.value(), hi=maxDelay.value();
        const int ms=hi>lo?std::uniform_int_distribution<int>(lo,hi)(rng):lo;
        next=GetTickCount64()+static_cast<ULONGLONG>(std::max(ms,0));
    }
    Vec hitPoint(jobject entity,Vec eye) { return aimInBox(boxOf(entity),eye); }
    // Nearest alive entity of `type` by closest box point; crystals already
    // attacked are invisible here, and so is a player on the friend list
    // (asked last, like the aura does: it costs JNI only for the few that
    // survive the class, life and distance tests).
    jobject nearest(jobject world,jobject player,Vec eye,double reach,const char* type,bool skipAttacked) {
        auto list=a.call<>(world,"net/minecraft/class_638","method_18112","()Ljava/lang/Iterable;");
        if(!list) return nullptr;
        auto it=a.call<>(list,"java/lang/Iterable","iterator","()Ljava/util/Iterator;");
        a.e->DeleteLocalRef(list);
        if(!it) return nullptr;
        const bool players=std::string_view(type)==Player;
        jobject best=nullptr; double distance=reach*reach;
        while(a.call<jboolean>(it,"java/util/Iterator","hasNext","()Z")) {
            auto entity=a.call<>(it,"java/util/Iterator","next","()Ljava/lang/Object;");
            if(a.e->IsInstanceOf(entity,a.cls(type)) && !a.e->IsSameObject(entity,player) && alive(entity) && !(skipAttacked && wasAttacked(entityId(entity)))) {
                double d=native::distance2(eye,hitPoint(entity,eye));
                if(d<distance && !(players && social::isFriend(a.e,entity))) { if(best) a.e->DeleteLocalRef(best); best=a.e->NewLocalRef(entity); distance=d; }
            }
            a.e->DeleteLocalRef(entity);
        }
        a.e->DeleteLocalRef(it);
        return best;
    }
    // Entity.getName().getString(): the player's display name for the log.
    std::string targetName(jobject entity) {
        auto text=a.call<>(entity,Entity,"method_5477","()Lnet/minecraft/class_2561;");
        auto s=static_cast<jstring>(a.call<>(text,"net/minecraft/class_2561","getString","()Ljava/lang/String;"));
        return s?jvm::toStdString(a.e,s):std::string("?");
    }
    // No entity at all inside the block `spot`. A full block cannot be placed
    // where any entity's box overlaps it: BlockItem.canPlace -> World.canPlace
    // -> doesNotIntersectEntities, on the client AND the server, and the local
    // player is an entity like any other there -- standing half over the edge
    // of a block blocks a placement into it. So the local player is NOT
    // exempted (a spot the player's box touches is rejected by the server
    // anyway); the rankings skip such spots in pure C++ before asking here.
    bool entityFree(jobject world,Block spot) {
        auto box=a.make("net/minecraft/class_238","(DDDDDD)V",double(spot.x),double(spot.y),double(spot.z),double(spot.x+1),double(spot.y+1),double(spot.z+1));
        auto occupying=a.call<>(world,"net/minecraft/class_1924","method_8335","(Lnet/minecraft/class_1297;Lnet/minecraft/class_238;)Ljava/util/List;",static_cast<jobject>(nullptr),box);
        return a.call<jint>(occupying,"java/util/List","size","()I")==0;
    }
    static bool overlaps(const Box6& b,Block s) { return b.maxX>s.x && b.minX<s.x+1 && b.maxY>s.y && b.minY<s.y+1 && b.maxZ>s.z && b.minZ<s.z+1; }
    // Can a block go into `b` right now: air or replaceable, no entity inside
    // (the player included -- World.canPlace refuses a cube that cuts any
    // entity's box).
    bool placeable(jobject world,const Box6& self,Block b) { return replaceable(world,b) && !overlaps(self,b) && entityFree(world,b); }
    // Anchor Macro with a target: the spot (an air block the anchor can occupy)
    // whose explosion hurts `target` most, and a face of a solid neighbour the
    // eye reaches to place it through. Every block around the eye is scored in
    // pure C++ by blast::estimate (the distance-only damage from the anchor
    // centre to the target's feet; monotonic in that distance, so the sort IS
    // the damage ranking, the eye distance breaks ties); nothing beyond radius
    // 10 hurts and is skipped, and so is any block the local player's own box
    // overlaps (the server refuses a block placed into an entity, us
    // included). The survivors are then asked, best first, whether the world
    // agrees: the spot is air and free of entities, and a face of a solid
    // neighbour -- the top face of the block below first (the usual floor
    // placement), then the four sides, then the underside of the block above,
    // each within the block reach and with the eye on its outer side -- is hit
    // by a ray from the eye. The FIRST spot that passes is the best one; the
    // loop stops there. Any positive damage counts. Caps: 48 spots asked, 8
    // face rays (2 draws each). The choice is kept for 300 ms so the lease
    // converges on one base.
    bool deferredAnchor(Block b,ULONGLONG now) const {
        return std::any_of(deferredAnchors.begin(),deferredAnchors.end(),[&](const DeferredAnchor& d){return d.block==b && now<d.until;});
    }
    void releaseDistantAnchor(Block b,ULONGLONG now) {
        if(deferredAnchors.size()>=16) deferredAnchors.erase(deferredAnchors.begin());
        deferredAnchors.push_back({b,now+1500});
        own.reset(); working.reset(); pending.reset(); pendingBase.reset();
        expectedCharges=-1; awaitingExplosion=false; cycleDelayStarted=false;
        pendingUntil=0; confirmationDeadline=0; resetShield();
        stalledAnchor.reset(); stalledSince=0; useAimFail=0;
        chosen.reset(); chosenSpot.reset(); chosenUntil=0; baseScan=0; targetScan=0;
        plan=Plan{}; haveAim=false; haveLease=false; leaseReal=false;
        aimPlanSeq=~0u; aimCheck=0;
        // Slot restoration remains owned by the normal idle tick callback.
        silentaim::release_action_rotation(this);
        setStatus("Anchor out of reach: choosing a new target");
        if(actionLog()) VLOG_INFO("explosives","{}: released distant anchor at {}",name(),at(b));
    }
    std::optional<Placement> chooseAnchorSpot(jobject world,jobject player,Vec eye,double reach,jobject target) {
        const auto now=GetTickCount64();
        const Vec tpos=native::position(a,target);
        const bool moved=native::distance2(tpos,anchorScanTarget)>.25 || native::distance2(eye,anchorScanEye)>.25;
        if(moved) { chosenSpot.reset(); anchorScanCursor=0; baseScan=0; }
        if(chosenSpot && now<chosenUntil && anchorInReach(chosenSpot->spot,eye,reach) &&
           air(world,chosenSpot->spot) && entityFree(world,chosenSpot->spot) && support(world,chosenSpot->base)) return chosenSpot;
        if(now<baseScan) return {};
        baseScan=now+50; chosenSpot.reset();
        anchorScanTarget=tpos; anchorScanEye=eye;
        const Box6 self=boxOf(player);
        struct Candidate { Block s; double damage; double d2; };
        std::vector<Candidate> candidates; candidates.reserve(256);
        const int r=std::min(static_cast<int>(std::ceil(reach))+1, targetRadius.value()+2);
        const Block origin{static_cast<int>(std::floor(tpos.x)),static_cast<int>(std::floor(tpos.y)),static_cast<int>(std::floor(tpos.z))};
        for(int x=-r;x<=r;++x) for(int z=-r;z<=r;++z) for(int dy=-r;dy<=r;++dy) {
            const Block s{origin.x+x,origin.y+dy,origin.z+z};
            if(deferredAnchor(s,now)) continue;
            if(overlaps(self,s)) continue;
            if(!anchorInReach(s,eye,reach)) continue;
            const Vec centre=blast::anchorCentre(s.x,s.y,s.z);
            const double targetDx=centre.x-tpos.x, targetDz=centre.z-tpos.z;
            if(targetDx*targetDx+targetDz*targetDz>double(targetRadius.value()*targetRadius.value())) continue;
            const double d2=native::distance2(eye,centre); if(d2>(reach+1.0)*(reach+1.0)) continue;
            const double damage=blast::estimate(centre,blast::kAnchorPower,tpos); if(damage<=0.0) continue;
            candidates.push_back({s,damage,d2});
        }
        std::stable_sort(candidates.begin(),candidates.end(),[](const Candidate& l,const Candidate& c){return l.damage>c.damage || (l.damage==c.damage && l.d2<c.d2);});
        int checked=0, rays=0, occupied=0, noFace=0, shieldChecks=0;
        // Resume beyond failed candidates next tick instead of repeatedly
        // exhausting the budget on the same six unshieldable positions.
        if(anchorScanCursor>=candidates.size()) anchorScanCursor=0;
        for(;anchorScanCursor<candidates.size();) {
            if(checked>=48 || rays>=8 || (safe && shieldChecks>=6)) break;
            const auto& c=candidates[anchorScanCursor++];
            native::Frame frame(a.e); ++checked;
            if(!air(world,c.s)) continue;
            if(!entityFree(world,c.s)) { ++occupied; continue; }
            if(safe) {
                ++shieldChecks;
                std::string reason;
                if(!chooseShield(world,player,eye,c.s,reason) && reason!="already shielded") continue;
            }
            bool anyFace=false;
            for(const Vec& n : kFaces) {
                const Block base{c.s.x-static_cast<int>(n.x),c.s.y-static_cast<int>(n.y),c.s.z-static_cast<int>(n.z)};
                const Vec fc{base.x+.5+n.x*.5,base.y+.5+n.y*.5,base.z+.5+n.z*.5};   // face centre
                if(native::distance2(eye,fc)>reach*reach) continue;
                if((eye.x-fc.x)*n.x+(eye.y-fc.y)*n.y+(eye.z-fc.z)*n.z<=1e-3) continue;   // the eye has to be on the outer side of the face
                if(!support(world,base)) continue; // Avoid interactive or partial support blocks.
                anyFace=true;
                if(rays>=8) break;
                ++rays;
                if(!aimAtFace(world,player,eye,base,n,2)) continue;
                chosenSpot=Placement{base,c.s}; chosenUntil=now+200; loggedNone=false; anchorScanCursor=0;
                if(!loggedBest || !(*loggedBest==c.s)) {
                    loggedBest=c.s;
                    VLOG_INFO("explosives","{}: target {} at {}, best anchor spot {} {} {} through the {} face of {} {} {}, expected damage {} (checked {}, occupied {}, rays {})",name(),targetName(target),std::sqrt(native::distance2(eye,hitPoint(target,eye))),c.s.x,c.s.y,c.s.z,faceName(n),base.x,base.y,base.z,c.damage,checked,occupied,rays);
                }
                return chosenSpot;
            }
            if(!anyFace) ++noFace;
        }
        if(!loggedNone) { loggedNone=true; loggedBest.reset(); VLOG_INFO("explosives","{}: no anchor spot for target {}: {} candidates, checked {}, occupied {}, no reachable face {}, rays {}",name(),targetName(target),candidates.size(),checked,occupied,noFace,rays); }
        return {};
    }
    bool shieldProtects(jobject world,jobject player,Block b,Block spot) {
        return filled(world,spot) && shieldCoverage(b,spot,native::position(a,player,false),velocity(player))>=2;
    }
    bool shieldLeavesAnchorVisible(jobject world,jobject player,Vec eye,Block b,Block spot) {
        const Box6 shieldBox=cellBox(spot);
        const double reach=blockReach(player);
        const Vec vel=velocity(player);
        for(const Vec& n : kFaces) {
            for(double offset : {.5,.25,.75}) {
                native::Frame frame(a.e);
                Vec point{b.x+offset,b.y+offset,b.z+offset};
                if(n.x) point.x=b.x+(n.x>0?.99:.01);
                if(n.y) point.y=b.y+(n.y>0?.99:.01);
                if(n.z) point.z=b.z+(n.z>0?.99:.01);
                if(native::distance2(eye,point)>(reach-.05)*(reach-.05)) continue;
                double entry=0;
                if(rayEntersBox(eye,add(point,eye,-1),shieldBox,1,entry)) continue;
                const Vec future=add(eye,vel);
                if(rayEntersBox(future,add(point,future,-1),shieldBox,1,entry)) continue;
                auto hit=ray(world,player,eye,point);
                // The anchor may still be a prospective placement here.
                if(!blockHit(hit) || hitBlock(hit)==b) return true;
            }
        }
        return false;
    }
    std::optional<Block> existingShield(jobject world,jobject player,Block b) {
        const Vec feet=native::position(a,player,false), vel=velocity(player);
        for(const Block offset : {Block{1,0,0},Block{-1,0,0},Block{0,0,1},Block{0,0,-1}})
            for(int dy : {0,-1,1}) {
                native::Frame frame(a.e);
                const Block spot{b.x+offset.x,b.y+dy,b.z+offset.z};
                if(shieldCoverage(b,spot,feet,vel)>=2 && filled(world,spot) &&
                   shieldLeavesAnchorVisible(world,player,native::position(a,player,true),b,spot)) return spot;
            }
        return {};
    }
    // Rank all adjacent sides and heights before spending world raycasts.
    // Keep the existing plan while it still covers the moving player.
    std::optional<ShieldPlan> chooseShield(jobject world,jobject player,Vec eye,Block b,std::string& reason) {
        const Vec feet=native::position(a,player,false), vel=velocity(player);
        if(existingShield(world,player,b)) { reason="already shielded"; return {}; }
        struct Candidate { Block spot; int score; };
        std::vector<Candidate> candidates;
        for(const Block offset : {Block{1,0,0},Block{-1,0,0},Block{0,0,1},Block{0,0,-1}})
            for(int dy : {0,-1,1}) {
                const Block spot{b.x+offset.x,b.y+dy,b.z+offset.z};
                const int score=shieldCoverage(b,spot,feet,vel);
                if(score>=2) candidates.push_back({spot,score});
            }
        std::stable_sort(candidates.begin(),candidates.end(),[](const Candidate& l,const Candidate& r){return l.score>r.score;});
        const Box6 self=boxOf(player);
        bool anyFree=false;
        for(const auto& candidate : candidates) {
            native::Frame frame(a.e);
            const Block spot=candidate.spot;
            if(!placeable(world,self,spot)) continue;
            if(!shieldLeavesAnchorVisible(world,player,eye,b,spot)) continue;
            anyFree=true;
            // Manual mode accepts the support face the player is actually
            // looking at, instead of insisting on the automatic top face.
            if(!useLease()) {
                auto hit=ray(world,player,eye,add(eye,dirOf(yawOf(player),pitchOf(player)),blockReach(player)));
                if(blockHit(hit)) {
                    const Block looked=hitBlock(hit);
                    if(neighbour(looked,normal(hitSide(hit)))==spot && support(world,looked))
                        return ShieldPlan{spot,looked};
                }
            }
            // Top face of the block below first: the natural placement.
            const Block below{spot.x,spot.y-1,spot.z};
            if(!(below==b) && support(world,below) && aimAtFace(world,player,eye,below,faceNormal(below,spot),2)) return ShieldPlan{spot,below};
            // Otherwise any other solid neighbour's face that looks into the
            // spot. Never an anchor (this one or a neighbouring one) nor a
            // block with a use handler: their faces consume the click.
            const Block others[]={{spot.x,spot.y+1,spot.z},{spot.x+1,spot.y,spot.z},{spot.x-1,spot.y,spot.z},{spot.x,spot.y,spot.z+1},{spot.x,spot.y,spot.z-1}};
            for(const Block& n : others) {
                if(n==b || !support(world,n)) continue;
                if(aimAtFace(world,player,eye,n,faceNormal(n,spot),2)) return ShieldPlan{spot,n};
            }
        }
        reason=anyFree?"no shield spot: no face to place against in view":"no shield spot: no free spot beside the anchor";
        return {};
    }
    // -- validation, shared by the pump (real view) and the callback (applied) --
    // Attack: the ray enters the crystal's box (shrunk tighter than Grim's
    // +0.0005 expansion) inside the entity reach minus a margin, also from the
    // post-travel position when moving, and no block sits in front of the entry.
    bool attackValid(jobject world,jobject player,Vec eye,Vec dir,Vec vel,bool moving,Box6 b,double range,bool& obstructed) {
        obstructed=false;
        b.minX+=.0005; b.minY+=.0005; b.minZ+=.0005; b.maxX-=.0005; b.maxY-=.0005; b.maxZ-=.0005;
        double t=0; if(!rayEntersBox(eye,dir,b,range,t) || t>range-.05) return false;
        if(moving) { double t2=0; if(!rayEntersBox(add(eye,vel),dir,b,range,t2) || t2>range-.05) return false; }
        if(t>1e-3 && blockHit(ray(world,player,eye,add(eye,dir,t)))) { obstructed=true; return false; }
        return true;
    }
    // Place / Use / Shield: the block ray along `dir` hits `b` within the
    // block reach minus a margin (through the face `face` when given), the
    // eye sits on the outer side of the face for a placement, a block-item
    // placement finds a replaceable, entity-free cell where the block will
    // appear (World.canPlace), and Grim's RotationPlace rule holds for every
    // click with a block item in hand (`heldBlockItem`; a placement always
    // is): the ray from the eye -- and from the post-velocity eye when moving
    // -- intercepts the box of the PLACED position, the clicked block plus the
    // face normal (the clicked block itself when it is replaceable), within
    // the reach. A moving player's post-travel ray has to agree on block and
    // face too. Returns the hit (local ref).
    jobject placeValid(jobject world,jobject player,Vec eye,Vec dir,Vec vel,bool moving,Block b,bool placing,double reach,const Vec* face=nullptr,bool heldBlockItem=true) {
        auto hit=ray(world,player,eye,add(eye,dir,reach));
        if(!blockHit(hit) || !(hitBlock(hit)==b)) return nullptr;
        auto side=hitSide(hit); const Vec n=normal(side);
        if(face && !sameNormal(n,*face)) return nullptr;
        const Block placed=replaceable(world,b)?b:neighbour(b,n);
        if(placing && anchor && (!replaceable(world,placed) || !entityFree(world,placed))) return nullptr;
        // A Use with a block item in hand (glowstone charging the anchor, an
        // explode with a block item): no block appears, but Grim's
        // RotationPlace box stands in the cell behind the clicked face --
        // whatever that cell contains (a SimpleCollisionBox of the position,
        // not of its block), so nothing about its contents is required here;
        // the intercept test below covers it.
        const Vec hp=hitPos(hit);
        if(std::sqrt(native::distance2(eye,hp))>reach-.05) return nullptr;
        if(placing || face) {
            // FabricatedPlace / PositionPlace: the eye has to sit on the outer side of the face.
            const Vec centre{b.x+.5+n.x*.5,b.y+.5+n.y*.5,b.z+.5+n.z*.5};
            if((eye.x-centre.x)*n.x+(eye.y-centre.y)*n.y+(eye.z-centre.z)*n.z<=1e-3) return nullptr;
        }
        if(placing || heldBlockItem) {
            // RotationPlace: the ray has to intercept the placed position's box.
            const Box6 box=cellBox(placed);
            double t=0; if(!rayEntersBox(eye,dir,box,reach,t) || t>reach-.05) return nullptr;
            if(moving) { double t2=0; if(!rayEntersBox(add(eye,vel),dir,box,reach,t2) || t2>reach-.05) return nullptr; }
        }
        if(moving) {
            // Grim raycasts from the flying's post-travel position; one tick of velocity has to agree.
            auto hit2=ray(world,player,add(eye,vel),add(add(eye,vel),dir,reach));
            if(!blockHit(hit2) || !(hitBlock(hit2)==b) || !a.e->IsSameObject(hitSide(hit2),side)) return nullptr;
        }
        return hit;
    }
    // Whether the block half of `p` validates along `dir`: the same call the
    // callback makes, so the pump's "real view" answer and the tick's answer
    // agree. `inv` names the held item for a Use (block item or not).
    jobject blockValid(jobject world,jobject player,jobject inv,Vec eye,Vec dir,Vec vel,bool moving,const Plan& p) {
        const double reach=blockReach(player);
        if(anchor && p.kind==Kind::Place && !(p.block==p.spot)) {
            const Vec face=faceNormal(p.block,p.spot);
            return placeValid(world,player,eye,dir,vel,moving,p.block,true,reach,&face,true);
        }
        if(p.kind==Kind::Shield) { const Vec face=faceNormal(p.block,p.spot); return placeValid(world,player,eye,dir,vel,moving,p.block,true,reach,&face,true); }
        if(p.kind==Kind::Use) return placeValid(world,player,eye,dir,vel,moving,p.block,false,reach,nullptr,p.slot>=0 && blockItem(item(inv,p.slot)));
        return placeValid(world,player,eye,dir,vel,moving,p.block,true,reach,nullptr,anchor);
    }
    // Does the player's OWN view already validate the plan? Then the lease
    // targets the real angles and nothing synthetic goes out (Vulcan sees a
    // player who simply clicked what they were looking at). An Attack with a
    // follow-up placement needs the real view to do both, so the same aim
    // serves the next tick's placement too.
    bool realViewValid(jobject world,jobject player,jobject inv,Vec eye,const Plan& p) {
        native::Frame f(a.e);
        const Vec dir=dirOf(yawOf(player),pitchOf(player));
        const Vec vel=velocity(player); const bool moving=vel.x*vel.x+vel.y*vel.y+vel.z*vel.z>1e-8;
        if(p.kind==Kind::Attack) {
            if(!crystalRef) return false;
            bool obstructed=false;
            if(!attackValid(world,player,eye,dir,vel,moving,boxOf(crystalRef),entityReach(player),obstructed)) return false;
            if(p.follow) { Plan q=p; q.kind=Kind::Place; if(!blockValid(world,player,inv,eye,dir,vel,moving,q)) return false; }
            return true;
        }
        return blockValid(world,player,inv,eye,dir,vel,moving,p)!=nullptr;
    }
    // -- the lease ------------------------------------------------------------
    // Publishes the aim for the plan. Automatic + silent: lease the rotation
    // (the callback acts once it is applied). Otherwise: no lease; on_tick
    // acts with the real rotation and the crosshair has to be on the target.
    void lease(jobject world,jobject player,jobject inv,Vec eye,Vec aim) {
        if(!useLease()) { silentaim::release_action_rotation(this); return; }
        haveAim=true; aimPoint=aim;
        const float yaw=yawOf(player), pitch=pitchOf(player);
        float targetYaw=yaw, targetPitch=pitch;
        // Mid-combo (a synthetic aim on the wire, the last action under 400
        // ms ago: the explode after the charge, the placement after the
        // attack) the target stays the aim point even when the real view
        // would validate too -- switching to the real view here would step
        // the wire away from the point the next click reuses and cost the
        // "next tick" the combo is built on.
        // Keep a synthetic target while this action converges too, including
        // before the first placement. A briefly valid camera ray must not
        // alternate the target between the camera and the cached aim point.
        const bool sameLease=leaseSeq==aimSeq && leasePlan.kind==plan.kind &&
            leasePlan.block==plan.block && leasePlan.spot==plan.spot;
        const bool midCombo=haveLease && !leaseReal &&
            (sameLease || GetTickCount64()-lastAction<400) && silentaim::action_lease_held(this);
        leaseReal=false;
        if(plan.kind!=Kind::None && !midCombo && realViewValid(world,player,inv,eye,plan)) {
            // (a) zero spoof: the real view already does the job.
            leaseReal=true;
        } else if(!anglesTo(eye,aim,targetYaw,targetPitch)) {
            plan.kind=Kind::None; setStatus("Too close to aim"); silentaim::release_action_rotation(this); return;
        }
        // Remembered bit for bit: hold() re-leases this very target, so the
        // hold after a Done never moves the wire (a recomputed angle from a
        // drifted eye, or the aim angle after a real-view lease, is a new
        // target, i.e. a synthetic look nobody needed).
        leaseYaw=targetYaw; leasePitch=targetPitch; haveLease=true;
        leasePlan=plan; leaseSeq=aimSeq;
        // Crystals never turn the model (the old Head Bob is permanently off);
        // the anchor macros keep turning it, as they always did.
        if(!silentaim::request_action_rotation(this,yaw,pitch,targetYaw,targetPitch,anchor,&callbacks)) {
            plan.kind=Kind::None;
            setStatus(silentaim::silent_rotation_available()?"Rotation busy":"Waiting for rotation hook");
        }
    }
    // Nothing to do this frame, but the aim is still wanted: keep the lease
    // pointed where it is instead of easing back and re-tracing a moment later
    // (the mirrored out-and-back deltas Vulcan's rotation heuristics look for).
    void hold(jobject player,Vec eye,std::string s) {
        plan.kind=Kind::None; setStatus(std::move(s));
        if(!useLease() || !haveAim || !haveLease) return;
        const float yaw=yawOf(player), pitch=pitchOf(player);
        // Within 400 ms of the last action the next one is imminent (a crystal
        // in flight, a charge to confirm): stay on the aim. Later the lease is
        // released and the sent angle eases home at the randomised pace. It
        // must not be re-leased at the real view instead: a live lease keeps
        // the spoof on the wire (the aura stays silent, block breaking stays
        // suppressed) and the sent angle then trails a manual flick by a step.
        if(GetTickCount64()-lastAction>=400) { silentaim::release_action_rotation(this); return; }
        // The SAME target lease() last leased: the aim angle it computed, or
        // the real view when that was the target (the player's own mouse is
        // the only thing that moves it then). Never anglesTo() again here:
        // lease() and hold() disagreeing on the target was one extra look
        // and one extra settling tick per action.
        const float targetYaw=leaseReal?yaw:leaseYaw, targetPitch=leaseReal?pitch:leasePitch;
        silentaim::request_action_rotation(this,yaw,pitch,targetYaw,targetPitch,anchor,&callbacks);
    }
    static bool anglesTo(Vec eye,Vec aim,float& yaw,float& pitch) {
        const double dx=aim.x-eye.x,dy=aim.y-eye.y,dz=aim.z-eye.z;
        if(dx*dx+dy*dy+dz*dz<1e-6) return false;
        yaw=static_cast<float>(std::atan2(-dx,dz)*kDeg);
        pitch=static_cast<float>(-std::atan2(dy,std::hypot(dx,dz))*kDeg);
        return true;
    }
    void idle(std::string s) { plan.kind=Kind::None; setStatus(std::move(s)); silentaim::release_action_rotation(this); }
    // Does one ray from the eye through `aim` satisfy the attack (enter the
    // crystal box) and, with `wantPlace`, also the placement on `p.block`
    // (the block ray hits the base's top face, the placed box is intercepted)?
    // Geometry only (no movement), from the real eye.
    bool aimValid(jobject world,jobject player,jobject inv,Vec eye,Vec aim,const Plan& p,bool wantPlace) {
        native::Frame f(a.e);
        Vec d=add(aim,eye,-1.0); const double len=std::sqrt(d.x*d.x+d.y*d.y+d.z*d.z); if(len<1e-6) return false;
        d={d.x/len,d.y/len,d.z/len};
        if(!crystalRef) return false;
        bool obstructed=false;
        if(!attackValid(world,player,eye,d,Vec{},false,boxOf(crystalRef),entityReach(player),obstructed)) return false;
        if(!wantPlace) return true;
        Plan q=p; q.kind=Kind::Place;
        return blockValid(world,player,inv,eye,d,Vec{},false,q)!=nullptr;
    }
    // The aim point for `p`: reused while the plan is the same action, drawn
    // afresh (random inside the valid region) for every new one. An Attack
    // with a follow-up placement is drawn so ONE ray does both: enters the
    // crystal box and hits the base's top face; when no such point exists
    // (the eye is below the base's top face: the ray reaches the base through
    // a side face and never rises into the crystal box -- the usual
    // hole-fighting geometry) the follow-up is dropped and the placement is
    // aimed on its own once the crystal is gone.
    std::optional<Vec> aimFor(jobject world,jobject player,jobject inv,Vec eye,Plan& p) {
        // An anchor Place keeps the face (and so the spot) it first aimed at
        // while the base is the same, even when the ranking re-plans the same
        // base with another spot: one point, one lease, no re-tracing.
        const bool anchorPlace=anchor && p.kind==Kind::Place;
        const bool same=aimPlanSeq==aimSeq && aimPlan.kind==p.kind && aimPlan.block==p.block && aimPlan.crystalId==p.crystalId && aimPlan.spot==p.spot;
        if(same && haveAim) {
            if(anchorPlace) p.spot=aimPlan.spot;
            if(p.kind==Kind::Attack && !aimPlan.follow) p.follow=false;
            const auto now=GetTickCount64();
            if(now<aimCheck) return aimPoint;
            if(cachedAimValid(world,player,inv,eye,p)) {
                aimCheck=now+(anchor?50:100);
                return aimPoint;
            }
            // The point stopped validating from the current eye (the player
            // moved): draw a fresh one below, from the same plan.
            aimCheck=now+(anchor?50:100);
            if(actionLog()) VLOG_INFO("explosives","{}: aim point for {} no longer valid, redrawing",name(),at(p.block));
        }
        std::optional<Vec> aim;
        if(p.kind==Kind::Attack) {
            if(!crystalRef) return {};
            const Box6 cb=boxOf(crystalRef);
            if(p.follow) {
                // A point just under the top face validates both halves; when it
                // does not (or the top face is out of view), a point inside the
                // crystal box whose ray still hits the base; otherwise attack only.
                aim=aimAtBlock(world,player,eye,p.block,false);
                if(aim && !aimValid(world,player,inv,eye,*aim,p,true)) aim.reset();
                for(int i=0;i<3 && !aim;++i) { const Vec q=randomInBox(cb,eye); if(aimValid(world,player,inv,eye,q,p,true)) aim=q; }
                if(!aim) {
                    p.follow=false;
                    if(actionLog()) VLOG_INFO("explosives","{}: no ray both attacks crystal {} and places on {}, attacking first",name(),p.crystalId,at(p.block));
                }
            }
            if(!aim) {
                for(int i=0;i<3 && !aim;++i) { const Vec q=randomInBox(cb,eye); if(aimValid(world,player,inv,eye,q,p,false)) aim=q; }
                if(!aim) aim=randomInBox(cb,eye);
            }
        }
        else if(anchorPlace) {
            if(!(p.block==p.spot)) aim=aimAtFace(world,player,eye,p.block,faceNormal(p.block,p.spot),5,inv,&p);
            else aim=aimAtAnchorFace(world,player,eye,p.block,p.spot);
        }
        else if(p.kind==Kind::Shield) aim=aimAtFace(world,player,eye,p.block,faceNormal(p.block,p.spot),5,inv,&p);
        // A Use on the anchor: any visible face, top first, then the sides,
        // then the bottom. (The explode normally never gets here: it reuses
        // the charge's point through the re-tag in execute().)
        else if(p.kind==Kind::Use) {
            Vec face{};
            aim=aimAtUseFace(world,player,inv,eye,p,face);
            if(aim && actionLog()) VLOG_INFO("explosives","{}: {} aim on the {} face of the anchor at {}",name(),p.charges>0?"explode":"charge",faceName(face),at(p.block));
        }
        else aim=aimAtBlock(world,player,eye,p.block,false);
        if(aim) { aimPlan=p; aimPlanSeq=aimSeq; aimPoint=*aim; haveAim=true; aimCheck=GetTickCount64()+(anchor?50:100); }
        // A failed redraw drops the stale point: the next frame draws again
        // instead of handing the old point back for another 100 ms (which
        // commit() would lease and execute() could never validate), so the
        // callers' "no face reachable" timers actually accumulate.
        else if(same) { haveAim=false; aimPlanSeq=~0u; aimCheck=0; }
        return aim;
    }
    // Does the cached aim point still validate `p` from the real eye? One ray,
    // no movement: the same geometry the callback checks along the applied
    // rotation once the lease has converged on the point.
    bool cachedAimValid(jobject world,jobject player,jobject inv,Vec eye,const Plan& p) {
        if(p.kind==Kind::Attack) return aimValid(world,player,inv,eye,aimPoint,p,p.follow);
        native::Frame f(a.e);
        Vec d=add(aimPoint,eye,-1.0); const double len=std::sqrt(d.x*d.x+d.y*d.y+d.z*d.z); if(len<1e-6) return false;
        d={d.x/len,d.y/len,d.z/len};
        float yaw=0,pitch=0;
        if(!anglesTo(eye,aimPoint,yaw,pitch)) return false;
        const Vec vel=velocity(player);
        const bool moving=vel.x*vel.x+vel.y*vel.y+vel.z*vel.z>1e-8;
        return blockValid(world,player,inv,eye,dirOf(yaw,pitch),vel,moving,p)!=nullptr;
    }
    // Returns whether an aim point exists for the plan (always true without
    // the lease: nothing is aimed there).
    bool commit(jobject world,jobject player,jobject inv,Vec eye,Plan p,const char* text) {
        p.at=GetTickCount64();
        // No lease (Place / Break, or Both with Silent off): the plan is only
        // handed to on_tick, which validates it against the real rotation. No
        // aim point is drawn -- there is nothing to aim, the crosshair already
        // is on the target -- and no ray is cast.
        if(!useLease()) { plan=p; setStatus(text); silentaim::release_action_rotation(this); return true; }
        auto aim=aimFor(world,player,inv,eye,p);
        plan=p;
        if(!aim) { idle(p.kind==Kind::Attack?"Crystal not visible":p.kind==Kind::Shield?"Shield support not visible":(anchor?"Anchor not visible":"Base not visible")); return false; }
        setStatus(text);
        lease(world,player,inv,eye,*aim);
        return true;
    }
    // -- the pumps --------------------------------------------------------------
    void crystalPump(jobject mc,jobject world,jobject player,jobject inv,Vec eye,double reach) {
        const auto now=GetTickCount64();
        expireAttacked(now);
        if(victim && !alive(victim)) {
            a.e->DeleteGlobalRef(victim); victim=nullptr;
            if(pauseKill.value()) { next=now+1000; idle("Paused after kill"); return; }
        }
        if(now>=targetScan) {
            auto target=nearest(world,player,eye,10.0,Player,false);
            if(victim) a.e->DeleteGlobalRef(victim); victim=target?a.e->NewGlobalRef(target):nullptr; targetScan=now+200;
        }
        if(onlyGround.value() && !onGround(player)) { idle("Waiting for ground"); return; }
        const double attackRange=entityReach(player);
        if(!automatic()) {
            // Place / Break: no scan. The crosshair's EntityHitResult is the
            // only crystal this mode knows (Break), and Place knows none.
            if(crystalRef) { a.e->DeleteGlobalRef(crystalRef); crystalRef=nullptr; }
            if(canBreak()) {
                auto hit=a.get(mc,Client,"field_1765","Lnet/minecraft/class_239;");
                if(hit && a.e->IsInstanceOf(hit,a.cls("net/minecraft/class_3966"))) {
                    auto target=a.call<>(hit,"net/minecraft/class_3966","method_17782","()Lnet/minecraft/class_1297;");
                    if(target && a.e->IsInstanceOf(target,a.cls(Crystal)) && alive(target) && !wasAttacked(entityId(target))) crystalRef=a.e->NewGlobalRef(target);
                }
            }
        } else {
            // Scan every frame while a cycle is running (a crystal in hand, a lease,
            // a recent action or a placement in flight); every 100 ms otherwise --
            // the entity-list walk is the one per-frame cost worth bounding.
            const bool hot=crystalRef || silentaim::action_lease_held(this) || now-lastAction<1500 || now<placeWaitUntil;
            if(hot || now>=crystalScan) {
                crystalScan=now+100;
                auto crystal=nearest(world,player,eye,attackRange,Crystal,true);
                if(crystalRef) { a.e->DeleteGlobalRef(crystalRef); crystalRef=nullptr; }
                // A crystal this module has not hit yet is the one the last placement
                // produced (or someone else's): the placement wait is over either way.
                if(crystal) { crystalRef=a.e->NewGlobalRef(crystal); placeWaitUntil=0; }
            }
        }
        if(canBreak() && crystalRef) {
            const int id=entityId(crystalRef);
            if(!useLease()) {
                auto hit=a.get(mc,Client,"field_1765","Lnet/minecraft/class_239;");
                if(!hit || !a.e->IsInstanceOf(hit,a.cls("net/minecraft/class_3966"))) { idle("Look at the crystal"); return; }
                auto target=a.call<>(hit,"net/minecraft/class_3966","method_17782","()Lnet/minecraft/class_1297;");
                if(!a.e->IsSameObject(crystalRef,target)) { idle("Look at the crystal"); return; }
            }
            if(native::distance2(eye,hitPoint(crystalRef,eye))>attackRange*attackRange) { idle("Crystal out of reach"); return; }
            // The crystal stands on the block under it; that base takes the next
            // crystal on the NEXT tick, from the same aim, when everything lines up.
            const Vec pos=native::position(a,crystalRef);
            const Block base{static_cast<int>(std::floor(pos.x)),static_cast<int>(std::floor(pos.y))-1,static_cast<int>(std::floor(pos.z))};
            const int slot=automatic()?findSlot(inv,".end_crystal"):-1;
            const bool follow=useLease() && slot>=0 && native::distance2(eye,top(base))<=reach*reach && validBase(world,base) && spaceFree(world,base,id);
            Plan p; p.kind=Kind::Attack; p.block=base; p.spot=base; p.slot=slot; p.crystalId=id; p.follow=follow;
            commit(world,player,inv,eye,p,"Exploding crystal");
            return;
        }
        if(!canPlace()) { idle("No crystal under the crosshair"); return; }
        const int slot=findSlot(inv,".end_crystal"); if(slot<0) { idle("No crystals in hotbar"); return; }
        // A placement is in flight: the server's crystal arrives within a round
        // trip. Placing again meanwhile only produces a rejected interaction.
        if(now<placeWaitUntil) { hold(player,eye,"Waiting for the crystal"); return; }
        // The base of the crystal just attacked, while the attack's aim is
        // still good for it (400 ms): the placement reuses that very point.
        std::optional<Block> b;
        if(followBase) {
            if(now<followUntil && native::distance2(eye,top(*followBase))<=reach*reach && validBase(world,*followBase)) b=followBase;
            else followBase.reset();
        }
        if(!b) b=chooseBase(mc,world,player,eye,reach);
        if(!b) { idle("No crystal base"); return; }
        if(!spaceFree(world,*b,-1)) { hold(player,eye,"Base occupied"); return; }
        Plan p; p.kind=Kind::Place; p.block=*b; p.spot=*b; p.slot=slot;
        commit(world,player,inv,eye,p,now<next?"Waiting for delay":"Placing crystal");
    }
    void anchorPump(jobject mc,jobject world,jobject player,jobject inv,Vec eye,double reach) {
        const auto now=GetTickCount64();
        updateHighlight();
        std::erase_if(deferredAnchors,[&](const DeferredAnchor& d){return now>=d.until;});
        auto dimension=a.call<>(world,"net/minecraft/class_1937","method_27983","()Lnet/minecraft/class_5321;");
        if(a.e->IsSameObject(dimension,a.constant("net/minecraft/class_1937","field_25180","Lnet/minecraft/class_5321;"))) { idle("Anchors do not explode in the Nether"); return; }
        // Distance takes precedence over every confirmation wait, including
        // charged anchors. Neither silent macro should track an unreachable
        // block indefinitely when the player leaves the fight.
        const auto tracked=pending?pending:(own?own:working);
        if(useLease() && tracked && !anchorInReach(*tracked,eye,reach))
            releaseDistantAnchor(*tracked,now);
        // 1. Adopt the anchor at the pending position the moment it exists;
        //    give up on the placement after 600 ms and re-plan at once.
        //    The anchor is adopted wherever it actually appeared: the block
        //    behind the face that was clicked, or the clicked block itself when
        //    that one was replaceable (grass, a snow layer).
        if(pending) {
            std::optional<Block> found;
            if(isAnchor(world,*pending)) found=pending;
            else if(pendingBase && isAnchor(world,*pendingBase)) found=pendingBase;
            if(found) {
                own=found; working.reset(); expectedCharges=-1; awaitingExplosion=false; resetShield();
                updateHighlight();
                if(actionLog()) VLOG_INFO("explosives","{}: anchor appeared at {}{}",name(),at(*found),found==pending?"":" (in the clicked block)");
                pending.reset(); pendingBase.reset();
            } else if(now<pendingUntil) { hold(player,eye,"Waiting for placement"); return; }
            else { if(actionLog()) VLOG_INFO("explosives","{}: placement at {} not confirmed, re-planning",name(),at(*pending)); pending.reset(); pendingBase.reset(); }
        }
        // 2. An anchor of ours that vanished exploded (or was broken): the cycle
        //    is complete, the delay starts, the next one is planned right away.
        auto gone=[&](std::optional<Block>& slot){
            if(!slot || isAnchor(world,*slot)) return;
            if(safe && !awaitingExplosion) {
                // A predicted anchor disappearing is not a completed cycle.
                // Avoid resending the same rejected placement every tick.
                if(deferredAnchors.size()>=16) deferredAnchors.erase(deferredAnchors.begin());
                deferredAnchors.push_back({*slot,now+1500});
                next=std::max(next,now+150);
                chosenSpot.reset(); chosenUntil=0; baseScan=0;
                plan=Plan{}; haveAim=false; haveLease=false; aimPlanSeq=~0u;
                silentaim::release_action_rotation(this);
                if(actionLog()) VLOG_INFO("explosives","{}: anchor at {} disappeared before explosion; replanning",name(),at(*slot));
            }
            if(awaitingExplosion && actionLog()) VLOG_INFO("explosives","{}: anchor at {} exploded",name(),at(*slot));
            if(awaitingExplosion && !cycleDelayStarted) cooldown();
            slot.reset(); expectedCharges=-1; awaitingExplosion=false; cycleDelayStarted=false; resetShield();
        };
        gone(own); gone(working);
        updateHighlight();
        if(awaitingExplosion) {
            if(now<confirmationDeadline) { hold(player,eye,"Waiting for explosion"); return; }
            awaitingExplosion=false; cycleDelayStarted=false;
            if(actionLog()) VLOG_INFO("explosives","{}: explosion not confirmed within 600 ms, re-planning",name());
        }
        // 3. The target: the nearest living non-friend player within 8 blocks
        //    (eye to their closest box point), refreshed every 100 ms -- the
        //    only entity-list walk here. With one the macro places on its own,
        //    in the spot whose anchor hurts them most (the crosshair block is
        //    the fallback when the ranking finds nothing this scan); without
        //    one it never places on its own -- an anchor of ours under the
        //    crosshair is still charged and exploded, and a block under the
        //    crosshair still takes an anchor behind the looked-at face.
        //    Without the lease (Silent Aim off) there is no target and no scan
        //    at all: the crosshair is the only source.
        const bool targetPlacement=useLease() && placementMode.index()==0;
        if(victim && (!targetPlacement || !alive(victim))) { a.e->DeleteGlobalRef(victim); victim=nullptr; }
        if(targetPlacement && now>=targetScan) {
            auto target=nearest(world,player,eye,double(kTargetRange),Player,false);
            if(!a.e->IsSameObject(victim,target)) { chosenSpot.reset(); chosenUntil=0; baseScan=0; anchorScanCursor=0; }
            if(victim) a.e->DeleteGlobalRef(victim); victim=target?a.e->NewGlobalRef(target):nullptr; targetScan=now+100;
        }
        std::optional<Block> target=own?own:working;
        Block spot{};
        if(!target) {
            Block looked{}; bool onAnchor=false;
            auto cross=crosshairAnchor(mc,world,eye,reach,looked,onAnchor);
            // An existing anchor under the crosshair is always the first choice;
            // otherwise the damage ranking decides where the next anchor goes,
            // and the block under the crosshair is used when there is nobody to
            // hit or nothing rankable this scan.
            if(cross && onAnchor && !deferredAnchor(*cross,now) &&
               (!onlyOwn.value() || (own && *own==*cross) || (working && *working==*cross))) target=cross;
            else if(victim) { if(auto pl=chooseAnchorSpot(world,player,eye,reach,victim)) { target=pl->base; spot=pl->spot; } }
            if(!target && !victim && cross && !deferredAnchor(onAnchor?*cross:looked,now)) { target=cross; spot=looked; }
        }
        if(!target) { idle(victim?"No damaging anchor position":"No anchor under the crosshair"); return; }
        Block b=*target;
        if(isAnchor(world,b)) {
            // Own = placed by this module, or charged by it (working).
            if(onlyOwn.value() && !(own && b==*own) && !(working && b==*working)) { idle("Ignoring other anchors"); return; }
            auto charges=a.constant("net/minecraft/class_4969","field_23153","Lnet/minecraft/class_2758;");
            auto value=a.call<>(state(world,b),"net/minecraft/class_2688","method_11654","(Lnet/minecraft/class_2769;)Ljava/lang/Comparable;",charges);
            const int count=a.call<jint>(value,"java/lang/Integer","intValue","()I");
            // Observe actual interactions, not repeated aim requests. A plan
            // can keep returning an aim forever without ever being executed.
            if(safe && useLease() && count==0 && !pending && expectedCharges<0) {
                const int shieldItemSlot=shieldSlot(inv);
                const int required=!shieldDone && shieldItemSlot>=0 && isGlowstone(item(inv,shieldItemSlot))?2:1;
                const bool supplied=glowstoneCount(inv)>=required && (shieldDone || shieldItemSlot>=0);
                if(!supplied || !stalledAnchor || !(*stalledAnchor==b) || observedAction!=lastAction) {
                    stalledAnchor=b; stalledSince=now; observedAction=lastAction;
                } else if(now-stalledSince>=static_cast<ULONGLONG>(kRecoveryTime)) {
                    if(deferredAnchors.size()>=16) deferredAnchors.erase(deferredAnchors.begin());
                    deferredAnchors.push_back({b,now+static_cast<ULONGLONG>(kRecoveryTime)});
                    if(actionLog()) VLOG_INFO("explosives","{}: uncharged anchor at {} stalled for {} ms; trying another position",name(),at(b),kRecoveryTime);
                    own.reset(); working.reset(); resetShield(); stalledAnchor.reset();
                    chosenSpot.reset(); chosenUntil=0; baseScan=0;
                    plan=Plan{}; haveAim=false; haveLease=false; aimPlanSeq=~0u;
                    idle("Recovering: choosing another anchor position"); return;
                }
            } else { stalledAnchor.reset(); }
            // An anchor of ours is always finished -- charged, then exploded --
            // wherever the target went: nothing of ours stays standing or armed.
            // The charge is applied client-side inside interactBlock itself
            // (RespawnAnchorBlock.charge runs on both sides), so this clears on
            // the very next frame; the timeout only covers a rejected use.
            if(expectedCharges>=0) {
                if(count>expectedCharges) expectedCharges=-1;
                else if(now<confirmationDeadline) { hold(player,eye,"Waiting for charge"); return; }
                else { expectedCharges=-1; if(actionLog()) VLOG_INFO("explosives","{}: charge not confirmed, re-planning",name()); }
            }
            // 4. Confirm the shield before charging. Manual mode uses the
            // real view for the support click and retains the current anchor
            // while the player moves the crosshair between the two blocks.
            if(safe) {
                if(!shieldAnchor || !(*shieldAnchor==b)) { resetShield(); shieldAnchor=b; }
                if(shieldDone && (!protectedBy || !shieldProtects(world,player,b,*protectedBy))) {
                    resetShield(); shieldAnchor=b;
                    haveAim=false; aimPlanSeq=~0u;
                }
                if(shield && !shieldDone && !shieldPending &&
                    shieldCoverage(b,shield->at,native::position(a,player,false),velocity(player))<2) {
                    shield.reset(); shieldChosen=false; haveAim=false; aimPlanSeq=~0u;
                }
                if(shieldBlocked) {
                    if(now<shieldUntil) { idle("Waiting to retry shield"); return; }
                    resetShield(); shieldAnchor=b;
                    haveAim=false; haveLease=false; aimPlanSeq=~0u;
                }
                if(!useLease() && !shieldDone && !shieldPending) shieldChosen=false;
                if(!shieldDone) {
                    if(!shieldPending && (!reserveAllows(inv,eye,2) ||
                       (!reserveAllows(inv,eye,1,
                           (shieldSlot(inv)>=0 && isGlowstone(item(inv,shieldSlot(inv))))?2:1)))) {
                        idle("Material reserve reached: waiting for refill or target");
                        return;
                    }
                    if(shieldPending) {
                        std::string what;
                        if(shieldUp(world,*shieldPending,what)) {
                            if(!shieldProtects(world,player,b,*shieldPending)) {
                                resetShield(); shieldAnchor=b; idle("Repositioning shield after movement"); return;
                            }
                            protectedBy=shieldPending; shieldDone=true;
                            if(actionLog()) VLOG_INFO("explosives","{}: shield up at {} ({})",name(),at(*shieldPending),what);
                        }
                        else if(now<shieldUntil) { hold(player,eye,"Waiting for the shield"); return; }
                        else { shieldBlocked=true; shieldUntil=now+static_cast<ULONGLONG>(kRetryDelay); idle("Shield placement not confirmed"); return; }
                    } else {
                        if(!shieldChosen) {
                            shieldChosen=true;
                            std::string reason;
                            if(auto sh=chooseShield(world,player,eye,b,reason)) { shield=sh; if(actionLog()) VLOG_INFO("explosives","{}: shield spot {} against {} {}",name(),at(sh->at),at(sh->support),faceName(faceNormal(sh->support,sh->at))); }
                            else if(reason=="already shielded") { protectedBy=existingShield(world,player,b); shieldDone=protectedBy.has_value(); if(!shieldDone) { shieldChosen=false; return; } }
                            else { shieldBlocked=true; shieldUntil=now+static_cast<ULONGLONG>(kRetryDelay); idle(reason); return; }
                        }
                        if(!shieldDone) {
                            const int slot=shieldSlot(inv);
                            if(slot<0) { idle("No shield block in hotbar"); return; }
                            if(count==0 && isGlowstone(item(inv,slot)) && glowstoneCount(inv)<2) {
                                idle("Need 2 glowstone: shield and charge"); return;
                            }
                            if(!placeable(world,boxOf(player),shield->at) || !support(world,shield->support)) { shieldBlocked=true; shieldUntil=now+static_cast<ULONGLONG>(kRetryDelay); idle("Shield spot no longer usable"); return; }
                            else {
                                Plan p; p.kind=Kind::Shield; p.block=shield->support; p.spot=shield->at; p.slot=slot;
                                if(!useLease() && !realViewValid(world,player,inv,eye,p)) {
                                    idle("Aim at shield support " + at(p.block) + " (" + faceName(faceNormal(p.block,p.spot)) + ")");
                                    return;
                                }
                                // The shield gets the same budget as the charge:
                                // a support face unreachable from the eye for
                                // 300 ms skips the shield instead of holding the
                                // (uncharged) anchor forever.
                                if(commit(world,player,inv,eye,p,"Shielding anchor")) { shieldAimFail=0; return; }
                                if(!shieldAimFail) { shieldAimFail=now; return; }
                                if(now-shieldAimFail<300) return;
                                shieldBlocked=true; shieldAimFail=0; shieldUntil=now+static_cast<ULONGLONG>(kRetryDelay);
                                idle("Shield support not reachable"); return;
                            }
                        }
                    }
                }
            }
            if(count>0) {
                // The server's onUseWithItem PASSes to the offhand when it holds
                // glowstone, and vanilla would then charge from OFF_HAND -- a
                // packet this module never sends.
                auto off=a.call<>(player,"net/minecraft/class_1309","method_6079","()Lnet/minecraft/class_1799;");
                auto offItem=a.call<>(off,"net/minecraft/class_1799","method_7909","()Lnet/minecraft/class_1792;");
                auto key=jvm::toStdString(a.e,static_cast<jstring>(a.call<>(offItem,"net/minecraft/class_1792","method_7876","()Ljava/lang/String;")));
                if(isGlowstone(key)) { idle("Remove glowstone from offhand"); return; }
            }
            if(count==0 && !reserveAllows(inv,eye,1)) {
                idle("Glowstone reserve reached: waiting for refill or target");
                return;
            }
            // shouldCancelInteraction: sneaking skips the block's onUse entirely.
            if(sneaking(player)) { idle("Stop sneaking to use the anchor"); return; }
            const int slot=count>0?explodeSlot(inv,count):findSlot(inv,".glowstone");
            if(slot<0) { idle(count>0?"Need a non-glowstone slot":"No glowstone in hotbar"); return; }
            Plan p; p.kind=Kind::Use; p.block=b; p.spot=b; p.slot=slot; p.charges=count;
            if(!useLease() && !realViewValid(world,player,inv,eye,p)) {
                idle(count>0?"Aim at the anchor to explode":"Aim at the anchor to charge");
                return;
            }
            // 5. The charge; the explode after it reuses the charge's aim
            //    point (re-tagged in execute()) and only changes the slot.
            //    A charge with no reachable face for 300 ms abandons the
            //    (still harmless, uncharged) anchor and plans a new position
            //    at once; an armed anchor is never abandoned.
            const bool aimed=commit(world,player,inv,eye,p,count>0?"Exploding anchor":"Charging anchor");
            if(aimed || count>0) { useAimFail=0; return; }
            if(!useAimFail) useAimFail=now;
            else if(now-useAimFail>=300) {
                if(actionLog()) VLOG_INFO("explosives","{}: anchor at {} not reachable; retaining it until visible",name(),at(b));
                // Retain this anchor: losing visibility must not start a trail
                // of new placements. The next pump retries its visible faces.
                useAimFail=now;
                idle("Move to see the current anchor");
            }
            return;
        }
        // Prepare the aim during the cycle delay; execute() still enforces
        // `next` before interacting, so rotation and cooldown can overlap.
        const int slot=findSlot(inv,".respawn_anchor"); if(slot<0) { idle("No anchors in hotbar"); return; }
        if(!canStartAnchor(inv,eye)) {
            idle(combatTargetInRange(eye)?"Material reserve reached":"Waiting for target in range");
            return;
        }
        if(safe && swap.value()) {
            // Do not start a fresh anchor only to consume the last glowstone
            // on its shield. Existing protection needs just the charge item.
            std::string reason;
            const auto sh=chooseShield(world,player,eye,spot,reason);
            const bool protectedAlready=!sh && reason=="already shielded";
            if(!sh && !protectedAlready) {
                chosenSpot.reset(); chosenUntil=0;
                idle("No usable shield support at this position"); return;
            }
            const int protectionSlot=shieldSlot(inv);
            if(!protectedAlready && protectionSlot<0) { idle("No shield block in hotbar"); return; }
            const int required=!protectedAlready && isGlowstone(item(inv,protectionSlot))?2:1;
            if(glowstoneCount(inv)<required) { idle(required==2?"Need 2 glowstone: shield and charge":"No glowstone to charge"); return; }
        }
        // `spot` is where the anchor is expected; the callback re-derives it
        // from the face its own ray hits, and adoption accepts either.
        Plan p; p.kind=Kind::Place; p.block=b; p.spot=spot; p.slot=slot;
        commit(world,player,inv,eye,p,now<next?"Waiting for delay":"Placing anchor");
    }
    void logCache() {
        if(cacheLogged) return; cacheLogged=true;
        auto has=[&](const char* owner,const char* id,const char* sig,bool field=false){ try { auto c=a.cls(owner); if(field) a.field(c,id,sig); else a.method(c,id,sig); return 1; } catch(const native::Failure&) { return 0; } };
        VLOG_INFO("explosives","{} cache: attackEntity={} interactBlock={} swing={} raycast={} ctx={} getSide={} getPos={} velocity={} hasVehicle={} sneaking={} onGround={} getId={} blockRange={} entityRange={} syncSlot={} selectedSlot={} offsetX={} charges={} stateGet={} isReplaceable={} isFullCube={} asItem={} getBlockEntity={} isEmpty={} -> ready",name(),
            has("net/minecraft/class_636","method_2918","(Lnet/minecraft/class_1657;Lnet/minecraft/class_1297;)V"),
            has("net/minecraft/class_636","method_2896","(Lnet/minecraft/class_746;Lnet/minecraft/class_1268;Lnet/minecraft/class_3965;)Lnet/minecraft/class_1269;"),
            has("net/minecraft/class_1309","method_6104","(Lnet/minecraft/class_1268;)V"),
            has("net/minecraft/class_1922","method_17742","(Lnet/minecraft/class_3959;)Lnet/minecraft/class_3965;"),
            has("net/minecraft/class_3959","<init>","(Lnet/minecraft/class_243;Lnet/minecraft/class_243;Lnet/minecraft/class_3959$class_3960;Lnet/minecraft/class_3959$class_242;Lnet/minecraft/class_1297;)V"),
            has("net/minecraft/class_3965","method_17780","()Lnet/minecraft/class_2350;"),
            has("net/minecraft/class_239","method_17784","()Lnet/minecraft/class_243;"),
            has(Entity,"method_18798","()Lnet/minecraft/class_243;"),
            has(Entity,"method_5765","()Z"),
            has(Entity,"method_5715","()Z"),
            has(Entity,"method_24828","()Z"),
            has(Entity,"method_5628","()I"),
            has(Player,"method_55754","()D"),
            has(Player,"method_55755","()D"),
            has("net/minecraft/class_636","method_2911","()V"),
            has("net/minecraft/class_1661","field_7545","I",true),
            has("net/minecraft/class_2350","method_10148","()I"),
            has("net/minecraft/class_4969","field_23153","Lnet/minecraft/class_2758;",true),
            has("net/minecraft/class_2688","method_11654","(Lnet/minecraft/class_2769;)Ljava/lang/Comparable;"),
            has(BlockState,"method_45474","()Z"),
            has(BlockState,"method_26234","(Lnet/minecraft/class_1922;Lnet/minecraft/class_2338;)Z"),
            has("net/minecraft/class_2248","method_8389","()Lnet/minecraft/class_1792;"),
            has("net/minecraft/class_1922","method_8321","(Lnet/minecraft/class_2338;)Lnet/minecraft/class_2586;"),
            has("net/minecraft/class_1799","method_7960","()Z"));
    }
    // -- the callback: runs inside the tick, on the game thread ---------------
    silentaim::ActionResult execute(const silentaim::ActionContext& ctx,bool allowAction) {
        using silentaim::ActionResult;
        // The registration outlives a disable (the slot restore lands on the
        // next tick); once that is done, nothing here may cost a JNI call.
        if(!isEnabled() && restoreSlot<0) return ActionResult::Wait;
        if(isEnabled() && safe && previousAimMode!=aimMode.index()) return ActionResult::Wait;
        a.e=ctx.env;
        try {
            native::Frame frame(a.e);
            auto mc=ctx.minecraft, player=ctx.player; if(!mc || !player) return ActionResult::Wait;
            auto inv=a.call<>(player,Player,"method_31548","()Lnet/minecraft/class_1661;");
            auto manager=a.get(mc,Client,"field_1761","Lnet/minecraft/class_636;");
            const auto now=GetTickCount64();
            const Plan p=plan;
            // 400 ms of staleness: the pump refreshes the plan every frame, so
            // only a real hitch trips this, and then nothing acts on old data.
            const bool idleTick=!allowAction || p.kind==Kind::None || now-p.at>400;
            // The slot goes back only on a genuinely idle tick, 300 ms after the
            // last action, never in the middle of a cycle -- and only from the
            // PRE-TICK point (the on_tick call inside handleInputEvents, before
            // the movement frame opens: !after_entity_actions, !position_final,
            // !frame_open), i.e. ahead of this tick's PlayerInput and
            // sprint/sneak EntityActions. The sendMovementPackets fallback point
            // sits after them, where a slot change on a toggle tick is
            // PacketOrderE; nothing is restored from there.
            const bool preTick=!ctx.after_entity_actions && !ctx.position_final && !ctx.frame_open;
            if(restoreSlot>=0 && preTick && ctx.tick_id!=actionTick && ctx.tick_id!=lastAttackTick && ctx.tick_id!=lastBlockTick && idleTick && now-lastAction>=300) { restore(inv,manager); return ActionResult::Wait; }
            if(idleTick) return ActionResult::Wait;
            // One action TYPE per tick: an attack never shares a tick id with a
            // block interaction, in either order (MultiActionsF, PacketOrderJ).
            const bool attackPlan=p.kind==Kind::Attack;
            const bool blockPlan=p.kind==Kind::Place || p.kind==Kind::Use || p.kind==Kind::Shield;
            if(attackPlan && ctx.tick_id==lastBlockTick) return ActionResult::Wait;
            if(blockPlan && (ctx.tick_id==lastAttackTick || ctx.tick_id==lastBlockTick)) return ActionResult::Wait;
            if(!alive(player)) return ActionResult::Wait;
            if(a.get(mc,Client,"field_1755","Lnet/minecraft/class_437;")) return ActionResult::Wait;
            if(a.call<jboolean>(player,Entity,"method_5765","()Z")) return ActionResult::Wait;
            if(!anchor && onlyGround.value() && !onGround(player)) return ActionResult::Wait;
            auto world=a.get(mc,Client,"field_1687","Lnet/minecraft/class_638;"); if(!world) return ActionResult::Wait;
            if(worldRef && !a.e->IsSameObject(worldRef,world)) return ActionResult::Wait;
            const Vec eye=native::position(a,player,true);
            if(anchor && useLease()) {
                const auto tracked=pending?pending:(own?own:working);
                if(tracked && !anchorInReach(*tracked,eye,blockReach(player))) {
                    releaseDistantAnchor(*tracked,now);
                    return ActionResult::Abort;
                }
            }
            const Vec vel=velocity(player);
            const bool moving=vel.x*vel.x+vel.y*vel.y+vel.z*vel.z>1e-8;
            const Vec dir=dirOf(ctx.applied_yaw,ctx.applied_pitch);
            auto hand=a.constant("net/minecraft/class_1268","field_5808","Lnet/minecraft/class_1268;");
            const Block b=p.block;
            // -- Attack. Never waits for the delay. No slot change: the crystal
            //    is hit with whatever is in hand.
            if(attackPlan) {
                if(!(crystalRef && a.e->IsInstanceOf(crystalRef,a.cls(Crystal)) && alive(crystalRef) && entityId(crystalRef)==p.crystalId && !wasAttacked(p.crystalId))) return ActionResult::Wait;
                const Box6 cbox=boxOf(crystalRef); bool obstructed=false;
                if(!attackValid(world,player,eye,dir,vel,moving,cbox,entityReach(player),obstructed)) { if(obstructed) setStatus("Crystal obstructed"); return ActionResult::Wait; }
                a.call<void>(manager,"net/minecraft/class_636","method_2918","(Lnet/minecraft/class_1657;Lnet/minecraft/class_1297;)V",player,crystalRef);
                a.call<void>(player,"net/minecraft/class_1309","method_6104","(Lnet/minecraft/class_1268;)V",hand);
                attacked.push_back({p.crystalId,now});
                lastAttackTick=ctx.tick_id;
                acted=true; lastAction=now; actionTick=ctx.tick_id; plan.kind=Kind::None; ++aimSeq;
                // The follow-up placement, next tick, from the SAME aim point:
                // the base is remembered and the aim is re-tagged as the Place
                // plan the pump commits next, so aimFor() reuses it verbatim
                // and the lease does not move.
                if(p.follow && haveAim) {
                    followBase=b; followUntil=now+400;
                    aimPlan=p; aimPlan.kind=Kind::Place; aimPlan.crystalId=-1; aimPlan.follow=false; aimPlan.spot=b; aimPlanSeq=aimSeq;
                } else followBase.reset();
                if(actionLog()) VLOG_INFO("explosives","{}: attacked crystal {} at {} {} {} from yaw {} pitch {} (wire yaw {} pitch {}), lattice {}{}",name(),p.crystalId,(cbox.minX+cbox.maxX)*.5,cbox.minY,(cbox.minZ+cbox.maxZ)*.5,ctx.applied_yaw,ctx.applied_pitch,ctx.wire_yaw,ctx.wire_pitch,silentaim::mouse_lattice_deg(),p.follow?", placing on the next tick":"");
                return ActionResult::Done;
            }
            if(!blockPlan) return ActionResult::Wait;
            // -- Place / Use / Shield. `next` is the delay between cycles (set by
            //    a crystal placement or a confirmed anchor explosion) or the
            //    short backoff after a rejected interaction; an anchor charge /
            //    shield / explode only ever meets the latter.
            if(now<next) return ActionResult::Wait;
            if(p.kind==Kind::Use) {
                if(!isAnchor(world,b)) return ActionResult::Wait;
                // A prediction correction can change the charge between the
                // render pump and this callback. Never execute a stale use.
                auto property=a.constant("net/minecraft/class_4969","field_23153","Lnet/minecraft/class_2758;");
                auto chargeValue=a.call<>(state(world,b),"net/minecraft/class_2688","method_11654","(Lnet/minecraft/class_2769;)Ljava/lang/Comparable;",property);
                const int currentCharges=a.call<jint>(chargeValue,"java/lang/Integer","intValue","()I");
                if(currentCharges!=p.charges) { plan.kind=Kind::None; return ActionResult::Wait; }
                if(safe && (!protectedBy || !shieldProtects(world,player,b,*protectedBy))) {
                    shieldDone=false; shieldChosen=false; shieldPending.reset();
                    protectedBy.reset(); plan.kind=Kind::None;
                    setStatus("Shield no longer covers you"); return ActionResult::Wait;
                }
                if(sneaking(player)) { setStatus("Stop sneaking to use the anchor"); return ActionResult::Wait; }
            } else if(p.kind==Kind::Shield) {
                if(!shieldAnchor || shieldCoverage(*shieldAnchor,p.spot,native::position(a,player,false),vel)<2) {
                    shieldChosen=false; shield.reset(); plan.kind=Kind::None;
                    setStatus("Replanning shield after movement"); return ActionResult::Wait;
                }
                if(!placeable(world,boxOf(player),p.spot) || !support(world,b)) return ActionResult::Wait;
            } else {
                if(!validBase(world,b) || (!anchor && !spaceFree(world,b,p.crystalId))) return ActionResult::Wait;
            }
            jobject hit=blockValid(world,player,inv,eye,dir,vel,moving,p);
            if(!hit) return ActionResult::Wait;
            // The anchor lands behind the face this ray hit (placeValid already
            // required a free cell there); that is the block to watch, whatever
            // the pump planned. A replaceable base takes it itself.
            Block spot=p.spot;
            if(anchor && p.kind==Kind::Place) spot=replaceable(world,b)?b:neighbour(b,normal(hitSide(hit)));
            // -- the wire. Every click Grim treats as a block place (a block
            //    item in hand: the anchor, the shield, glowstone charging,
            //    an explode with a block item) goes out only once the
            //    pre-flying pass, run here exactly as Grim runs it against
            //    the rotation on the wire (and the prev-yaw combos), hits the
            //    placed cell; the post-flying pass is blockValid above along
            //    the applied rotation, which the flying behind the click
            //    carries either way (a Look when it changed, the forced
            //    PositionAndOnGround when it did not). With the lease that is
            //    the whole gate: a stationary lease is bit-stable so the
            //    tick after the landing passes with wire == applied, and a
            //    moving player's wire (last tick's aim at the same point)
            //    still intercepts the cell -- requiring wire_settled as well
            //    deadlocked while walking, since the aim angle to a fixed
            //    point moves with the eye every tick and the settle never
            //    came. wire_settled stays informational in the log. In
            //    Client mode (the real view on the wire) a look that already
            //    pointed at the cell last tick is enough, as it is for Grim.
            //    A crystal placement is not a BlockPlace for Grim: exempt.
            const bool grimPlace=p.kind==Kind::Shield || (p.kind==Kind::Place && anchor) || (p.kind==Kind::Use && p.slot>=0 && blockItem(item(inv,p.slot)));
            if(grimPlace) {
                const Block cell=replaceable(world,b)?b:neighbour(b,normal(hitSide(hit)));
                const char* combo="none";
                const bool pre=preFlyingHits(ctx,eye,vel,moving,cellBox(cell),blockReach(player),combo);
                const bool ready=useLease()?pre:(ctx.wire_settled || pre);
                if(!ready) {
                    if(now-settleLogAt>=500) {
                        settleLogAt=now;
                        VLOG_INFO("explosives","{}: settling {} at {} (cell {}): applied yaw {} pitch {}, wire yaw {} pitch {}{}, pre-flying {}",name(),
                            p.kind==Kind::Shield?"shield":p.kind==Kind::Place?"anchor placement":(p.charges>0?"explode":"charge"),at(b),at(cell),
                            ctx.applied_yaw,ctx.applied_pitch,ctx.wire_yaw,ctx.wire_pitch,ctx.wire_settled?" (settled)":" (not yet sent)",combo);
                    }
                    return ActionResult::Wait;
                }
            }
            // Vanilla order inside the tick: the slot field is set FIRST, so the
            // single UpdateSelectedSlot (interactBlock's own syncSelectedSlot)
            // precedes the interaction; nothing else of ours went out on this
            // tick (the guard above), so the slot change is the tick's first
            // action packet -- what PacketOrderE expects.
            const int current=selected(inv);
            if(p.slot>=0 && p.slot!=current) { if(restoreSlot<0) restoreSlot=current; switchedSlot=p.slot; setSlot(inv,p.slot); }
            // The shield item's key, read BEFORE the click empties the slot
            // when it held the last block: the confirmation compares the
            // block that appears against it.
            const std::string heldKey=(p.kind==Kind::Shield && p.slot>=0)?item(inv,p.slot):std::string();
            auto result=a.call<>(manager,"net/minecraft/class_636","method_2896","(Lnet/minecraft/class_746;Lnet/minecraft/class_1268;Lnet/minecraft/class_3965;)Lnet/minecraft/class_1269;",player,hand,hit);
            bool accepted=a.call<jboolean>(result,"net/minecraft/class_1269","method_23665","()Z");
            // The client's ActionResult is not the server's for a crystal:
            // EndCrystalItem.useOnBlock FAILs as soon as ANY entity stands in
            // the box above the base, and a crystal this module hit on the
            // previous tick is still there client-side (clientDamage never
            // removes it; only the server's damage path does, and its destroy
            // packet is a round trip away). The server, which killed the
            // crystal first, accepts the placement. So a client FAIL that is
            // explained by an attacked crystal in the box counts as placed --
            // without this every cycle took the 300 ms rejection backoff.
            if(!accepted && !anchor && !spaceFree(world,b,-1,true)) accepted=true;
            if(accepted) a.call<void>(player,"net/minecraft/class_1309","method_6104","(Lnet/minecraft/class_1268;)V",hand);
            lastBlockTick=ctx.tick_id;
            // The packets are out either way (interactBlock always sends); only the bookkeeping is conditional.
            // Crystals: the cycle delay starts at the placement. Anchors: it
            // starts when the explosion is confirmed (anchorPump), so the
            // charge, the shield and the explode that follow a placement never wait.
            if(!anchor) { cooldown(); placeWaitUntil=now+300; followBase.reset(); }
            if(!accepted) next=std::max<ULONGLONG>(next,now+(anchor?kRetryDelay:300));
            if(accepted && anchor) {
                if(p.kind==Kind::Place) { pending=spot; pendingBase=b; pendingUntil=now+600; }
                else if(p.kind==Kind::Shield) { shieldPending=p.spot; shieldUntil=now+600; shieldKey=heldKey; }
                else if(p.charges>0) {
                    awaitingExplosion=true; confirmationDeadline=now+600;
                    // Both anchor macros overlap the configured cycle delay with
                    // the response wait. The next anchor still requires both.
                    cycleDelayStarted=true;
                    if(cycleDelayStarted) cooldown();
                }
                else { working=b; expectedCharges=p.charges; confirmationDeadline=now+600; }
            } else if(anchor && p.kind==Kind::Shield) { shieldBlocked=true; shieldUntil=now+static_cast<ULONGLONG>(kRetryDelay); VLOG_INFO("explosives","{}: shield placement rejected; combo stopped",name()); }
            if(!accepted) setStatus("Interaction rejected");
            if(actionLog()) VLOG_INFO("explosives","{}: {} at {}{}{} from yaw {} pitch {} (wire yaw {} pitch {}{}), lattice {}",name(),
                accepted?(p.kind==Kind::Place?(anchor?"placed anchor":"placed crystal"):p.kind==Kind::Shield?"placed shield":(p.charges>0?"exploded anchor":"charged anchor")):"interaction rejected",
                at(b),(anchor && (p.kind==Kind::Place || p.kind==Kind::Shield))?std::string(" -> ")+at(spot):std::string(),
                p.kind==Kind::Use?std::string(" via the ")+faceName(normal(hitSide(hit)))+" face":std::string(),
                ctx.applied_yaw,ctx.applied_pitch,ctx.wire_yaw,ctx.wire_pitch,ctx.wire_settled?", settled":"",silentaim::mouse_lattice_deg());
            acted=true; lastAction=now; actionTick=ctx.tick_id; plan.kind=Kind::None; ++aimSeq;
            // The explode, next tick, from the SAME aim point as the charge:
            // the aim is re-tagged under the new sequence so aimFor() hands
            // the explode plan (same kind, same block) this very point, the
            // lease keeps its target (silentaim keeps the settle for an
            // unchanged target), and only the slot changes. With a non-block
            // explode item no place rule applies and it fires on the first
            // tick the lease is applied again; with a block item the wire
            // still carries this rotation, so the pre-flying pass holds by
            // construction.
            if(accepted && anchor && p.kind==Kind::Use && p.charges==0 && haveAim) { aimPlan=p; aimPlanSeq=aimSeq; aimCheck=now+150; }
            return ActionResult::Done;
        } catch(const native::Failure& ex) {
            setStatus("Retrying interaction"); VLOG_WARN("explosives","{}: {}",name(),ex.what());
            plan.kind=Kind::None; next=GetTickCount64()+(anchor?kRetryDelay:2000);
            return ActionResult::Abort;
        }
    }
    // -- the pump: chooses, leases, never sends ---------------------------------
    void tick() {
        if(anchor && (previousPlacementMode!=placementMode.index() || previousTargetRadius!=targetRadius.value())) {
            previousPlacementMode=placementMode.index();
            previousTargetRadius=targetRadius.value();
            anchorScanCursor=0;
            chosenSpot.reset(); chosenUntil=0; baseScan=0; targetScan=0;
            if(plan.kind==Kind::Place) {
                plan=Plan{}; haveAim=false; haveLease=false; aimPlanSeq=~0u;
                silentaim::release_action_rotation(this);
            }
        }
        if(safe && previousAimMode!=aimMode.index()) {
            previousAimMode=aimMode.index();
            // Keep placement/charge confirmations and anchor ownership when
            // changing mode, but discard the old rotation and queued click.
            silentaim::release_action_rotation(this);
            plan=Plan{}; haveAim=false; haveLease=false; aimPlanSeq=~0u;
            shieldBlocked=false; shieldAimFail=0; useAimFail=0;
            stalledAnchor.reset();
            if(!shieldDone && !shieldPending) { shieldChosen=false; shield.reset(); }
        }
        if(vgui::bridge::isOverlayVisible() || !native::focused() || vgui::bridge::isEnabled("Freecam")) {silentaim::release_action_rotation(this); return;}
        jvm::JniScope scope; if(!scope) return; a.e=scope.get(); native::Frame frame(a.e);
        auto mc=a.minecraft(); if(!a.call<jboolean>(mc,Client,"method_18854","()Z")) return;
        auto world=a.get(mc,Client,"field_1687","Lnet/minecraft/class_638;");
        auto player=a.get(mc,Client,"field_1724","Lnet/minecraft/class_746;");
        if(!world || !player) {reset(); return;}
        if(worldRef && !a.e->IsSameObject(worldRef,world)) reset();
        if(!worldRef) {worldRef=a.e->NewGlobalRef(world); a.check();}
        if(!alive(player)) {reset(); return;}
        if(a.get(mc,Client,"field_1755","Lnet/minecraft/class_437;")) {silentaim::release_action_rotation(this); return;}
        logCache();
        // Manual interactions also depend on the pre-tick callback hook.
        silentaim::ensure_installed();
        if(acted) { acted=false; if(keybind().mode()==BindMode::Once) { setEnabled(false); return; } }
        // No flying while riding (the vehicle branch sends its own look packet
        // and skips sendMovementPackets), so no callback and no lease.
        if(a.call<jboolean>(player,Entity,"method_5765","()Z")) { idle("Riding"); return; }
        auto inv=a.call<>(player,Player,"method_31548","()Lnet/minecraft/class_1661;");
        Vec eye=native::position(a,player,true);
        double reach=blockReach(player);
        if(!std::isfinite(reach) || reach<=0) return;
        if(anchor) anchorPump(mc,world,player,inv,eye,reach); else crystalPump(mc,world,player,inv,eye,reach);
    }
    static const char* nameOf(Variant v) { return v==Variant::Crystal?"Auto Crystal":v==Variant::Anchor?"Anchor Macro":"Safe Anchor"; }
    static const char* descriptionOf(Variant v) {
        return v==Variant::Crystal?"Places and explodes end crystals on valid bases"
             : v==Variant::Anchor?"Places, charges and explodes respawn anchors"
             : "Places anchors, confirms a shield, then charges and explodes them";
    }
public:
    explicit Explosive(Variant v):Module(nameOf(v),descriptionOf(v),Category::Combat),variant(v),anchor(v!=Variant::Crystal),safe(v==Variant::SafeAnchor) {
        if(!anchor) registerSetting(&mode);
        registerSetting(&minDelay); registerSetting(&maxDelay);
        if(anchor) {
            if(safe) {
                aimMode.setDescription("Silent aims automatically; Manual uses your crosshair for the shield and anchor");
                registerSetting(&aimMode);
            } else registerSetting(&silent);
            registerSetting(&swap); registerSetting(&onlyOwn);
            placementMode.setDescription("Target places near the opponent; Crosshair places where you aim. Manual always uses your crosshair");
            targetRadius.setDescription("Maximum horizontal distance in blocks from the target for automatic placement");
            placementMode.setVisibleIf([this]{return useLease();});
            targetRadius.setVisibleIf([this]{return useLease() && placementMode.index()==0;});
            materialReserve.setDescription("Keep this many anchors, glowstone and shield blocks in the hotbar; set 5 or 7 for PvP");
            registerSetting(&placementMode); registerSetting(&targetRadius); registerSetting(&materialReserve);
            if(safe) {
                shieldBlock.setDescription("The block placed between you and the anchor before it blows; Any takes the first full block in your hotbar");
                registerSetting(&shieldBlock);
            }
        } else {
            registerSetting(&onlyGround); registerSetting(&pauseKill); registerSetting(&silent);
            silent.setVisibleIf([this]{return mode.index()==2;});
        }
        // Silent: act once the leased rotation is applied. Client: act from the
        // pre-tick point with the real rotation. Both paths also carry the
        // slot restore, which must never share a tick with a use.
        callbacks.on_ready=[this](const silentaim::ActionContext& c){ return useLease()?execute(c,true):silentaim::ActionResult::Wait; };
        // Client mode verifies against the REAL rotation, so it must not act while
        // another owner (the aura, another module's lease) puts a different
        // rotation on this tick's flying: Grim judges the use against that packet.
        callbacks.on_tick=[this](const silentaim::ActionContext& c){
            if(useLease() && silentaim::action_lease_held(this)) return;
            const bool manual=!useLease() && !silentaim::rotation_spoof_active();
            if(safe && manual && isEnabled() && !c.after_entity_actions && !c.position_final && !c.frame_open) {
                // Consume the current world state and camera at the action
                // point; a missed render frame must not cost another tick.
                plan.kind=Kind::None;
                try { tick(); }
                catch(const native::Failure& ex) {
                    VLOG_WARN("explosives","{}: {}",name(),ex.what());
                    reset(); next=GetTickCount64()+(anchor?kRetryDelay:2000);
                }
            }
            execute(c,manual);
        };
    }
    ~Explosive() override { reset(); silentaim::unregister_action_callbacks(this); }
    void onInit() override { listen<events::GameLoopEvent>([this](events::GameLoopEvent&){if(!isEnabled()) return; try{tick();}catch(const native::Failure& ex){setStatus("Retrying interaction"); VLOG_WARN("explosives","{}: {}",name(),ex.what()); reset(); next=GetTickCount64()+(anchor?kRetryDelay:2000);}}); }
    void onEnable() override { reset(); silentaim::register_action_callbacks(this,callbacks); setStatus("Ready"); }
    void onDisable() override { reset(); }
    std::string displayInfo() const override {
        if(!anchor) return std::string{mode.current()};
        return useLease()?"Silent":"Manual";
    }
    bool deferredClick() const noexcept override {return true;}
};
class Auto : public Explosive {public: Auto():Explosive(Variant::Crystal){}};
class AnchorMacro : public Explosive {public: AnchorMacro():Explosive(Variant::Anchor){}};
class SafeAnchor : public Explosive {public: SafeAnchor():Explosive(Variant::SafeAnchor){}};
VANTA_REGISTER_MODULE(Auto);
VANTA_REGISTER_MODULE(AnchorMacro);
VANTA_REGISTER_MODULE(SafeAnchor);
}
}
