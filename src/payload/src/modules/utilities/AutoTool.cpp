#include "core/NativeCallbacks.h"
// AutoTool.cpp — picks the best hotbar tool for the block being mined or the
// entity being attacked, and can do it WITHOUT the client ever seeing the swap.
//
// Silent Swap (verified against the 1.21.11 bytecode):
//   ClientPlayerInteractionManager.syncSelectedSlot compares PlayerInventory
//   .selectedSlot against its own lastSelectedSlot and only emits a packet when
//   they differ. So sending UpdateSelectedSlotC2SPacket ourselves, WITHOUT ever
//   writing selectedSlot, leaves the client rendering the old item (hotbar and
//   hand both read selectedSlot) while the server acts on the slot we sent —
//   and vanilla never contradicts us, because from its point of view nothing
//   changed. The one leak is the user scrolling: that moves selectedSlot, the
//   sync fires and clobbers our slot, so the real slot is watched and the spoof
//   re-asserted when it moves.
//
// The visible path instead writes selectedSlot and lets vanilla emit the packet
// from its own call site (the same thing AutoTotem does), and deliberately never
// swaps mid-break, because changing the held stack resets block-breaking
// progress.

#include "core/event/Events.h"
#include "core/module/Module.h"
#include "jvm/JniHelper.h"
#include "jvm/JvmContext.h"
#include "platform/Win32Process.h"
#include "util/Log.h"
#include "vgui/bridge.h"

#include <jnihook.h>

#include <windows.h>   // SEH around JNIHook_Attach, same as the silentaim hook

#include <atomic>

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

namespace dt::modules {

namespace {

// Bring-up logging for the swap path, written next to the other Dt logs.
bool g_dbg_on = false;
void dbg(const char* fmt, ...)
{
#ifndef _DEBUG
    (void)fmt;
    return;
#else
    if (!g_dbg_on) return;
    static std::FILE* fp = nullptr;
    static bool tried = false;
    if (!tried) {
        tried = true;
        const std::wstring dbg_path = dt::platform::logDir() + L"\\dt_autotool.txt";
        _wfopen_s(&fp, dbg_path.c_str(), L"w");
    }
    if (!fp) return;
    static const auto t0 = std::chrono::steady_clock::now();
    const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - t0).count();
    std::fprintf(fp, "[%7lld] ", ms);
    va_list ap; va_start(ap, fmt);
    std::vfprintf(fp, fmt, ap);
    va_end(ap);
    std::fputc(0x0A, fp);
    std::fflush(fp);
#endif
}

// ── Verified Yarn 1.21.11 symbols ────────────────────────────────────────────
struct cache {
    bool tried = false, ok = false;

    jclass    mc_class = nullptr;
    jmethodID mc_getInstance = nullptr;
    jfieldID  mc_player = nullptr, mc_world = nullptr;
    jfieldID  mc_crosshair = nullptr, mc_interaction = nullptr;

    jfieldID  player_inventory = nullptr;                 // PlayerEntity.inventory
    jfieldID  player_network = nullptr;                   // ClientPlayerEntity.networkHandler
    jfieldID  inv_selected = nullptr;                     // PlayerInventory.selectedSlot
    jmethodID inv_getStack = nullptr;                     // PlayerInventory.getStack(int)

    jmethodID nh_sendPacket = nullptr;                    // ClientCommonNetworkHandler.sendPacket
    jclass    slot_packet_class = nullptr;                // UpdateSelectedSlotC2SPacket
    jmethodID slot_packet_ctor = nullptr;                 // (I)V

    jmethodID hit_getType = nullptr;                      // HitResult.getType
    jobject   hit_type_block = nullptr, hit_type_entity = nullptr;
    jmethodID bhr_getBlockPos = nullptr;                  // BlockHitResult.getBlockPos
    jmethodID world_getBlockState = nullptr;              // BlockView.getBlockState

    jmethodID stack_isEmpty = nullptr;                    // ItemStack.isEmpty
    jmethodID stack_miningSpeed = nullptr;                // getMiningSpeedMultiplier(BlockState)
    jmethodID stack_suitableFor = nullptr;                // isSuitableFor(BlockState)
    jmethodID stack_isIn = nullptr;                       // isIn(TagKey)
    jmethodID stack_getEnchants = nullptr;                // ItemStack.getEnchantments()
    jmethodID ench_getSet    = nullptr;                   // ItemEnchantmentsComponent set
    jmethodID entry_matchesId = nullptr;                  // RegistryEntry.matchesId(Identifier)
    jmethodID set_iterator = nullptr, iter_hasNext = nullptr, iter_next = nullptr;
    jobject   id_fortune = nullptr, id_silk = nullptr;    // global Identifier refs
    jobject   tag_swords = nullptr, tag_axes = nullptr;

    jmethodID im_isBreaking = nullptr;                    // isBreakingBlock()
    jclass    im_class = nullptr;                         // ClientPlayerInteractionManager
    jfieldID  im_network = nullptr;                       // its own networkHandler
    jmethodID ent_isSneaking = nullptr, ent_isOnGround = nullptr;
    jfieldID  im_progress = nullptr;      // ClientPlayerInteractionManager.currentBreakingProgress
    jfieldID  ent_age = nullptr;          // ticks, used to find tick edges

    // MC's own attack key — the exact boolean that drives handleBlockBreaking().
    jfieldID  mc_options    = nullptr;    // MinecraftClient.options
    jfieldID  opt_attackKey = nullptr;    // GameOptions.attackKey
    jfieldID  key_pressed   = nullptr;    // KeyBinding.pressed
    // Vanilla's between-blocks state, and what block is actually being mined.
    jfieldID  mc_attackCd   = nullptr;    // MinecraftClient.attackCooldown
    jfieldID  im_cooldown   = nullptr;    // blockBreakingCooldown (set to 5 on break)
    jfieldID  im_breakPos   = nullptr;    // currentBreakingPos
    // Debug-only readers.
    jmethodID bp_getX = nullptr, bp_getY = nullptr, bp_getZ = nullptr;
    jclass    bp_class = nullptr;         // BlockPos, for the zombie-break reset
    jmethodID bp_ctor  = nullptr;         // BlockPos(int,int,int)
    jmethodID state_getBlock = nullptr;   // BlockState.getBlock
    jmethodID state_isAir    = nullptr;   // BlockState.isAir
    jmethodID state_toolReq  = nullptr;   // BlockState.isToolRequired
    jmethodID block_transKey = nullptr;   // Block.getTranslationKey
};
cache C{};

jobject global_static_obj(JNIEnv* env, jclass cls, const char* field, const char* sig)
{
    jfieldID f = env->GetStaticFieldID(cls, field, sig);
    if (!f) { if (env->ExceptionCheck()) env->ExceptionClear(); return nullptr; }
    jobject o = env->GetStaticObjectField(cls, f);
    if (!o) return nullptr;
    jobject g = env->NewGlobalRef(o);
    env->DeleteLocalRef(o);
    return g;
}

// Bindable on its own so it can be RETRIED: init() never runs again once it
// succeeds, and these classes can legitimately not be loaded yet on the first
// pass. Without the retry, Prefer Fortune / Silk Touch would have silently done
// nothing for the rest of the session.
void bind_enchants(JNIEnv* env)
{
    if (C.stack_getEnchants && C.id_fortune && C.id_silk && C.entry_matchesId
        && C.ench_getSet && C.set_iterator && C.iter_hasNext && C.iter_next) return;
    // ItemStack.getEnchantments(). This one used to be bound in init() next to
    // the other ItemStack methods; extracting this function moved the rest here
    // and left this line behind in the deleted block, so it stayed nullptr and
    // has_enchant() bailed out on its very first check for every slot — the
    // whole preference was inert. Bind it here with the others.
    if (!C.stack_getEnchants) {
        if (jclass is = dt::jvm::findClassAnyLoader(env, "net/minecraft/class_1799")) {
            C.stack_getEnchants = env->GetMethodID(is, "method_58657",
                                      "()Lnet/minecraft/class_9304;");
            if (env->ExceptionCheck()) env->ExceptionClear();
            env->DeleteLocalRef(is);
        }
    }
    if (!C.ench_getSet) {
        if (jclass ec = dt::jvm::findClassAnyLoader(env, "net/minecraft/class_9304")) {
            C.ench_getSet = env->GetMethodID(ec, "method_57534", "()Ljava/util/Set;");
            if (env->ExceptionCheck()) env->ExceptionClear();
            env->DeleteLocalRef(ec);
        }
    }
    if (!C.entry_matchesId) {
        if (jclass re = dt::jvm::findClassAnyLoader(env, "net/minecraft/class_6880")) {
            C.entry_matchesId = env->GetMethodID(re, "method_40226", "(Lnet/minecraft/class_2960;)Z");
            if (env->ExceptionCheck()) env->ExceptionClear();
            env->DeleteLocalRef(re);
        }
    }
    if (!C.id_fortune || !C.id_silk) {
        if (jclass ident = dt::jvm::findClassAnyLoader(env, "net/minecraft/class_2960")) {
            // Identifier.ofVanilla(String) -> minecraft:<path>
            jmethodID of = env->GetStaticMethodID(ident, "method_60656",
                               "(Ljava/lang/String;)Lnet/minecraft/class_2960;");
            if (env->ExceptionCheck()) env->ExceptionClear();
            if (of) {
                auto mk = [&](const char* path) -> jobject {
                    jstring js = env->NewStringUTF(path);
                    if (!js) return nullptr;
                    jobject id = env->CallStaticObjectMethod(ident, of, js);
                    env->DeleteLocalRef(js);
                    if (env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }
                    if (!id) return nullptr;
                    jobject g = env->NewGlobalRef(id);
                    env->DeleteLocalRef(id);
                    return g;
                };
                if (!C.id_fortune) C.id_fortune = mk("fortune");
                if (!C.id_silk)    C.id_silk    = mk("silk_touch");
            }
            env->DeleteLocalRef(ident);
        }
    }
    if (!C.set_iterator) {
        if (jclass st = env->FindClass("java/util/Set")) {
            C.set_iterator = env->GetMethodID(st, "iterator", "()Ljava/util/Iterator;");
            env->DeleteLocalRef(st);
        }
    }
    if (!C.iter_hasNext || !C.iter_next) {
        if (jclass it = env->FindClass("java/util/Iterator")) {
            C.iter_hasNext = env->GetMethodID(it, "hasNext", "()Z");
            C.iter_next    = env->GetMethodID(it, "next", "()Ljava/lang/Object;");
            env->DeleteLocalRef(it);
        }
    }
    if (env->ExceptionCheck()) env->ExceptionClear();
    static bool reported = false;
    const bool ready = C.stack_getEnchants && C.ench_getSet && C.entry_matchesId
                    && C.set_iterator && C.iter_hasNext && C.iter_next
                    && C.id_fortune && C.id_silk;
    if (!reported && ready) {
        reported = true;
        dbg("enchant lookup ready (getEnchantments=%p set=%p matchesId=%p "
            "fortune=%p silk=%p)",
            (void*)C.stack_getEnchants, (void*)C.ench_getSet,
            (void*)C.entry_matchesId, (void*)C.id_fortune, (void*)C.id_silk);
    } else if (!ready) {
        dbg("enchant lookup NOT ready: getEnchantments=%p compSet=%p matchesId=%p "
            "setIter=%p hasNext=%p next=%p fortune=%p silk=%p",
            (void*)C.stack_getEnchants, (void*)C.ench_getSet, (void*)C.entry_matchesId,
            (void*)C.set_iterator, (void*)C.iter_hasNext, (void*)C.iter_next,
            (void*)C.id_fortune, (void*)C.id_silk);
    }
}

bool init(JNIEnv* env)
{
    if (C.ok) return true;
    // Do NOT latch a failure: UpdateSelectedSlotC2SPacket and the item tags are
    // loaded lazily by the game, so the first attempts (main menu) legitimately
    // fail. Retry, rate limited, until everything resolves.
    static long long s_next_try = 0;
    const long long now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    if (now < s_next_try) return false;
    s_next_try = now + 1000;

    jclass mc     = dt::jvm::findClassAnyLoader(env, "net/minecraft/class_310");
    jclass player = dt::jvm::findClassAnyLoader(env, "net/minecraft/class_1657");
    jclass cplayer= dt::jvm::findClassAnyLoader(env, "net/minecraft/class_746");
    jclass inv    = dt::jvm::findClassAnyLoader(env, "net/minecraft/class_1661");
    jclass nh     = dt::jvm::findClassAnyLoader(env, "net/minecraft/class_634");
    jclass pkt    = dt::jvm::findClassAnyLoader(env, "net/minecraft/class_2868");
    jclass hit    = dt::jvm::findClassAnyLoader(env, "net/minecraft/class_239");
    jclass hitT   = dt::jvm::findClassAnyLoader(env, "net/minecraft/class_239$class_240");
    jclass bhr    = dt::jvm::findClassAnyLoader(env, "net/minecraft/class_3965");
    jclass world  = dt::jvm::findClassAnyLoader(env, "net/minecraft/class_638");
    jclass stack  = dt::jvm::findClassAnyLoader(env, "net/minecraft/class_1799");
    jclass tags   = dt::jvm::findClassAnyLoader(env, "net/minecraft/class_3489");
    jclass im     = dt::jvm::findClassAnyLoader(env, "net/minecraft/class_636");
    if (!mc || !player || !cplayer || !inv || !nh || !pkt || !hit || !hitT
        || !bhr || !world || !stack || !tags || !im) {
        VLOG_WARN("autotool", "class resolve failed — module inert");
        return false;
    }

    C.mc_getInstance  = env->GetStaticMethodID(mc, "method_1551", "()Lnet/minecraft/class_310;");
    C.mc_player       = env->GetFieldID(mc, "field_1724", "Lnet/minecraft/class_746;");
    C.mc_world        = env->GetFieldID(mc, "field_1687", "Lnet/minecraft/class_638;");
    C.mc_crosshair    = env->GetFieldID(mc, "field_1765", "Lnet/minecraft/class_239;");
    C.mc_interaction  = env->GetFieldID(mc, "field_1761", "Lnet/minecraft/class_636;");

    C.player_inventory = env->GetFieldID(player,  "field_7514", "Lnet/minecraft/class_1661;");
    C.player_network   = env->GetFieldID(cplayer, "field_3944", "Lnet/minecraft/class_634;");
    C.inv_selected     = env->GetFieldID(inv, "field_7545", "I");
    C.inv_getStack     = env->GetMethodID(inv, "method_5438", "(I)Lnet/minecraft/class_1799;");

    // sendPacket lives on ClientCommonNetworkHandler; JNI finds it through the
    // subclass. The name has moved between 1.21.x drops, so try a fallback too.
    C.nh_sendPacket = env->GetMethodID(nh, "method_52787", "(Lnet/minecraft/class_2596;)V");
    if (!C.nh_sendPacket) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        C.nh_sendPacket = env->GetMethodID(nh, "method_2883", "(Lnet/minecraft/class_2596;)V");
        if (env->ExceptionCheck()) env->ExceptionClear();
    }
    if (!C.nh_sendPacket) {
        if (jclass cch = dt::jvm::findClassAnyLoader(env, "net/minecraft/class_8673")) {
            C.nh_sendPacket = env->GetMethodID(cch, "method_52787", "(Lnet/minecraft/class_2596;)V");
            if (env->ExceptionCheck()) env->ExceptionClear();
            env->DeleteLocalRef(cch);
        }
    }
    C.slot_packet_ctor = env->GetMethodID(pkt, "<init>", "(I)V");

    C.hit_getType       = env->GetMethodID(hit, "method_17783",
                              "()Lnet/minecraft/class_239$class_240;");
    C.bhr_getBlockPos   = env->GetMethodID(bhr, "method_17777", "()Lnet/minecraft/class_2338;");
    C.world_getBlockState = env->GetMethodID(world, "method_8320",
                              "(Lnet/minecraft/class_2338;)Lnet/minecraft/class_2680;");

    C.stack_isEmpty     = env->GetMethodID(stack, "method_7960", "()Z");
    C.stack_miningSpeed = env->GetMethodID(stack, "method_7924", "(Lnet/minecraft/class_2680;)F");
    C.stack_suitableFor = env->GetMethodID(stack, "method_7951", "(Lnet/minecraft/class_2680;)Z");
    C.stack_isIn        = env->GetMethodID(stack, "method_31573", "(Lnet/minecraft/class_6862;)Z");
    bind_enchants(env);
    C.im_isBreaking     = env->GetMethodID(im, "method_2923", "()Z");
    C.im_progress       = env->GetFieldID(im, "field_3715", "F");
    C.im_cooldown       = env->GetFieldID(im, "field_3716", "I");
    C.im_breakPos       = env->GetFieldID(im, "field_3714", "Lnet/minecraft/class_2338;");
    C.im_network        = env->GetFieldID(im, "field_3720", "Lnet/minecraft/class_634;");
    C.im_class          = static_cast<jclass>(env->NewGlobalRef(im));
    if (jclass ent = dt::jvm::findClassAnyLoader(env, "net/minecraft/class_1297")) {
        C.ent_isSneaking = env->GetMethodID(ent, "method_5715", "()Z");
        C.ent_isOnGround = env->GetMethodID(ent, "method_24828", "()Z");
        C.ent_age        = env->GetFieldID(ent, "field_6012", "I");
        env->DeleteLocalRef(ent);
    }
    // MC's attack key. Verified: class_310.field_1690 -> class_315.field_1886
    // -> class_304.field_1653. GetAsyncKeyState is only kept for comparison.
    C.mc_options = env->GetFieldID(mc, "field_1690", "Lnet/minecraft/class_315;");
    // doAttack() sets this to 10 whenever you click at air, and while it is > 0
    // handleBlockBreaking() returns immediately — 10 ticks (500 ms) of NOTHING.
    C.mc_attackCd = env->GetFieldID(mc, "field_1771", "I");
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (jclass go = dt::jvm::findClassAnyLoader(env, "net/minecraft/class_315")) {
        C.opt_attackKey = env->GetFieldID(go, "field_1886", "Lnet/minecraft/class_304;");
        if (env->ExceptionCheck()) env->ExceptionClear();
        env->DeleteLocalRef(go);
    }
    if (jclass kb = dt::jvm::findClassAnyLoader(env, "net/minecraft/class_304")) {
        C.key_pressed = env->GetFieldID(kb, "field_1653", "Z");
        if (env->ExceptionCheck()) env->ExceptionClear();
        env->DeleteLocalRef(kb);
    }
    // Debug readers (never required for the module to work).
    if (jclass bp = dt::jvm::findClassAnyLoader(env, "net/minecraft/class_2338")) {
        C.bp_getX = env->GetMethodID(bp, "method_10263", "()I");
        C.bp_getY = env->GetMethodID(bp, "method_10264", "()I");
        C.bp_getZ = env->GetMethodID(bp, "method_10260", "()I");
        C.bp_ctor = env->GetMethodID(bp, "<init>", "(III)V");
        if (env->ExceptionCheck()) env->ExceptionClear();
        if (!C.bp_class) C.bp_class = static_cast<jclass>(env->NewGlobalRef(bp));
        env->DeleteLocalRef(bp);
    }
    if (jclass bs = dt::jvm::findClassAnyLoader(env, "net/minecraft/class_2680")) {
        C.state_getBlock = env->GetMethodID(bs, "method_26204", "()Lnet/minecraft/class_2248;");
        C.state_isAir    = env->GetMethodID(bs, "method_26215", "()Z");
        C.state_toolReq  = env->GetMethodID(bs, "method_29291", "()Z");
        if (env->ExceptionCheck()) env->ExceptionClear();
        env->DeleteLocalRef(bs);
    }
    if (jclass blk = dt::jvm::findClassAnyLoader(env, "net/minecraft/class_2248")) {
        C.block_transKey = env->GetMethodID(blk, "method_63499", "()Ljava/lang/String;");
        if (env->ExceptionCheck()) env->ExceptionClear();
        env->DeleteLocalRef(blk);
    }
    if (env->ExceptionCheck()) env->ExceptionClear();

    C.hit_type_block  = global_static_obj(env, hitT, "field_1332", "Lnet/minecraft/class_239$class_240;");
    C.hit_type_entity = global_static_obj(env, hitT, "field_1331", "Lnet/minecraft/class_239$class_240;");
    C.tag_swords      = global_static_obj(env, tags, "field_42611", "Lnet/minecraft/class_6862;");
    C.tag_axes        = global_static_obj(env, tags, "field_42612", "Lnet/minecraft/class_6862;");

    C.mc_class          = static_cast<jclass>(env->NewGlobalRef(mc));
    C.slot_packet_class = static_cast<jclass>(env->NewGlobalRef(pkt));

    env->DeleteLocalRef(mc); env->DeleteLocalRef(player); env->DeleteLocalRef(cplayer);
    env->DeleteLocalRef(inv); env->DeleteLocalRef(nh); env->DeleteLocalRef(pkt);
    env->DeleteLocalRef(hit); env->DeleteLocalRef(hitT); env->DeleteLocalRef(bhr);
    env->DeleteLocalRef(world); env->DeleteLocalRef(stack); env->DeleteLocalRef(tags);
    env->DeleteLocalRef(im);

    C.ok = C.mc_getInstance && C.mc_player && C.mc_crosshair && C.mc_interaction
        && C.player_inventory && C.player_network && C.inv_selected && C.inv_getStack
        && C.nh_sendPacket && C.slot_packet_ctor && C.slot_packet_class
        && C.hit_getType && C.hit_type_block && C.bhr_getBlockPos && C.world_getBlockState
        && C.stack_isEmpty && C.stack_miningSpeed && C.im_isBreaking && C.mc_class;
    VLOG_INFO("autotool", "init ok={} sendPacket={} pktClass={} tags[s={} a={}]",
              (int)C.ok, (void*)C.nh_sendPacket, (void*)C.slot_packet_class,
              (void*)C.tag_swords, (void*)C.tag_axes);
    if (!C.ok) {
        // Release what we did take so the retry starts clean.
        if (C.mc_class) { env->DeleteGlobalRef(C.mc_class); C.mc_class = nullptr; }
        if (C.slot_packet_class) { env->DeleteGlobalRef(C.slot_packet_class); C.slot_packet_class = nullptr; }
        if (C.hit_type_block)  { env->DeleteGlobalRef(C.hit_type_block);  C.hit_type_block = nullptr; }
        if (C.hit_type_entity) { env->DeleteGlobalRef(C.hit_type_entity); C.hit_type_entity = nullptr; }
        if (C.tag_swords) { env->DeleteGlobalRef(C.tag_swords); C.tag_swords = nullptr; }
        if (C.tag_axes)   { env->DeleteGlobalRef(C.tag_axes);   C.tag_axes = nullptr; }
        if (C.id_fortune) { env->DeleteGlobalRef(C.id_fortune); C.id_fortune = nullptr; }
        if (C.id_silk)    { env->DeleteGlobalRef(C.id_silk);    C.id_silk = nullptr; }
    }
    return C.ok;
}

// ── AutoTool's own slot-sync hook ─────────────────────────────────────────
// Verified in the 1.21.11 bytecode: ClientPlayerInteractionManager
// .updateBlockBreakingProgress (method_2902) begins with
//     0: aload_0
//     1: invokevirtual method_2911   // syncSelectedSlot
//     4: getfield      field_3716    // blockBreakingCooldown  (early return)
// i.e. vanilla re-sends the held slot as the FIRST thing of every mining tick,
// even during the 5-tick cooldown after a block breaks, and always before the
// START_DESTROY / STOP_DESTROY packets further down the same method.
//
// That is why the visible swap always worked and Silent Swap did not: the
// visible path rides vanilla's own send site, while our packet was going out
// from interactionManager.tick() — a different point of the tick. When it
// landed after the destroy packets the server evaluated the block with the
// EMPTY hand, and STOP_DESTROY needs delta*(ticks+1) >= 0.7 (a bare hand gives
// ~0.04), so the server denied the break and re-mined the block itself at hand
// speed. Hooking method_2911 puts our packet exactly where vanilla's goes:
// inside the tick (no Grim "Post"), ahead of every destroy packet.
//
// We run the ORIGINAL FIRST and send afterwards, so that if vanilla emits the
// real slot in the same call (the user scrolled) our slot is the one that wins.
// DECLARATIVE, not a one-shot queue: g_server_slot is what the server SHOULD
// believe we hold, and the hook keeps making that true. The old single-slot
// mailbox could be overwritten between two ticks, silently dropping a send and
// stranding the server on the wrong item for a whole block with no way back.
std::atomic<int>  g_server_slot{ -1 };   // -1 = don't care
std::atomic<int>  g_sent_slot{ -1 };     // what we last actually put on the wire
std::atomic<bool> g_hook_ready{ false };
jmethodID ORIG_im_sync = nullptr;
jmethodID g_im_sync_mid = nullptr;

void send_slot_now(JNIEnv* env, jobject im, int slot)
{
    if (!C.im_network || !C.slot_packet_class || !C.nh_sendPacket) return;
    jobject nh = env->GetObjectField(im, C.im_network);
    if (!nh) return;
    jobject pkt = env->NewObject(C.slot_packet_class, C.slot_packet_ctor, (jint)slot);
    if (env->ExceptionCheck()) { env->ExceptionClear(); env->DeleteLocalRef(nh); return; }
    if (pkt) {
        env->CallVoidMethod(nh, C.nh_sendPacket, pkt);
        if (env->ExceptionCheck()) env->ExceptionClear();
        env->DeleteLocalRef(pkt);
    }
    env->DeleteLocalRef(nh);
}

void JNICALL hkImSync(JNIEnv* env, jobject thiz)
{
    dt::NativeCallback callbackLifetime;
    // Original first (it may emit the real slot), ours second so it wins.
    if (ORIG_im_sync && C.im_class) {
        env->CallNonvirtualVoidMethod(thiz, C.im_class, ORIG_im_sync);
        if (env->ExceptionCheck()) env->ExceptionClear();
    }
    const int want = g_server_slot.load(std::memory_order_acquire);
    if (want >= 0 && want != g_sent_slot.load(std::memory_order_acquire)) {
        send_slot_now(env, thiz, want);
        g_sent_slot.store(want, std::memory_order_release);
        dbg("hook: sent slot %d from syncSelectedSlot", want);
    }
}

void install_hook(JNIEnv* env)
{
    static bool tried = false;
    if (tried || !C.im_class) return;
    // Latch only AFTER we know we can actually try. Setting it before the VM
    // check meant one early tick with no VM pointer disabled Silent Swap for
    // the whole session, silently falling back to the visible swap.
    if (!dt::jvm::ctx().vm.load()) return;
    tried = true;

    // Candidate names: the intermediary for this has moved across 1.21.x drops,
    // the same way sendPacket and sendMovementPackets did in this codebase.
    static const char* kCandidates[] = { "method_2911", "syncSelectedSlot", "method_2927" };
    jmethodID mid = nullptr;
    const char* used = nullptr;
    for (const char* cand : kCandidates) {
        mid = env->GetMethodID(C.im_class, cand, "()V");
        if (env->ExceptionCheck()) env->ExceptionClear();
        if (mid) { used = cand; break; }
    }
    if (!mid) { VLOG_WARN("autotool", "hook: syncSelectedSlot not found"); return; }

    jnihook_result_t init = JNIHook_Init(dt::jvm::ctx().vm.load());
    if (init != JNIHOOK_OK && init != JNIHOOK_ERR_ADD_JVMTI_CAPS) {
        VLOG_WARN("autotool", "hook: JNIHook_Init failed ({})", (int)init);
        return;
    }

    jnihook_result_t r = JNIHOOK_ERR_JVMTI_OPERATION;
    __try {
        r = JNIHook_Attach(mid, reinterpret_cast<void*>(hkImSync), &ORIG_im_sync);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        VLOG_WARN("autotool", "hook: JNIHook_Attach raised {}",
                  (unsigned long)GetExceptionCode());
        r = JNIHOOK_ERR_JVMTI_OPERATION;
    }
    if (r != JNIHOOK_OK || !ORIG_im_sync) {
        VLOG_WARN("autotool", "hook: attach failed (r={}) — silent swap falls back", (int)r);
        ORIG_im_sync = nullptr;
        return;
    }
    g_im_sync_mid = mid;
    g_hook_ready.store(true, std::memory_order_release);
    VLOG_INFO("autotool", "hook: attached to '{}' — slot packets now ride vanilla's own send site", used);
    dbg("hook: attached via %s", used);
}

// Why the module stopped wanting a tool this frame — printed on the edge so
// one log answers "what exactly is happening".
enum drop_reason {
    R_NONE = 0, R_OFF, R_COND, R_NO_HIT, R_MISS, R_MINING_OFF, R_COMBAT_OFF,
    R_NO_SESSION, R_NO_STATE, R_NO_TOOL, R_RESTORE_REQ
};
const char* reason_name(int r)
{
    switch (r) {
        case R_OFF:         return "MODULE_OFF_OR_GUI";
        case R_COND:        return "CONDITIONS_FAIL";
        case R_NO_HIT:      return "NO_CROSSHAIR_RESULT";
        case R_MISS:        return "LOOKING_AT_AIR";
        case R_MINING_OFF:  return "MINING_SWAP_OFF";
        case R_COMBAT_OFF:  return "COMBAT_SWAP_OFF_OR_NOT_CLICKING";
        case R_NO_SESSION:  return "REQUIRE_CLICK (key up, not breaking, no cooldown)";
        case R_NO_STATE:    return "BLOCKSTATE_UNREADABLE";
        case R_NO_TOOL:     return "NO_BETTER_TOOL_IN_HOTBAR";
        case R_RESTORE_REQ: return "RESTORE_REQUESTED";
        default:            return "NONE";
    }
}

using clock_ms = std::chrono::steady_clock;
long long now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               clock_ms::now().time_since_epoch()).count();
}

} // namespace

class AutoTool : public Module {
public:
    AutoTool()
        : Module("AutoTool", "Swaps to the right tool",
                 Category::Utilities)
    {
        registerSetting(&conditions_);
        registerSetting(&preferredWeapon_);
        registerSetting(&switchDelay_);
        registerSetting(&switchBack_);
        registerSetting(&restoreDelay_);
        // The delay only means anything if we ever go back.
        restoreDelay_.setVisibleIf([this] { return switchBack_.value(); });
        registerSetting(&combatSwap_);
        registerSetting(&miningSwap_);
        registerSetting(&requireClick_);
        registerSetting(&silentSwap_);
        registerSetting(&preferFortune_);
        registerSetting(&preferSilkTouch_);
        registerSetting(&debug_);
    }

    void onInit() override
    {
        listen<events::GameLoopEvent>([this](events::GameLoopEvent&) { tick(); });
    }

    // Every enable starts clean. wants_restore_ used to be able to latch on
    // for good (see the restore branch), and while it is set the module refuses
    // to want any tool at all — so one toggle killed AutoTool until the game
    // restarted.
    // NOTE: Module::drawImGui() is dead code for this UI — the ClickGui builds
    // mirror cards from settings() instead of calling it, so overriding it here
    // achieved nothing. The exclusion lives in tick() and is now visible in the
    // GUI because c_boolean_setting adopts the real value.
    void onEnable()  override { reset_session(); }
    void onDisable() override { wants_restore_ = true; }

private:
    EnumSetting conditions_     { this, "Conditions", { "Always", "Only Sneak" }, 0 };
    EnumSetting preferredWeapon_{ this, "Preferred Weapon", { "Sword", "Axe" }, 0 };
    IntSetting  switchDelay_    { this, "Action Delay MS", 35, 0, 500 };
    BoolSetting switchBack_     { this, "Slot Return", true };
    IntSetting  restoreDelay_   { this, "Restore Delay MS", 35, 0, 500 };
    BoolSetting combatSwap_     { this, "Combat Swap", true };
    BoolSetting miningSwap_     { this, "Mining Swap", true };
    BoolSetting requireClick_   { this, "Require Click", true };
    BoolSetting silentSwap_     { this, "Quick Swap", false };
    // The two are mutually exclusive — an item cannot carry both anyway, and
    // wanting both at once has no meaning. Turning one on turns the other off.
    BoolSetting preferFortune_  { this, "Prefer Fortune", false };
    BoolSetting preferSilkTouch_{ this, "Prefer Silk Touch", true };
    BoolSetting debug_          { this, "Debug Log", false };

    // Swap session state.
    int  spoof_slot_    = -1;   // slot the SERVER currently believes we hold
    int  origin_slot_   = -1;   // slot to go back to
    int  last_real_slot_= -1;   // watches the user scrolling
    long long target_lost_at_ = 0;
    long long desire_since_   = 0;   // when the current desire appeared
    int       desire_slot_    = -1;
    int       sticky_slot_    = -1;
    bool      cond_latched_   = false;
    static constexpr float kEarlyBreak = 0.20f;   // still "before the first hit"
    // NOTE: there is deliberately no rate limit and no grace timer anywhere in
    // this module. Every one of them ended up adding hidden milliseconds on top
    // of Switch/Restore Delay. What has to survive a one-frame blip is scoped to
    // the mining SESSION (the button being held) instead — see tick().
    bool wants_restore_ = false;
    // Silent mining boost: MC accumulates break progress locally from the item
    // you can SEE, so a packet-only swap speeds up nothing. Instead we measure
    // the delta MC just added and top it up to what the tool would have added.
    int   last_age_ = -1;
    float last_progress_ = 0.f;
    bool  boost_seeded_ = false;   // false until we know where this break is at
    float boost_k_ = 1.f;          // toolDelta / handDelta for the current block
    int   boost_k_slot_ = -2;      // slot boost_k_ was measured for

    // ── instrumentation ──
    int   dbg_last_desired_ = -1;
    int   dbg_last_age_ = -1;
    bool  dbg_key_mc_ = false, dbg_key_async_ = false;
    int   dbg_last_attack_cd_ = 0;
    int   visible_written_ = -1;   // slot WE last wrote in the visible path
    bool  user_override_   = false;// user moved the hotbar; stand down
    bool  prev_fortune_ = false, prev_silk_ = true;
    bool  dbg_ench_warned_ = false;
    int   dbg_ench_mask_ = -1, dbg_ench_best_ = -2;
    // Unconditional probe: printed with the periodic state line so one log
    // answers whether best_for_block runs at all and what it reads.
    long long bfb_calls_ = 0;
    jobject bfb_want_ = nullptr;
    bool bfb_silk_ = false, bfb_fort_ = false;
    int  bfb_mask_ = -1, bfb_best_ = -2;
    bool  dbg_held_restore_ = false;
    int   zombie_ticks_ = 0;       // ticks seen in the post-denial zombie state
    int   den_hits_ = 0;           // consecutive non-air samples on a broken block
    int   trace_lines_ = 0;        // per-break cap so a stall cannot flood the log
    float dbg_prev_prog_ = 0.f;
    float hand_delta_ = 0.f;       // MC's own per-tick add, i.e. the bare-hand rate
    float obs_delta_  = 0.f;       // the per-tick add we actually observed
    int   prev_break_cd_ = 0;      // to catch the 0 -> 5 jump that means "broke it"
    // block currently being mined
    bool  trk_valid_ = false;
    int   trk_x_ = 0, trk_y_ = 0, trk_z_ = 0, trk_start_age_ = 0;
    float trk_peak_ = 0.f;
    char  trk_name_[48] = { 0 };
    jobject trk_pos_ = nullptr;    // global ref, the BlockPos we are breaking
    // "did the server actually accept that break?" watch
    jobject den_pos_ = nullptr;    // global ref
    int   den_x_ = 0, den_y_ = 0, den_z_ = 0, den_age_ = 0, den_slot_ = -1;
    long long den_ms_ = 0;

    // ── debug helpers ──
    void block_name_of(JNIEnv* env, jobject state, char* out, size_t n)
    {
        out[0] = 0;
        if (!state || !C.state_getBlock || !C.block_transKey) return;
        jobject blk = env->CallObjectMethod(state, C.state_getBlock);
        if (env->ExceptionCheck()) { env->ExceptionClear(); return; }
        if (!blk) return;
        jstring js = static_cast<jstring>(env->CallObjectMethod(blk, C.block_transKey));
        if (env->ExceptionCheck()) env->ExceptionClear();
        if (js) {
            if (const char* c = env->GetStringUTFChars(js, nullptr)) {
                const char* dot = std::strrchr(c, '.');   // block.minecraft.stone -> stone
                std::snprintf(out, n, "%s", dot ? dot + 1 : c);
                env->ReleaseStringUTFChars(js, c);
            }
            env->DeleteLocalRef(js);
        }
        env->DeleteLocalRef(blk);
    }

    bool pos_xyz(JNIEnv* env, jobject bp, int& x, int& y, int& z)
    {
        if (!bp || !C.bp_getX || !C.bp_getY || !C.bp_getZ) return false;
        x = env->CallIntMethod(bp, C.bp_getX);
        y = env->CallIntMethod(bp, C.bp_getY);
        z = env->CallIntMethod(bp, C.bp_getZ);
        if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
        return true;
    }

    void drop_global(JNIEnv* env, jobject& ref)
    {
        if (ref) { env->DeleteGlobalRef(ref); ref = nullptr; }
    }

    void reset_session()
    {
        spoof_slot_ = -1; origin_slot_ = -1; last_real_slot_ = -1;
        target_lost_at_ = 0; wants_restore_ = false;
        sticky_slot_ = -1; desire_slot_ = -1; cond_latched_ = false;
        boost_seeded_ = false; last_progress_ = 0.f; last_age_ = -1;
        trk_valid_ = false; dbg_last_desired_ = -1; boost_k_ = 1.f; boost_k_slot_ = -2;
        visible_written_ = -1; user_override_ = false;
    }

    // Queue the slot for the tick hook. Sending it straight from here would put
    // it after the movement packet — Grim's "Post" window — so the hook drains
    // it at the front of the next tick instead, exactly where vanilla sends it.
    void queue_slot(JNIEnv* env, jobject player, int slot)
    {
        (void)env; (void)player;
        if (g_hook_ready.load(std::memory_order_acquire)) {
            g_server_slot.store(slot, std::memory_order_release);
            dbg("want server slot %d (goes out from syncSelectedSlot)", slot);
            return;
        }
        dbg("hook not ready — slot %d NOT sent (would flag Post)", slot);
    }

    void send_slot(JNIEnv* env, jobject player, int slot)
    {
        jobject nh = env->GetObjectField(player, C.player_network);
        if (!nh) { dbg("send_slot(%d): networkHandler NULL", slot); return; }
        jobject pkt = env->NewObject(C.slot_packet_class, C.slot_packet_ctor, (jint)slot);
        if (env->ExceptionCheck()) {
            env->ExceptionClear(); env->DeleteLocalRef(nh);
            dbg("send_slot(%d): packet ctor THREW", slot);
            return;
        }
        if (!pkt) { env->DeleteLocalRef(nh); dbg("send_slot(%d): packet NULL", slot); return; }
        env->CallVoidMethod(nh, C.nh_sendPacket, pkt);
        const bool threw = env->ExceptionCheck() == JNI_TRUE;
        if (threw) env->ExceptionClear();
        dbg("send_slot(%d): sent%s", slot, threw ? " but sendPacket THREW" : " OK");
        env->DeleteLocalRef(pkt);
        env->DeleteLocalRef(nh);
    }

    // Does this stack carry the enchantment with that id? Walks the stack's
    // ItemEnchantmentsComponent and asks each RegistryEntry whether its id
    // matches — no registry lookup, so nothing to go stale.
    bool has_enchant(JNIEnv* env, jobject stack, jobject id)
    {
        if (!id || !C.stack_getEnchants || !C.ench_getSet || !C.entry_matchesId
            || !C.set_iterator || !C.iter_hasNext || !C.iter_next) return false;
        jobject comp = env->CallObjectMethod(stack, C.stack_getEnchants);
        if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
        if (!comp) return false;
        jobject set = env->CallObjectMethod(comp, C.ench_getSet);
        env->DeleteLocalRef(comp);
        if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
        if (!set) return false;
        jobject it = env->CallObjectMethod(set, C.set_iterator);
        env->DeleteLocalRef(set);
        if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
        if (!it) return false;
        bool found = false;
        while (!found && env->CallBooleanMethod(it, C.iter_hasNext) == JNI_TRUE) {
            if (env->ExceptionCheck()) { env->ExceptionClear(); break; }
            jobject e = env->CallObjectMethod(it, C.iter_next);
            if (env->ExceptionCheck()) { env->ExceptionClear(); break; }
            if (!e) continue;
            found = env->CallBooleanMethod(e, C.entry_matchesId, id) == JNI_TRUE;
            if (env->ExceptionCheck()) env->ExceptionClear();
            env->DeleteLocalRef(e);
        }
        env->DeleteLocalRef(it);
        if (env->ExceptionCheck()) env->ExceptionClear();
        return found;
    }

    // Best hotbar slot for a block: must be able to harvest it, then carry the
    // enchantment you asked to prefer, then be the fastest.
    int best_for_block(JNIEnv* env, jobject inv, jobject state)
    {
        // Preferring one means picking it even over a faster tool — that is the
        // whole point — but never over a tool that can actually harvest.
        jobject wantEnch = preferSilkTouch_.value() ? C.id_silk
                         : (preferFortune_.value()  ? C.id_fortune : nullptr);
        const char* wantName = preferSilkTouch_.value() ? "silk_touch"
                             : (preferFortune_.value() ? "fortune" : "none");
        if ((preferSilkTouch_.value() || preferFortune_.value()) && !wantEnch
            && !dbg_ench_warned_) {
            dbg_ench_warned_ = true;
            dbg("PREFER %s is on but its Identifier never resolved — the "
                "preference cannot do anything", wantName);
        }
        ++bfb_calls_;
        bfb_want_ = wantEnch;
        bfb_silk_ = preferSilkTouch_.value();
        bfb_fort_ = preferFortune_.value();
        int ench_mask = 0;
        int best = -1; float bestSpeed = -1.f;
        bool bestSuitable = false, bestEnch = false;
        for (int i = 0; i < 9; ++i) {
            jobject st = env->CallObjectMethod(inv, C.inv_getStack, (jint)i);
            if (env->ExceptionCheck()) { env->ExceptionClear(); continue; }
            if (!st) continue;
            if (env->CallBooleanMethod(st, C.stack_isEmpty) == JNI_TRUE) { env->DeleteLocalRef(st); continue; }
            const float sp = env->CallFloatMethod(st, C.stack_miningSpeed, state);
            bool suitable = false;
            if (C.stack_suitableFor)
                suitable = env->CallBooleanMethod(st, C.stack_suitableFor, state) == JNI_TRUE;
            if (env->ExceptionCheck()) env->ExceptionClear();
            const bool ench = wantEnch && has_enchant(env, st, wantEnch);
            if (ench) ench_mask |= (1 << i);
            env->DeleteLocalRef(st);
            // Harvest first (a faster tool that drops nothing is useless), then
            // the enchantment you prefer, then raw speed.
            bool better;
            if (suitable != bestSuitable)   better = suitable;
            else if (ench != bestEnch)      better = ench;
            else                            better = sp > bestSpeed;
            if (best < 0 || better) {
                best = i; bestSpeed = sp; bestSuitable = suitable; bestEnch = ench;
            }
        }
        bfb_mask_ = ench_mask;
        bfb_best_ = best;
        if (wantEnch && (ench_mask != dbg_ench_mask_ || best != dbg_ench_best_)) {
            dbg_ench_mask_ = ench_mask; dbg_ench_best_ = best;
            dbg("prefer %s: hotbar slots with it = 0x%03X, picked slot %d "
                "(has it=%d, harvests=%d)",
                wantName, ench_mask, best, (int)bestEnch, (int)bestSuitable);
        }
        return best;
    }

    // Best weapon slot: the preferred category first, the other as a fallback.
    int best_weapon(JNIEnv* env, jobject inv)
    {
        if (!C.stack_isIn || (!C.tag_swords && !C.tag_axes)) return -1;
        const bool wantSword = (preferredWeapon_.index() == 0);
        jobject first  = wantSword ? C.tag_swords : C.tag_axes;
        jobject second = wantSword ? C.tag_axes   : C.tag_swords;

        int hitFirst = -1, hitSecond = -1;
        for (int i = 0; i < 9; ++i) {
            jobject st = env->CallObjectMethod(inv, C.inv_getStack, (jint)i);
            if (env->ExceptionCheck()) { env->ExceptionClear(); continue; }
            if (!st) continue;
            if (env->CallBooleanMethod(st, C.stack_isEmpty) != JNI_TRUE) {
                if (hitFirst < 0 && first
                    && env->CallBooleanMethod(st, C.stack_isIn, first) == JNI_TRUE) hitFirst = i;
                else if (hitSecond < 0 && second
                    && env->CallBooleanMethod(st, C.stack_isIn, second) == JNI_TRUE) hitSecond = i;
            }
            if (env->ExceptionCheck()) env->ExceptionClear();
            env->DeleteLocalRef(st);
            if (hitFirst >= 0) break;
        }
        return hitFirst >= 0 ? hitFirst : hitSecond;
    }

    // How much faster slot `tool` breaks this block than the slot we visibly
    // hold. MC's delta is speed / hardness / (canHarvest ? 30 : 100), so the
    // hardness cancels and only the speed and the harvest divisor remain.
    float break_ratio(JNIEnv* env, jobject inv, jobject state, int tool, int hand)
    {
        if (tool == hand) return 1.f;
        auto probe = [&](int slot, float& speed, bool& harvest) {
            speed = 1.f; harvest = false;
            jobject st = env->CallObjectMethod(inv, C.inv_getStack, (jint)slot);
            if (env->ExceptionCheck()) { env->ExceptionClear(); return; }
            if (!st) return;
            speed = env->CallFloatMethod(st, C.stack_miningSpeed, state);
            if (C.stack_suitableFor)
                harvest = env->CallBooleanMethod(st, C.stack_suitableFor, state) == JNI_TRUE;
            if (env->ExceptionCheck()) env->ExceptionClear();
            env->DeleteLocalRef(st);
        };
        float sTool = 1.f, sHand = 1.f; bool hTool = false, hHand = false;
        probe(tool, sTool, hTool);
        probe(hand, sHand, hHand);
        if (sHand <= 0.0001f) sHand = 1.f;
        // Vanilla's divisor is canHarvest ? 30 : 100, and PlayerEntity.canHarvest
        // (method_7305) is:
        //     !state.isToolRequired() || mainHandStack.isSuitableFor(state)
        // — so for a block that does NOT require a tool (wood, dirt, sand, wool,
        // leaves, chests...) BOTH sides get 30 no matter what you hold. Using
        // isSuitableFor on its own added a bogus 100/30 = 3.33x on every one of
        // them: an axe on oak finished in 2 ticks instead of 7, and the server
        // then scored the break as 0.15 * 3 = 0.45 against its 0.70 threshold
        // and refused it.
        bool toolReq = true;
        if (C.state_toolReq) {
            toolReq = env->CallBooleanMethod(state, C.state_toolReq) == JNI_TRUE;
            if (env->ExceptionCheck()) { env->ExceptionClear(); toolReq = true; }
        }
        const bool canTool = !toolReq || hTool;
        const bool canHand = !toolReq || hHand;
        const float k = (sTool / sHand) * ((canHand ? 30.f : 100.f) / (canTool ? 30.f : 100.f));
        return (k > 1.f && k < 1000.f) ? k : 1.f;
    }

    void tick()
    {
        dt::jvm::JniScope s;
        if (!s) return;
        JNIEnv* env = s.get();
        g_dbg_on = debug_.value();
        if (!init(env)) { dbg("init not ready"); return; }
        install_hook(env);

        bind_enchants(env);   // cheap no-op once bound; never latch it off

        // Prefer Fortune / Prefer Silk Touch are mutually exclusive: whichever
        // you just switched ON wins, and the other goes off.
        if (preferFortune_.value() && preferSilkTouch_.value()) {
            if (!prev_fortune_)      preferSilkTouch_.set(false);
            else if (!prev_silk_)    preferFortune_.set(false);
            else                     preferFortune_.set(false);
        }
        prev_fortune_ = preferFortune_.value();
        prev_silk_    = preferSilkTouch_.value();

        // Never act while the ClickGui owns the screen.
        const bool live = isEnabled() && !::dt::vgui::bridge::isOverlayVisible()
                       && !::dt::vgui::bridge::isEnabled("Freecam");   // the crosshair is the camera's

        jobject mc = env->CallStaticObjectMethod(C.mc_class, C.mc_getInstance);
        if (env->ExceptionCheck()) { env->ExceptionClear(); return; }
        if (!mc) return;
        jobject player = env->GetObjectField(mc, C.mc_player);
        if (!player) { env->DeleteLocalRef(mc); reset_session(); return; }
        jobject inv = env->GetObjectField(player, C.player_inventory);
        if (!inv) { env->DeleteLocalRef(player); env->DeleteLocalRef(mc); return; }

        const int realSlot = env->GetIntField(inv, C.inv_selected);
        const long long now = now_ms();
        const int  age = C.ent_age ? env->GetIntField(player, C.ent_age) : -1;
        const bool tick_edge = (age != dbg_last_age_);

        // The user scrolling moves selectedSlot, which makes vanilla's own sync
        // send that slot and quietly undo a silent spoof — re-assert it.
        if (spoof_slot_ >= 0 && last_real_slot_ >= 0 && realSlot != last_real_slot_) {
            origin_slot_ = realSlot;
            // Vanilla just sent the real slot from its own sync, so force ours
            // back onto the wire even though the intent has not changed.
            g_sent_slot.store(-1, std::memory_order_release);
            queue_slot(env, player, spoof_slot_);
            dbg("user scrolled to %d — re-asserting spoof %d", realSlot, spoof_slot_);
        }
        // The visible path writes selectedSlot. If it then reads back as
        // something else, the user scrolled or hit a hotbar key — writing ours
        // back on the next frame is what locked the hotbar. Stand down until the
        // target actually changes; their hand beats ours.
        if (visible_written_ >= 0 && realSlot != visible_written_) {
            user_override_ = true;
            origin_slot_   = realSlot;
            visible_written_ = -1;
            dbg("user moved the hotbar to %d — standing down until the target changes",
                realSlot);
        }
        last_real_slot_ = realSlot;

        // ── raw inputs, read once per frame ──────────────────────────────────
        jobject im0 = env->GetObjectField(mc, C.mc_interaction);
        bool    breaking_now  = false;
        float   break_progress = 0.f;
        int     break_cd = 0;
        jobject break_pos = nullptr;
        if (im0) {
            breaking_now = env->CallBooleanMethod(im0, C.im_isBreaking) == JNI_TRUE;
            if (C.im_progress) break_progress = env->GetFloatField(im0, C.im_progress);
            if (C.im_cooldown) break_cd       = env->GetIntField(im0, C.im_cooldown);
            if (C.im_breakPos) break_pos      = env->GetObjectField(im0, C.im_breakPos);
            if (env->ExceptionCheck()) env->ExceptionClear();
        }

        // MC's OWN attack key is the ground truth: GameOptions.attackKey.pressed
        // is the exact boolean MinecraftClient.handleBlockBreaking() is called
        // with. GetAsyncKeyState is kept only so the log can show a mismatch.
        int attack_cd = 0;
        if (C.mc_attackCd) {
            attack_cd = env->GetIntField(mc, C.mc_attackCd);
            if (env->ExceptionCheck()) env->ExceptionClear();
        }

        // Which block MC is actually mining right now (field_3714). Needed to
        // stop a one-frame glance at a neighbour from stealing the swap.
        int  cbx = 0, cby = 0, cbz = 0;
        const bool cb_valid = pos_xyz(env, break_pos, cbx, cby, cbz);
        const bool break_live = breaking_now || break_progress > 0.f;
        // Vanilla's blockBreakingCooldown is 5 ticks in which method_2902 returns
        // at its offset 21 — NOTHING can be mined. Meanwhile we have already
        // predicted the block to air, so the raycast falls straight through it
        // onto whatever is behind (the soul_sand in your log). Treat that window
        // as still belonging to the block we just broke: there is nothing to gain
        // by changing tools in it, and changing them cost a redundant pair of
        // slot packets per block and clobbered the boost rate.
        const bool break_owns = break_live || break_cd > 0;

        const bool key_async = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        bool key_mc = key_async;
        bool key_mc_ok = false;
        if (C.mc_options && C.opt_attackKey && C.key_pressed) {
            jobject opts = env->GetObjectField(mc, C.mc_options);
            if (opts) {
                jobject ak = env->GetObjectField(opts, C.opt_attackKey);
                if (ak) {
                    key_mc = env->GetBooleanField(ak, C.key_pressed) == JNI_TRUE;
                    key_mc_ok = true;
                    env->DeleteLocalRef(ak);
                }
                env->DeleteLocalRef(opts);
            }
            if (env->ExceptionCheck()) env->ExceptionClear();
        }
        if (attack_cd > 0 && dbg_last_attack_cd_ == 0)
            dbg("MC STALL: attackCooldown=%d — handleBlockBreaking() will do NOTHING "
                "for %d ticks (doAttack sets it to 10 when you click at air)",
                attack_cd, attack_cd);
        dbg_last_attack_cd_ = attack_cd;

        if (key_mc != key_async && (key_mc != dbg_key_mc_ || key_async != dbg_key_async_))
            dbg("KEY MISMATCH: mc=%d async=%d  (mc is the one that drives mining)",
                (int)key_mc, (int)key_async);
        dbg_key_mc_ = key_mc; dbg_key_async_ = key_async;

        const bool clicking_now = key_mc;
        // ONE mining session. blockBreakingCooldown is MC's OWN field: it is set
        // to 5 the instant a block breaks (class_636 method_2902, offset 367) and
        // for those 5 ticks isBreakingBlock() reads false even though you never
        // let go of the button. Riding that out with MC's own state is not a
        // timer of mine — the delays you set stay the only milliseconds here.
        // A session is the attack key being held, full stop.
        // blockBreakingCooldown must NOT be in here: it is only decremented
        // inside method_2902, and handleBlockBreaking only calls that while the
        // key is down — so the moment you let go it FREEZES at whatever it was.
        // With it in the condition the session never ended, which killed Switch
        // Back entirely and left user_override_ latched so the swap to the tool
        // never fired again. It was never needed either: during that cooldown
        // the key is still held, so clicking_now already covers the gap between
        // two blocks.
        const bool session = clicking_now || breaking_now;
        // Your hand beats ours for the rest of THIS hold, not for ever. A new
        // click is a new decision.
        if (!session) user_override_ = false;

        // Conditions gate: everything below only runs when they hold.
        bool condOk = true;
        if (conditions_.index() == 1) {
            condOk = C.ent_isSneaking
                  && env->CallBooleanMethod(player, C.ent_isSneaking) == JNI_TRUE;
        }
        if (env->ExceptionCheck()) env->ExceptionClear();
        // isSneaking() drops for a frame here and there and that blinked the
        // whole module off mid-block. Hold it for the session only: let go of
        // the button and it releases the same frame.
        if (condOk) cond_latched_ = true;
        else if (session && cond_latched_) condOk = true;
        else cond_latched_ = false;

        // ── what do we want to be holding? ──────────────────────────────────
        int  desired = -1;
        int  reason  = R_NONE;
        char tgt_name[48]; tgt_name[0] = 0;
        int  tgt_x = 0, tgt_y = 0, tgt_z = 0; bool tgt_valid = false;

        if (!live)        reason = R_OFF;
        else if (!condOk) reason = R_COND;
        else {
            jobject hit = env->GetObjectField(mc, C.mc_crosshair);
            if (!hit) reason = R_NO_HIT;
            else {
                jobject type = env->CallObjectMethod(hit, C.hit_getType);
                const bool isBlock  = type && C.hit_type_block
                                   && env->IsSameObject(type, C.hit_type_block) == JNI_TRUE;
                const bool isEntity = type && C.hit_type_entity
                                   && env->IsSameObject(type, C.hit_type_entity) == JNI_TRUE;
                if (type) env->DeleteLocalRef(type);

                // Require Click ON: only an actual press arms the tool, so
                // merely LOOKING at a block never swaps. Once armed, the mining
                // session keeps it — vanilla's 5-tick blockBreakingCooldown
                // included — so a chain of blocks never loses the tool between
                // hits. OFF: looking at the block is enough, as before.
                const bool armed = !requireClick_.value()
                                || clicking_now
                                || (origin_slot_ >= 0 && session);

                if (isBlock && !miningSwap_.value())        reason = R_MINING_OFF;
                else if (isEntity && !combatSwap_.value())  reason = R_COMBAT_OFF;
                else if (!isBlock && !isEntity)             reason = R_MISS;
                else if (isBlock) {
                    if (!armed) reason = R_NO_SESSION;
                    else {
                        jobject world = env->GetObjectField(mc, C.mc_world);
                        jobject bpos  = env->CallObjectMethod(hit, C.bhr_getBlockPos);
                        if (env->ExceptionCheck()) env->ExceptionClear();
                        // THE block to pick a tool for is the one MC is feeding
                        // calcBlockBreakingDelta with — class_636.field_3714 —
                        // not the crosshair. They disagree constantly: the ray
                        // passes through the block the frame we predict it to
                        // air, it clips a neighbour at the edges, and during the
                        // post-break cooldown it is already on the next one. Any
                        // of those made us pick the tool for a block we were not
                        // mining, and the real one then went bare-handed. This
                        // is the "sometimes it mines with the hand, depends
                        // where I look".
                        jobject aim = (break_live && cb_valid && break_pos) ? break_pos : bpos;
                        if (world && aim) {
                            tgt_valid = pos_xyz(env, aim, tgt_x, tgt_y, tgt_z);
                            jobject state = env->CallObjectMethod(world, C.world_getBlockState, aim);
                            if (env->ExceptionCheck()) env->ExceptionClear();
                            if (!state) reason = R_NO_STATE;
                            else {
                                block_name_of(env, state, tgt_name, sizeof tgt_name);
                                // A break already under way OWNS the swap. Between
                                // two hits the crosshair grazes a neighbour for a
                                // frame or two (soul_sand in your log) and picking
                                // the shovel for THAT hands the server a different
                                // item right as the STOP for the block we are
                                // really mining goes out — and it moved boost_k_
                                // to 1.0 mid-block, so the boost rate flipped too.
                                const bool other_block = break_owns && cb_valid && tgt_valid
                                                      && (tgt_x != cbx || tgt_y != cby || tgt_z != cbz);
                                if (other_block && spoof_slot_ >= 0) {
                                    desired = spoof_slot_;      // keep mining what we started
                                } else {
                                    desired = best_for_block(env, inv, state);
                                    if (desired < 0) reason = R_NO_TOOL;
                                    // Only trust the crosshair for the boost rate
                                    // when no break owns it; the authoritative
                                    // value is taken from currentBreakingPos the
                                    // moment the break actually starts.
                                    else if (silentSwap_.value() && !break_owns)
                                        boost_k_ = break_ratio(env, inv, state, desired, realSlot);
                                }
                                env->DeleteLocalRef(state);
                                // Changing the held stack RESETS block-breaking
                                // progress (method_2922 compares field_3718
                                // against the live main-hand stack), so the
                                // VISIBLE path must not switch once a bar is
                                // actually up. It is keyed on the PROGRESS, not
                                // on origin_slot_: that was left over from an
                                // earlier session, so the very first tick of a
                                // fresh break already pinned `desired` to
                                // whatever was in hand and the whole block then
                                // went bare-handed. At progress ~0 there is
                                // nothing to lose — attackBlock zeroes it anyway.
                                if (breaking_now && !silentSwap_.value()
                                    && break_progress > kEarlyBreak)
                                    desired = realSlot;
                            }
                        } else {
                            reason = R_NO_STATE;
                        }
                        if (bpos)  env->DeleteLocalRef(bpos);
                        if (world) env->DeleteLocalRef(world);
                    }
                } else {   // entity
                    if (requireClick_.value() && !clicking_now) reason = R_COMBAT_OFF;
                    else {
                        desired = best_weapon(env, inv);
                        if (desired < 0) reason = R_NO_TOOL;
                    }
                }
                env->DeleteLocalRef(hit);
            }
        }

        if (wants_restore_) { desired = -1; reason = R_RESTORE_REQ; }

        // Hard rule: while a break is actually running, keep the slot we already
        // gave the server. Dropping it mid-block is what made the server finish
        // the block with the bare hand.
        if (breaking_now && spoof_slot_ >= 0 && desired < 0 && !wants_restore_)
            desired = spoof_slot_;

        // The crosshair blips to air between two blocks and isBreakingBlock()
        // goes false for vanilla's post-break cooldown. Scoped to the session,
        // never to a duration: held → keep the tool for the next block;
        // released → the desire is gone this very frame.
        if (desired >= 0) {
            sticky_slot_ = desired;
        } else if (sticky_slot_ >= 0 && session && !wants_restore_) {
            // ONLY inside a live mining session. Letting this branch run with
            // Require Click off made it permanent — sticky_slot_ could never
            // reach the reset below, so `desired` stayed pinned to the last tool
            // for ever, even while looking at air, and the swap below then
            // rewrote the hotbar slot every single frame. With Require Click off
            // nothing is lost: looking at the block recomputes `desired`
            // directly, and looking away must drop it so Restore Delay can run.
            desired = sticky_slot_;
        } else {
            sticky_slot_ = -1;
        }

        // ── why did we stop wanting a tool? (edge-triggered) ────────────────
        if (desired < 0 && dbg_last_desired_ >= 0) {
            dbg("DROP (had %d) reason=%s | keyMC=%d%s keyAsync=%d breaking=%d cd=%d "
                "atkCd=%d session=%d cond=%d live=%d | spoof=%d origin=%d target=%s(%d,%d,%d)",
                dbg_last_desired_, reason_name(reason), (int)key_mc,
                key_mc_ok ? "" : "(unavailable)", (int)key_async, (int)breaking_now,
                break_cd, attack_cd, (int)session, (int)condOk, (int)live,
                spoof_slot_, origin_slot_,
                tgt_name[0] ? tgt_name : "-", tgt_x, tgt_y, tgt_z);
        }
        dbg_last_desired_ = desired;

        {
            static long long s_next = 0;
            if (now >= s_next) {
                s_next = now + 1000;
                dbg("prefer: silk=%d fortune=%d | ids silk=%p fortune=%p match=%p "
                    "getEnch=%p compSet=%p",
                    (int)preferSilkTouch_.value(), (int)preferFortune_.value(),
                    (void*)C.id_silk, (void*)C.id_fortune, (void*)C.entry_matchesId,
                    (void*)C.stack_getEnchants, (void*)C.ench_getSet);
                dbg("bestForBlock: calls=%lld | inside it silk=%d fortune=%d want=%p "
                    "| lastMask=0x%03X lastBest=%d",
                    bfb_calls_, (int)bfb_silk_, (int)bfb_fort_, (void*)bfb_want_,
                    bfb_mask_, bfb_best_);
                dbg("state real=%d spoof=%d origin=%d desired=%d serverHas=%d | "
                    "keyMC=%d async=%d breaking=%d breakCd=%d atkCd=%d prog=%.3f | "
                    "cond=%d live=%d silent=%d hook=%d",
                    realSlot, spoof_slot_, origin_slot_, desired,
                    g_sent_slot.load(std::memory_order_acquire),
                    (int)key_mc, (int)key_async, (int)breaking_now, break_cd, attack_cd,
                    break_progress, (int)condOk, (int)live, (int)silentSwap_.value(),
                    (int)g_hook_ready.load(std::memory_order_acquire));
            }
        }

        const int effective = (spoof_slot_ >= 0) ? spoof_slot_ : realSlot;

        // NEVER hand the server back the real slot while a break is in flight.
        // Verified in class_3225.method_14263: on STOP_DESTROY the server scores
        // the WHOLE break as calcBlockBreakingDelta(live held item) * (ticks+1)
        // and needs >= 0.7f. Our boost finishes the bar on the render thread and
        // MC only emits STOP on its NEXT tick, so a restore landing in that gap
        // makes the server score the break with the empty hand (~0.04) and deny
        // it. It then sets failedToMine, and class_3225.method_14264 treats that
        // as a PRIORITY branch: until the denied block finally grinds out at hand
        // speed, every following block gets ZERO server-side progress. That is
        // exactly "the next one is mined with the hand" — and exactly why the
        // visible swap never showed it, since there the client holds the real
        // tool and client and server always agree.
        // ...but only while the key is actually down. After a refused break the
        // progress field just sits there frozen (0.853 with breaking=0 and the
        // key up, all over your log), and gating on it alone would have parked
        // Silent Swap's restore for ever. Let go of the button and MC is not
        // going to send a STOP_DESTROY, so there is nothing left to protect.
        const bool break_in_flight = breaking_now
                                  || (break_progress > 0.f && clicking_now);

        // With the session rule above this sends at most one slot per session —
        // tool on the first hit, real slot on release — which is what a player
        // who picks the right tool looks like anyway. So no rate limit: every
        // floor I put here outlived your delays and blocked the NEXT block's
        // swap, which is what left it to the bare hand.
        // Silent Swap only emits a packet, so it cannot reset MC's own progress
        // and is free to correct the server's slot at any point in a break.
        // The visible path must not change slots mid-break (that DOES reset the
        // progress bar), except right at the start.
        const bool may_send = silentSwap_.value()
                           || !breaking_now
                           || break_progress < kEarlyBreak;

        if (desired >= 0) {
            target_lost_at_ = 0;
            // Switch Delay is measured from when THIS desire first appeared.
            if (desired != desire_slot_) {
                desire_slot_ = desired; desire_since_ = now;
                user_override_ = false;      // new target, our turn again
            }
            if (desired != effective && !user_override_ && may_send
                && now - desire_since_ >= switchDelay_.value()) {
                if (origin_slot_ < 0) origin_slot_ = realSlot;
                const bool canSilent = silentSwap_.value()
                                    && g_hook_ready.load(std::memory_order_acquire);
                if (canSilent) {
                    queue_slot(env, player, desired);   // server only; client untouched
                    spoof_slot_ = desired;
                } else {
                    env->SetIntField(inv, C.inv_selected, (jint)desired);
                    spoof_slot_ = -1;                  // vanilla syncs this one itself
                    last_real_slot_ = desired;
                    visible_written_ = desired;
                }
                dbg("swap -> %d after %lldms (switch delay=%d, silent=%d, real=%d, target=%s)",
                    desired, now - desire_since_, (int)switchDelay_.value(),
                    (int)silentSwap_.value(), realSlot, tgt_name[0] ? tgt_name : "-");
            }
        } else {
            desire_slot_ = -1;
            user_override_ = false;
            if (target_lost_at_ == 0) target_lost_at_ = now;
            const long long idle = restoreDelay_.value();
            // The desire is only ever -1 here because the session ended, so this
            // fires exactly Restore Delay after that — nothing else gates it.
            // Switch Back off: keep whatever the tool swap left us with. Only a
            // module-level stand-down (toggled off / left the world) still puts
            // the original slot back.
            const bool hold_for_break = (break_in_flight && spoof_slot_ >= 0)
                                     || (!switchBack_.value() && !wants_restore_);
            if (hold_for_break && !dbg_held_restore_) {
                if (!switchBack_.value())
                    dbg("restore HELD: Slot Return is off — keeping slot %d", realSlot);
                else
                    dbg("restore HELD: a break is still in flight (breaking=%d prog=%.3f) — "
                        "handing the slot back now would make the server deny it",
                        (int)breaking_now, break_progress);
            }
            dbg_held_restore_ = hold_for_break;
            // Nothing was ever swapped, so there is nothing to give back —
            // clear the stand-down instead of latching it for ever.
            if (origin_slot_ < 0 && spoof_slot_ < 0) wants_restore_ = false;
            if ((now - target_lost_at_) >= idle && origin_slot_ >= 0 && !hold_for_break) {
                if (spoof_slot_ >= 0) {
                    queue_slot(env, player, realSlot);  // hand the server back the real slot
                    spoof_slot_ = -1;
                } else if (!silentSwap_.value() && realSlot != origin_slot_) {
                    env->SetIntField(inv, C.inv_selected, (jint)origin_slot_);
                    last_real_slot_ = origin_slot_;
                }
                visible_written_ = -1;      // done driving the hotbar
                dbg("restore after %lldms (restore delay=%lld)",
                    now - target_lost_at_, idle);
                origin_slot_ = -1;
                wants_restore_ = false;
            }
        }

        // ── Silent mining: top up the client's own break progress ───────────
        // The visible item never changes, so MC keeps accumulating at hand
        // speed (verified: method_2902 offset 173 feeds client.player into
        // calcBlockBreakingDelta). Once per TICK we measure what it just added
        // and add the remainder the tool would have contributed.
        if (silentSwap_.value() && spoof_slot_ >= 0 && im0 && C.im_progress && C.ent_age) {
            const float prog = env->GetFloatField(im0, C.im_progress);
            // Gate on the PROGRESS, never on breakingBlock. Verified:
            // class_636.method_2922 (isCurrentlyBreaking) only compares
            // currentBreakingPos and the held stack — it does NOT look at
            // field_3717 — so MC happily keeps adding progress with
            // breakingBlock == false after a refused break. Gating the boost on
            // breakingBlock is exactly what dropped those blocks to bare-hand
            // speed for six seconds.
            if (prog <= 0.f) {
                last_progress_ = 0.f;          // break ended or was cancelled
                boost_seeded_ = false;
            } else if (!boost_seeded_) {
                // Joining a break that is already under way (the spoof came back
                // mid-block). Seeding at 0 makes the first delta the whole bar,
                // so the client finishes far ahead of the server's own maths —
                // the server then DENIES the break and resyncs the block.
                last_progress_ = (prog > 0.05f) ? prog : 0.f;
                boost_seeded_ = true;
                last_age_ = age;
            } else if (age != last_age_) {
                const float delta = prog - last_progress_;
                if (delta > 0.f && delta < 0.5f) hand_delta_ = delta;
                if (delta > 0.f && boost_k_ > 1.f) {
                    float boosted = prog + delta * (boost_k_ - 1.f);
                    if (boosted > 1.f) boosted = 1.f;
                    env->SetFloatField(im0, C.im_progress, boosted);
                    last_progress_ = boosted;
                    break_progress = boosted;
                } else {
                    last_progress_ = prog;
                }
            }
            if (env->ExceptionCheck()) env->ExceptionClear();
            if (age != last_age_) last_age_ = age;
        } else {
            last_progress_ = 0.f;
            boost_seeded_ = false;
        }

        // ── tick-by-tick trace of a live break ──────────────────────────────
        // Six-ish lines per block, and it is the only way to see what MC really
        // did: whether it ever set breakingBlock, whether progress restarted,
        // and how many ticks elapsed between START_DESTROY and STOP_DESTROY
        // (the server scores the break as delta*(ticks+1) >= 0.7).
        if (tick_edge && (breaking_now || break_progress > 0.f)) {
            if (trace_lines_ < 40) {
                ++trace_lines_;
                dbg("  tick age=%d breaking=%d prog=%.3f (was %.3f) breakCd=%d keyMC=%d "
                    "spoof=%d serverHas=%d k=%.2f",
                    age, (int)breaking_now, break_progress, dbg_prev_prog_, break_cd,
                    (int)key_mc, spoof_slot_, g_sent_slot.load(std::memory_order_acquire),
                    boost_k_);
            }
        } else if (tick_edge) {
            trace_lines_ = 0;
        }
        if (tick_edge) {
            if (break_progress > dbg_prev_prog_ && break_progress - dbg_prev_prog_ < 0.5f)
                obs_delta_ = break_progress - dbg_prev_prog_;
            dbg_prev_prog_ = break_progress;
        }

        // ── zombie break: MC mining with breakingBlock == false ─────────────
        // Verified: class_636.method_2922 (isCurrentlyBreaking) is
        //     pos.equals(field_3714) && areItemsAndComponentsEqual(mainHand, field_3718)
        // — it does NOT look at field_3717. So after the server rejects a break,
        // currentBreakingPos still points at the block and MC keeps ADDING
        // progress without ever setting breakingBlock and without sending a new
        // START_DESTROY. The boost is gated on breakingBlock, so the block then
        // grinds out at bare-hand speed — six seconds in your log.
        // Recovery: clear currentBreakingPos so isCurrentlyBreaking fails. MC's
        // own method_2902 then tail-calls attackBlock, which emits a clean
        // START_DESTROY and re-arms breakingBlock. No packet of ours involved.
        // Report the state, but DO NOT touch it. Clearing currentBreakingPos
        // here threw away a live break every three ticks and restarted it — that
        // was the "sometimes it just stops". MC finishes this state on its own
        // (isCurrentlyBreaking still holds, so progress keeps accruing to 1.0 and
        // it re-sends STOP_DESTROY); with the boost now following the progress
        // instead of breakingBlock it finishes at tool speed, which was the only
        // real damage.
        if (tick_edge && !breaking_now && break_progress > 0.f && break_cd == 0 && session) {
            if (++zombie_ticks_ == 3)
                dbg("note: MC is mining with breakingBlock=0 at %.3f (the block came "
                    "back after a refused break). Letting it finish — boost still on.",
                    break_progress);
        } else if (tick_edge) {
            zombie_ticks_ = 0;
        }

        // ── per-block tracker: one summary line per block, once per tick ────
        if (tick_edge) {
            int bx = 0, by = 0, bz = 0;
            const bool has_pos = pos_xyz(env, break_pos, bx, by, bz);
            const bool same    = trk_valid_ && has_pos
                              && bx == trk_x_ && by == trk_y_ && bz == trk_z_;

            // finished / abandoned the block we were tracking
            if (trk_valid_ && (!breaking_now || !same || trk_peak_ >= 0.999f)) {
                const int ticks = age - trk_start_age_;
                // field_3716 is only ever RAISED to 5 on the finish path of
                // method_2902, so the 0 -> 5 jump is what says the block really
                // completed. Testing `break_cd > 0` on its own was wrong: the
                // counter is still winding down from the PREVIOUS block, so a
                // break that had barely started got reported as done — and that
                // armed the denial watch, which then invented a SERVER DENIED.
                // Sampling alone cannot see >= 0.9 either, because MC crosses
                // 1.0 and zeroes the field inside the same tick.
                if (trk_peak_ >= 0.9f || break_cd > prev_break_cd_) {
                    // Exactly what class_3225.method_14263 will score this break
                    // as on STOP_DESTROY: delta(live held item) * (ticks + 1),
                    // which must reach 0.7f or the server refuses the break.
                    // Visible swap: the client holds the real tool, so what we
                    // watched MC add IS the server's rate. Silent Swap: MC adds
                    // at bare-hand rate and we top it up, so the server's rate is
                    // the hand rate times k.
                    const float srv_delta = (silentSwap_.value() && spoof_slot_ >= 0)
                                          ? hand_delta_ * boost_k_ : obs_delta_;
                    const float srv_score = srv_delta * (float)(ticks + 1);
                    dbg("BREAK done (%d,%d,%d) %s in %d ticks | serverSlot=%d k=%.2f "
                        "| server will score %.3f x %d = %.2f (needs >=0.70) -> %s",
                        trk_x_, trk_y_, trk_z_, trk_name_, ticks,
                        spoof_slot_ >= 0 ? spoof_slot_ : realSlot, boost_k_,
                        srv_delta, ticks + 1, srv_score,
                        srv_score >= 0.7f ? "should PASS" : "will FAIL");
                    // Hand the tracked position to the denial watch below.
                    drop_global(env, den_pos_);
                    den_pos_ = trk_pos_; trk_pos_ = nullptr;
                    den_x_ = trk_x_; den_y_ = trk_y_; den_z_ = trk_z_;
                    den_age_ = age; den_ms_ = now; den_hits_ = 0;
                    den_slot_ = spoof_slot_ >= 0 ? spoof_slot_ : realSlot;
                } else {
                    dbg("BREAK ABORTED (%d,%d,%d) %s at %.3f after %d ticks | "
                        "breaking=%d cd=%d keyMC=%d spoof=%d",
                        trk_x_, trk_y_, trk_z_, trk_name_, trk_peak_, ticks,
                        (int)breaking_now, break_cd, (int)key_mc, spoof_slot_);
                }
                trk_valid_ = false;
                drop_global(env, trk_pos_);
            }

            // started a new block
            if (breaking_now && has_pos && !trk_valid_) {
                trk_valid_ = true;
                trk_x_ = bx; trk_y_ = by; trk_z_ = bz;
                trk_start_age_ = age; trk_peak_ = break_progress;
                trk_name_[0] = 0;
                drop_global(env, trk_pos_);
                trk_pos_ = env->NewGlobalRef(break_pos);
                jobject world = env->GetObjectField(mc, C.mc_world);
                if (world) {
                    jobject st = env->CallObjectMethod(world, C.world_getBlockState, break_pos);
                    if (env->ExceptionCheck()) env->ExceptionClear();
                    if (st) {
                        block_name_of(env, st, trk_name_, sizeof trk_name_);
                        // THE authoritative boost rate: computed from the block MC
                        // is really accumulating into (field_3714), against the
                        // slot the server holds vs the one the client shows. The
                        // crosshair cannot be trusted here — it passes through the
                        // block the frame we finish it.
                        // Against the tool we are ABOUT TO hold, not the one the
                        // server still has. At the first tick of a new block the
                        // swap has not gone out yet, so spoof_slot_ is still the
                        // PREVIOUS block's tool — a pickaxe measured against a
                        // cobweb scores 1.0, and since the crosshair recompute is
                        // blocked while a break is live, k stayed at 1.00 for that
                        // block and every one after it. That is the "it mines it
                        // with the hand after switching block type".
                        {
                            const int ktool = (desired >= 0) ? desired : spoof_slot_;
                            if (silentSwap_.value() && ktool >= 0) {
                                boost_k_ = break_ratio(env, inv, st, ktool, realSlot);
                                boost_k_slot_ = ktool;
                            }
                        }
                        env->DeleteLocalRef(st);
                    }
                    env->DeleteLocalRef(world);
                }
                dbg("BREAK start (%d,%d,%d) %s | held=%d serverSlot=%d want=%d k=%.2f boost=%s",
                    bx, by, bz, trk_name_[0] ? trk_name_ : "?", realSlot,
                    spoof_slot_ >= 0 ? spoof_slot_ : realSlot, desired, boost_k_,
                    (silentSwap_.value() && spoof_slot_ >= 0) ? "ON" : "off");
            } else if (trk_valid_ && break_progress > trk_peak_) {
                trk_peak_ = break_progress;
            }

            // The swap for THIS block lands a tick or two AFTER the break
            // starts, so re-measure as soon as the server's slot actually
            // changes — otherwise k keeps describing whatever tool was in play
            // when the block began.
            if (silentSwap_.value() && trk_valid_ && breaking_now && break_pos
                && spoof_slot_ >= 0 && spoof_slot_ != boost_k_slot_) {
                jobject w2 = env->GetObjectField(mc, C.mc_world);
                if (w2) {
                    jobject st2 = env->CallObjectMethod(w2, C.world_getBlockState, break_pos);
                    if (env->ExceptionCheck()) env->ExceptionClear();
                    if (st2) {
                        const float nk = break_ratio(env, inv, st2, spoof_slot_, realSlot);
                        if (nk != boost_k_)
                            dbg("boost k re-measured for slot %d on %s: %.2f -> %.2f",
                                spoof_slot_, trk_name_, boost_k_, nk);
                        boost_k_ = nk;
                        boost_k_slot_ = spoof_slot_;
                        env->DeleteLocalRef(st2);
                    }
                    env->DeleteLocalRef(w2);
                }
            }

            // ── did the SERVER accept that break? ───────────────────────────
            // We predict the block to air locally the moment we finish it. If it
            // comes back, the server refused: STOP_DESTROY needs
            // delta*(ticks+1) >= 0.7 and it evaluates delta with whatever slot IT
            // believes we hold. A bare hand gives ~0.04, so it denies and then
            // re-mines the block itself at hand speed — which is exactly what
            // "it mined it with the hand" looks like from your side.
            if (den_pos_) {
                const int elapsed = age - den_age_;
                jobject world = env->GetObjectField(mc, C.mc_world);
                if (world) {
                    jobject st = env->CallObjectMethod(world, C.world_getBlockState, den_pos_);
                    if (env->ExceptionCheck()) env->ExceptionClear();
                    bool air = true;
                    if (st) {
                        if (C.state_isAir)
                            air = env->CallBooleanMethod(st, C.state_isAir) == JNI_TRUE;
                        if (env->ExceptionCheck()) env->ExceptionClear();
                        env->DeleteLocalRef(st);
                    }
                    env->DeleteLocalRef(world);
                    // One non-air sample is not proof: on a SUCCESSFUL break the
                    // client first rolls the predicted block back when the
                    // sequence ack lands and only then applies the server's real
                    // update, so the block legitimately blinks back for a tick.
                    // Only a block that STAYS is a refusal.
                    if (!air) ++den_hits_; else den_hits_ = 0;
                    if (den_hits_ >= 4 && elapsed >= 2) {
                        dbg("!! SERVER DENIED the break: (%d,%d,%d) is still there %d ticks "
                            "(%lldms) after we broke it. serverSlot at break was %d, "
                            "k=%.2f. The server re-mines it itself and gives every LATER "
                            "block zero progress until it pops (failedToMine).",
                            den_x_, den_y_, den_z_, elapsed, now - den_ms_, den_slot_,
                            boost_k_);
                        drop_global(env, den_pos_);
                        den_hits_ = 0;
                    } else if (elapsed > 40) {
                        den_hits_ = 0;
                        drop_global(env, den_pos_);   // stayed broken: server accepted
                    }
                }
            }
        }
        // Self-heal. The intent is declarative, so if a send was ever lost the
        // hook puts it back on the wire at vanilla's next sync instead of
        // leaving the server on the wrong item for the rest of the block.
        if (spoof_slot_ >= 0)
            g_server_slot.store(spoof_slot_, std::memory_order_release);

        if (tick_edge) { dbg_last_age_ = age; prev_break_cd_ = break_cd; }

        if (break_pos) env->DeleteLocalRef(break_pos);
        if (im0)       env->DeleteLocalRef(im0);
        env->DeleteLocalRef(inv);
        env->DeleteLocalRef(player);
        env->DeleteLocalRef(mc);
        if (env->ExceptionCheck()) env->ExceptionClear();
    }
};

} // namespace dt::modules

DT_REGISTER_MODULE(dt::modules::AutoTool)
