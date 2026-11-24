#include "core/NativeCallbacks.h"
#include "chams_item.h"
#include "chams_debug.h"
#include "chams_gl.h"
#include "chams_jni.h"   // self_entity_id / self_skin_gl

#include "jvm/JniHelper.h"
#include "jvm/JvmContext.h"

#include <windows.h>
#include <jnihook.h>

#include <atomic>

namespace vanta::modules::chams::item {

namespace {

constexpr const char* kImmediate = "net/minecraft/class_4597$class_4598";  // VertexConsumerProvider$Immediate
constexpr const char* kDraw      = "method_22993";                        // Immediate.draw()

jclass    g_immediate_class = nullptr;   // global ref, may be null
jmethodID g_draw            = nullptr;

std::atomic<bool> g_installed{ false };
std::atomic<bool> g_active{ false };

std::atomic<unsigned long long> c_renders{ 0 };
std::atomic<unsigned long long> c_flushes{ 0 };
std::atomic<unsigned long long> c_no_flush{ 0 };

// ── The two feature renderers a player's equipment goes through ──────────────
//
// Both class names and both method names are STABLE across 1.21 .. 1.21.11 in
// this repo's cached Yarn tables; only the parameter shapes move, and every
// shape they take is listed below and probed in order.
//
//   class_989.method_17162   HeldItemFeatureRenderer.render   sword, shield, ...
//   class_970.method_17157   ArmorFeatureRenderer.render      helmet .. boots
//
// Armour is here even though armour TEXTURES are already resolved by name in
// chams_jni.cpp, and the reason is the enchantment glint. Minecraft draws a
// glint as a second pass over the same geometry, masked by glDepthFunc(EQUAL):
// it paints only where the depth matches exactly what the piece underneath
// left behind. Our replay puts the armour in the depth band, so the game's own
// armour draw then fails LEQUAL and writes no real depth -- and the glint,
// arriving at real depth with nothing to match, disappears along with it. The
// glint texture is not an armour texture, so texture identity could not rescue
// it either. Bracketing the whole armour render puts the piece AND its glint in
// the band together, where the EQUAL test finds what it is looking for.
//
// That is exactly why the held item's glint already works: the item bracket
// covers everything the item render emits, glint included.
struct variant { const char* sig; int floats; const char* range; };

const variant kHeldVariants[] = {
    { "(Lnet/minecraft/class_4587;Lnet/minecraft/class_4597;ILnet/minecraft/class_10426;FF)V",
      2, "1.21.4 .. 1.21.8" },
    { "(Lnet/minecraft/class_4587;Lnet/minecraft/class_11659;ILnet/minecraft/class_10426;FF)V",
      2, "1.21.9+" },
    { "(Lnet/minecraft/class_4587;Lnet/minecraft/class_4597;ILnet/minecraft/class_10042;FF)V",
      2, "1.21.2 .. 1.21.3" },
    { "(Lnet/minecraft/class_4587;Lnet/minecraft/class_4597;ILnet/minecraft/class_1309;FFFFFF)V",
      6, "1.21 .. 1.21.1" },
};

const variant kArmourVariants[] = {
    { "(Lnet/minecraft/class_4587;Lnet/minecraft/class_4597;ILnet/minecraft/class_10034;FF)V",
      2, "1.21.2 .. 1.21.8" },
    { "(Lnet/minecraft/class_4587;Lnet/minecraft/class_11659;ILnet/minecraft/class_10034;FF)V",
      2, "1.21.9+" },
    { "(Lnet/minecraft/class_4587;Lnet/minecraft/class_4597;ILnet/minecraft/class_1309;FFFFFF)V",
      6, "1.21 .. 1.21.1" },
};

jclass    g_held_class   = nullptr;   jmethodID ORIG_held   = nullptr;
jclass    g_armour_class = nullptr;   jmethodID ORIG_armour = nullptr;

// ── 1.21.9+ : THE DEFERRED COMMAND QUEUE ────────────────────────────────────
//
// From 1.21.9 the feature renderers no longer draw. They SUBMIT to a queue and
// the geometry is emitted later, in bulk, by the command renderers. A flush
// around the feature renderer therefore brackets nothing, which is why the
// hooks above go inert there.
//
// What makes a fix possible is a detail found by reading the class files rather
// than assuming:
//
//   OrderedRenderCommandQueueImpl.getBatchingQueues() -> Int2ObjectAVLTreeMap
//
// The batching queues live in an INT-KEYED SORTED MAP. The key is an arbitrary
// ordering value, not an index into anything fixed, so a queue can be created
// under a key of our own choosing and it simply renders in key order.
//
//   1. While a player's equipment is being submitted, redirect that submission
//      into a batching queue under OUR key. That queue then contains player
//      equipment and nothing else.
//   2. ItemCommandRenderer.render takes the BatchingRenderCommandQueue it is
//      about to emit -- AND the VertexConsumerProvider.Immediate to emit into.
//      When the queue it is handed is ours, bracket that call with the same two
//      flushes used above.
//
// So the scoping the flush cannot provide at submit time is recovered at render
// time, from the game's own batching.
//
// One fact here had to come from the jar, because the mapping tables cannot
// express it: RenderCommandQueue is an INTERFACE and its submitItem is
// abstract. Hooking it would have intercepted nothing. The concrete
// implementation lives on BatchingRenderCommandQueue (class_11788), and that is
// what is detoured.
constexpr const char* kBatchQueue  = "net/minecraft/class_11788";  // BatchingRenderCommandQueue
constexpr const char* kSubmitItem  = "method_73480";
constexpr const char* kSubmitItemSig =
    "(Lnet/minecraft/class_4587;Lnet/minecraft/class_811;III[ILjava/util/List;"
    "Lnet/minecraft/class_1921;Lnet/minecraft/class_10444$class_10445;)V";

constexpr const char* kItemCmdRenderer = "net/minecraft/class_11687";  // ItemCommandRenderer
constexpr const char* kIcrRender       = "method_73010";
constexpr const char* kIcrRenderSig =
    "(Lnet/minecraft/class_11788;Lnet/minecraft/class_4597$class_4598;"
    "Lnet/minecraft/class_4618;)V";

constexpr const char* kOrderedQueue = "net/minecraft/class_11659";  // OrderedRenderCommandQueue
constexpr const char* kGetBatching  = "method_73529";
constexpr const char* kGetBatchingSig = "(I)Lnet/minecraft/class_11785;";

// Our ordering key. Any int is legal -- the queues are a sorted map -- and a
// large positive one puts player equipment last, which is where a replayed copy
// wants to be anyway.
constexpr jint kVantaOrder = 0x40000000;

// An item is not submitted through one door. A sword goes to submitItem, a
// BLOCK held in the hand goes to submitBlockStateModel, and armour pieces and
// shields go to submitModelPart. Redirecting only submitItem is why some items
// came out right and others did not -- the ones that missed were not culled,
// they were never steered into our queue at all.
jclass    g_bq_class    = nullptr;
jmethodID ORIG_submitItem = nullptr;
jmethodID ORIG_submitBSM  = nullptr;         // submitBlockStateModel
jmethodID ORIG_submitMP1  = nullptr;         // submitModelPart, four overloads
jmethodID ORIG_submitMP2  = nullptr;
jmethodID ORIG_submitMP3  = nullptr;
jmethodID ORIG_submitMP4  = nullptr;

// ARMOUR'S ACTUAL DOOR, and the reason hkArmour2 was a no-op on 1.21.9+.
//
// The armour bracket set g_ordered faithfully and every submission inside it
// went through a method nobody had hooked. EquipmentRenderer submits each layer
// as getBatchingQueue(idx++) followed by submitModel -- method_73490, declared
// concretely on BatchingRenderCommandQueue and absent from the six that were
// attached. Nothing in the log could show it: the bracket counted, the flush
// ran, and the queue it was guarding stayed empty.
//
// So armour lived on texture identity alone, which reaches the base layer and
// nothing else -- not the leather overlay, not the trims (drawn from a sprite
// atlas, so no per-trim texture exists to name), not the enchantment glint. And
// an un-replayed layer is not merely un-chammed: the body's own band footprint
// deletes it. That is "the armour, like it does not exist".
//
// The 8-arg method_73489 needs no hook of its own; its entire body widens into
// this one.
jmethodID ORIG_submitModel = nullptr;

jclass    g_icr_class   = nullptr;   jmethodID ORIG_icrRender  = nullptr;
jclass    g_mcr_class   = nullptr;   jmethodID ORIG_mcrRender  = nullptr;   // ModelCommandRenderer
jclass    g_mpr_class   = nullptr;   jmethodID ORIG_mprRender  = nullptr;   // ModelPartCommandRenderer
jmethodID g_get_batching = nullptr;
jclass    g_ocq_class    = nullptr;

// The OrderedRenderCommandQueue handed to a feature renderer, live only for the
// duration of that call. Render happens on one thread, so a plain pointer is
// enough and an atomic would only obscure that.
// ── OWNERSHIP, DERIVED FROM THE RENDER STATE ────────────────────────────────
//
// The redirect used to gate on gl::rendering_player(), which chams_cull writes
// from EntityRenderDispatcher.shouldRender. Disassembling the 1.21.11 jar
// showed why that could not work here: shouldRender is called only from the
// EXTRACT pass, and every feature-renderer submission happens in the separate
// SUBMIT pass, which never calls it. The two cannot interleave, so at submit
// time the flag holds ONE value for the entire frame -- "was the last entity
// the extract loop touched a player".
//
// All-or-nothing per frame is exactly the reported symptom. Flag false and no
// player's equipment is redirected at all, so the item draws at real depth and
// is deleted by the band its owner's own body already wrote. Flag true and
// EVERY living entity's equipment is redirected, the local player's included.
// And a frame that redirects nothing reconciles clean, which is how lostFrames
// stayed at 0 while items vanished.
//
// The render state is the right signal because it arrives as the 4th argument
// of the very call being bracketed. class_10055 (PlayerEntityRenderState)
// extends class_10034 extends class_10426, so both hooked feature renderers
// receive it for a player, and field_53528 is assigned Entity.getId() -- an
// exact per-entity identity AND an exact self test, with no frame-global
// assumption anywhere.
jclass    g_pstate_class = nullptr;    // class_10055, global ref
jfieldID  g_pstate_id    = nullptr;    // field_53528, the entity id

// Set for the duration of one feature-renderer call: the entity id whose
// equipment is being submitted, or -1 when the state was not a player's.
int       g_owner_id = -1;

std::atomic<unsigned long long> c_self_skipped{ 0 };

jobject   g_ordered   = nullptr;
jobject   g_our_queue = nullptr;   // global ref; the queue our redirects go to

std::atomic<unsigned long long> c_redirects{ 0 };
std::atomic<unsigned long long> c_queue_brackets{ 0 };

// Per-frame reconciliation. A frame that redirected a player's equipment into
// our queue and then never saw that queue come back to be emitted is the exact
// shape of "the item vanished": the geometry went somewhere nothing drew it.
// Counting it separates a stale queue reference from every other theory.
std::atomic<unsigned long long> c_frames_redirect_no_bracket{ 0 };
unsigned long long f_redirects = 0;
unsigned long long f_renders   = 0;
std::atomic<unsigned long long> c_frames_render_no_redirect{ 0 };
unsigned long long f_brackets  = 0;

// ─────────────────────────────────────────────────────────────────────────────
bool can_flush(JNIEnv* env, jobject provider)
{
    if (!provider || !g_immediate_class || !g_draw) return false;
    return env->IsInstanceOf(provider, g_immediate_class) == JNI_TRUE;
}

// Push whatever Minecraft has buffered so far straight at the driver.
//
// This is the whole trick. The immediate-mode provider accumulates geometry and
// only submits it when the render layer changes, which is precisely why a held
// item's draw does not arrive while its owner is being rendered. Calling draw()
// here forces the boundary to fall where it is needed.
void flush(JNIEnv* env, jobject provider)
{
    env->CallVoidMethod(provider, g_draw);
    if (env->ExceptionCheck()) env->ExceptionClear();
    c_flushes.fetch_add(1, std::memory_order_relaxed);
}

// The bracket. BOTH flushes are required: without the first one, the second
// would also push whatever earlier entities had left buffered, and the flag
// would be claiming geometry it does not own.
// Who owns the equipment about to be submitted, from the render state handed
// to the feature renderer. Returns -1 when it is not a player's.
int owner_of(JNIEnv* env, jobject state)
{
    if (!state || !g_pstate_class || !g_pstate_id) return -1;
    if (env->IsInstanceOf(state, g_pstate_class) != JNI_TRUE) return -1;
    const jint id = env->GetIntField(state, g_pstate_id);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return -1; }
    return static_cast<int>(id);
}

template <class CallOrig>
inline void bracket(JNIEnv* env, jobject provider, int owner, CallOrig&& call_original)
{
    if (!g_active.load(std::memory_order_relaxed)) { call_original(); return; }

    // Not a player, or the player is US. Either way this equipment is not ours
    // to replay, and the local player is excluded HERE rather than left to a
    // texture test that shared assets defeat.
    if (owner < 0) { call_original(); return; }
    if (owner == ::vanta::modules::chams::jni::self_entity_id()) {
        c_self_skipped.fetch_add(1, std::memory_order_relaxed);
        call_original();
        return;
    }

    // NO FLUSH, NO FLAG. On 1.21.9+ the render target is an
    // OrderedRenderCommandQueue: geometry is SUBMITTED to a deferred queue
    // (RenderCommandQueue.submitItem and friends) and drawn later in bulk, so
    // there is no boundary to force and no draw() to force it with. An unscoped
    // flag is exactly what put the enchantment glint over the whole screen once
    // already, so on those versions this does nothing at all.
    if (!can_flush(env, provider)) {
        // 1.21.9+ : `provider` is an OrderedRenderCommandQueue. There is nothing
        // to flush here because nothing is drawn here -- the feature renderer
        // only SUBMITS. What this window is good for is telling submitItem that
        // the submissions about to happen belong to this entity, so they can be
        // steered into a queue of our own. The equipment flag is deliberately
        // NOT raised here: it would cover no draws and scope nothing, which is
        // the mistake that once put the enchantment glint over the screen.
        if (g_get_batching && ORIG_submitItem) {
            jobject   prev_q = g_ordered;
            const int prev_o = g_owner_id;
            g_ordered  = provider;
            g_owner_id = owner;
            c_renders.fetch_add(1, std::memory_order_relaxed);
            ++f_renders;
            call_original();
            g_ordered  = prev_q;
            g_owner_id = prev_o;
            return;
        }

        if (!c_no_flush.fetch_add(1, std::memory_order_relaxed))
            CHAMS_LOG("item: render target is not an Immediate and the 1.21.9+ command "
                      "queue hooks did not resolve -- equipment is NOT covered. Players "
                      "and armour geometry are unaffected.");
        call_original();
        return;
    }

    c_renders.fetch_add(1, std::memory_order_relaxed);
    ++f_renders;

    flush(env, provider);                  // everything already pending leaves FIRST
    gl::set_rendering_equipment(true);
    call_original();
    flush(env, provider);                  // now ONLY this piece is in flight
    gl::set_rendering_equipment(false);
}

// ── Detours. One pair per target, because each must call its own original ────
void JNICALL hkHeld2(JNIEnv* env, jobject thiz, jobject m, jobject p,
                     jint light, jobject st, jfloat a, jfloat b)
{
    vanta::NativeCallback callbackLifetime;
    bracket(env, p, owner_of(env, st), [&] {
        if (ORIG_held && g_held_class) {
            env->CallNonvirtualVoidMethod(thiz, g_held_class, ORIG_held, m, p, light, st, a, b);
            if (env->ExceptionCheck()) env->ExceptionClear();
        }
    });
}
void JNICALL hkHeld6(JNIEnv* env, jobject thiz, jobject m, jobject p,
                     jint light, jobject e,
                     jfloat a, jfloat b, jfloat c, jfloat d, jfloat f, jfloat g)
{
    vanta::NativeCallback callbackLifetime;
    // 1.21..1.21.1 hands the LivingEntity itself, not a render state, and that
    // branch never reaches the queue path -- it keeps the flush bracket, where
    // ownership is still settled at draw time.
    bracket(env, p, 0, [&] {
        if (ORIG_held && g_held_class) {
            env->CallNonvirtualVoidMethod(thiz, g_held_class, ORIG_held,
                                          m, p, light, e, a, b, c, d, f, g);
            if (env->ExceptionCheck()) env->ExceptionClear();
        }
    });
}
void JNICALL hkArmour2(JNIEnv* env, jobject thiz, jobject m, jobject p,
                       jint light, jobject st, jfloat a, jfloat b)
{
    vanta::NativeCallback callbackLifetime;
    bracket(env, p, owner_of(env, st), [&] {
        if (ORIG_armour && g_armour_class) {
            env->CallNonvirtualVoidMethod(thiz, g_armour_class, ORIG_armour, m, p, light, st, a, b);
            if (env->ExceptionCheck()) env->ExceptionClear();
        }
    });
}
void JNICALL hkArmour6(JNIEnv* env, jobject thiz, jobject m, jobject p,
                       jint light, jobject e,
                       jfloat a, jfloat b, jfloat c, jfloat d, jfloat f, jfloat g)
{
    vanta::NativeCallback callbackLifetime;
    bracket(env, p, 0, [&] {
        if (ORIG_armour && g_armour_class) {
            env->CallNonvirtualVoidMethod(thiz, g_armour_class, ORIG_armour,
                                          m, p, light, e, a, b, c, d, f, g);
            if (env->ExceptionCheck()) env->ExceptionClear();
        }
    });
}

// The queue a player's equipment submission should go to, or null to leave it
// where it was. Returns a LOCAL ref the caller must release.
jobject redirect_target(JNIEnv* env, jobject thiz)
{
    // g_owner_id is set only inside a bracket that already established this
    // submission belongs to a player who is not us. gl::rendering_player() is
    // deliberately NOT consulted here: on this path it is frame-global.
    if (!g_active.load(std::memory_order_relaxed) || !g_ordered
        || g_owner_id < 0 || !g_get_batching || !g_bq_class)
        return nullptr;

    jobject q = env->CallObjectMethod(g_ordered, g_get_batching, kVantaOrder);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }
    if (!q) return nullptr;

    // Redirect only when the queue is genuinely a different one. Submitting
    // back into `thiz` through the original would be a no-op dressed as a
    // redirect, and would quietly do nothing forever.
    if (env->IsSameObject(q, thiz) != JNI_FALSE) { env->DeleteLocalRef(q); return nullptr; }

    if (!g_our_queue || env->IsSameObject(g_our_queue, q) == JNI_FALSE) {
        if (g_our_queue) env->DeleteGlobalRef(g_our_queue);
        g_our_queue = env->NewGlobalRef(q);
    }
    c_redirects.fetch_add(1, std::memory_order_relaxed);
    ++f_redirects;
    return q;
}

#define CHAMS_REDIRECT(ORIG, ...)                                                  do {                                                                               jobject _q = redirect_target(env, thiz);                                       if ((ORIG) && g_bq_class) {                                                        env->CallNonvirtualVoidMethod(_q ? _q : thiz, g_bq_class, (ORIG),                                            __VA_ARGS__);                                    if (env->ExceptionCheck()) env->ExceptionClear();                          }                                                                              if (_q) env->DeleteLocalRef(_q);                                           } while (0)

// submitModel(Model, S state, MatrixStack, RenderLayer, III, Sprite, I, Crumbling)
void JNICALL hkSubmitModel(JNIEnv* env, jobject thiz, jobject model, jobject state,
                           jobject m, jobject layer, jint l1, jint l2, jint l3,
                           jobject sprite, jint i4, jobject crumbling)
{
    vanta::NativeCallback callbackLifetime; CHAMS_REDIRECT(ORIG_submitModel, model, state, m, layer, l1, l2, l3, sprite, i4, crumbling); }

void JNICALL hkSubmitItem(JNIEnv* env, jobject thiz, jobject m, jobject ctx,
                          jint a, jint b, jint c, jintArray tints, jobject quads,
                          jobject layer, jobject glint)
{
    vanta::NativeCallback callbackLifetime; CHAMS_REDIRECT(ORIG_submitItem, m, ctx, a, b, c, tints, quads, layer, glint); }

// A BLOCK held in the hand comes through here, not submitItem.
void JNICALL hkSubmitBSM(JNIEnv* env, jobject thiz, jobject m, jobject layer, jobject model,
                         jfloat r, jfloat g, jfloat b, jint i1, jint i2, jint i3)
{
    vanta::NativeCallback callbackLifetime; CHAMS_REDIRECT(ORIG_submitBSM, m, layer, model, r, g, b, i1, i2, i3); }

// Armour pieces and shields. Four overloads, all of them live.
void JNICALL hkSubmitMP1(JNIEnv* env, jobject thiz, jobject part, jobject m, jobject layer,
                         jint l1, jint l2, jobject sprite)
{
    vanta::NativeCallback callbackLifetime; CHAMS_REDIRECT(ORIG_submitMP1, part, m, layer, l1, l2, sprite); }

void JNICALL hkSubmitMP2(JNIEnv* env, jobject thiz, jobject part, jobject m, jobject layer,
                         jint l1, jint l2, jobject sprite, jint i3, jobject crumbling)
{
    vanta::NativeCallback callbackLifetime; CHAMS_REDIRECT(ORIG_submitMP2, part, m, layer, l1, l2, sprite, i3, crumbling); }

void JNICALL hkSubmitMP3(JNIEnv* env, jobject thiz, jobject part, jobject m, jobject layer,
                         jint l1, jint l2, jobject sprite, jboolean z1, jboolean z2)
{
    vanta::NativeCallback callbackLifetime; CHAMS_REDIRECT(ORIG_submitMP3, part, m, layer, l1, l2, sprite, z1, z2); }

void JNICALL hkSubmitMP4(JNIEnv* env, jobject thiz, jobject part, jobject m, jobject layer,
                         jint l1, jint l2, jobject sprite, jboolean z1, jboolean z2,
                         jint i3, jobject crumbling, jint i4)
{
    vanta::NativeCallback callbackLifetime; CHAMS_REDIRECT(ORIG_submitMP4, part, m, layer, l1, l2, sprite, z1, z2, i3, crumbling, i4); }

// 1.21.9+ : the queue is emitted here, and the Immediate to emit into arrives
// as a parameter -- so this is where the two flushes finally have something to
// bracket.
// Is this the batch we steered a player's equipment into?
bool is_our_batch(JNIEnv* env, jobject batch, jobject immediate)
{
    return g_active.load(std::memory_order_relaxed)
        && g_our_queue && batch
        && env->IsSameObject(batch, g_our_queue) == JNI_TRUE
        && can_flush(env, immediate);
}

// Each command TYPE has its own renderer, and a player's equipment lands in
// several of them: an item in ItemCommandRenderer, a held block and armour in
// the model and model-part renderers. All three are bracketed, because the
// redirect is only half the mechanism -- steering a submission into our queue
// and then not bracketing the renderer that emits it would leave the geometry
// drawn in our ordering slot with no chams on it, which is a change for
// nothing.
void JNICALL hkIcrRender(JNIEnv* env, jobject thiz, jobject batch,
                         jobject immediate, jobject ctx)
{
    vanta::NativeCallback callbackLifetime;
    const bool ours = is_our_batch(env, batch, immediate);
    if (ours) {
        flush(env, immediate);                 // clear anything pending FIRST
        gl::set_rendering_owned_equipment(true);
        c_queue_brackets.fetch_add(1, std::memory_order_relaxed);
        ++f_brackets;
    }
    if (ORIG_icrRender && g_icr_class) {
        env->CallNonvirtualVoidMethod(thiz, g_icr_class, ORIG_icrRender, batch, immediate, ctx);
        if (env->ExceptionCheck()) env->ExceptionClear();
    }
    if (ours) {
        flush(env, immediate);                 // only OUR batch is in flight
        gl::set_rendering_owned_equipment(false);
    }
}

void JNICALL hkMcrRender(JNIEnv* env, jobject thiz, jobject batch, jobject immediate,
                         jobject ctx, jobject immediate2)
{
    vanta::NativeCallback callbackLifetime;
    const bool ours = is_our_batch(env, batch, immediate);
    if (ours) {
        flush(env, immediate);
        gl::set_rendering_owned_equipment(true);
        c_queue_brackets.fetch_add(1, std::memory_order_relaxed);
    }
    if (ORIG_mcrRender && g_mcr_class) {
        env->CallNonvirtualVoidMethod(thiz, g_mcr_class, ORIG_mcrRender,
                                      batch, immediate, ctx, immediate2);
        if (env->ExceptionCheck()) env->ExceptionClear();
    }
    if (ours) { flush(env, immediate); gl::set_rendering_owned_equipment(false); }
}

void JNICALL hkMprRender(JNIEnv* env, jobject thiz, jobject batch, jobject immediate,
                         jobject ctx, jobject immediate2)
{
    vanta::NativeCallback callbackLifetime;
    const bool ours = is_our_batch(env, batch, immediate);
    if (ours) {
        flush(env, immediate);
        gl::set_rendering_owned_equipment(true);
        c_queue_brackets.fetch_add(1, std::memory_order_relaxed);
    }
    if (ORIG_mprRender && g_mpr_class) {
        env->CallNonvirtualVoidMethod(thiz, g_mpr_class, ORIG_mprRender,
                                      batch, immediate, ctx, immediate2);
        if (env->ExceptionCheck()) env->ExceptionClear();
    }
    if (ours) { flush(env, immediate); gl::set_rendering_owned_equipment(false); }
}

// Attach one method by name+signature on an already-resolved class. The 1.21.9+
// queue path needs seven of these and several are overloads on one class, so
// they share this rather than seven copies of the same twenty lines.
bool attach_on(JNIEnv* env, jclass k, const char* owner, const char* name,
               const char* sig, void* detour, jmethodID* orig, const char* label)
{
    jmethodID mid = env->GetMethodID(k, name, sig);
    if (env->ExceptionCheck()) env->ExceptionClear();
    ::vanta::desklog::jni_lookup(dbg::ch(), "method", owner, name, label, mid != nullptr);
    if (!mid) return false;

    jnihook_result_t r = JNIHOOK_ERR_JVMTI_OPERATION;
    __try { r = JNIHook_Attach(mid, detour, orig); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        CHAMS_LOG("item: JNIHook_Attach(%s) faulted (0x%08lX)", label,
                  (unsigned long)GetExceptionCode());
        r = JNIHOOK_ERR_JVMTI_OPERATION;
    }
    if (r != JNIHOOK_OK) {
        CHAMS_LOG("item: JNIHook_Attach(%s) returned %d", label, (int)r);
        *orig = nullptr;
        return false;
    }
    return true;
}

// Resolve, attach, and report one feature renderer. Returns true if it took.
bool attach_one(JNIEnv* env, const char* cls, const char* method,
                const variant* vars, int nvars,
                void* detour2, void* detour6,
                jclass* out_class, jmethodID* out_orig, const char* label)
{
    jclass k = ::vanta::jvm::findClassAnyLoader(env, cls);
    ::vanta::desklog::jni_lookup(dbg::ch(), "class", cls, "<self>", "", k != nullptr);
    if (!k) { if (env->ExceptionCheck()) env->ExceptionClear(); return false; }

    jmethodID mid = nullptr;
    const variant* used = nullptr;
    for (int i = 0; i < nvars; ++i) {
        mid = env->GetMethodID(k, method, vars[i].sig);
        if (env->ExceptionCheck()) env->ExceptionClear();
        if (mid) { used = &vars[i]; break; }
    }
    ::vanta::desklog::jni_lookup(dbg::ch(), "method", cls, method,
                                 used ? used->range : "<no variant matched>", mid != nullptr);
    if (!mid) {
        CHAMS_LOG("item: %s.%s matched none of the %d known signatures -- %s is not "
                  "covered on this version.", cls, method, nvars, label);
        env->DeleteLocalRef(k);
        return false;
    }

    *out_class = static_cast<jclass>(env->NewGlobalRef(k));
    env->DeleteLocalRef(k);

    void* detour = (used->floats == 6) ? detour6 : detour2;

    // SEH around the attach, as chams_cull does: Lunar and Feather put
    // aggressive mixins on the render classes and JNIHook can fault rewriting
    // one, which would take javaw down with no Java exception to catch.
    jnihook_result_t ar = JNIHOOK_ERR_JVMTI_OPERATION;
    __try {
        ar = JNIHook_Attach(mid, detour, out_orig);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        CHAMS_LOG("item: JNIHook_Attach(%s) faulted (code=0x%08lX) -- %s left alone.",
                  label, (unsigned long)GetExceptionCode(), label);
        ar = JNIHOOK_ERR_JVMTI_OPERATION;
    }

    if (ar != JNIHOOK_OK) {
        CHAMS_LOG("item: JNIHook_Attach(%s) returned %d -- not installed", label, (int)ar);
        *out_orig = nullptr;
        env->DeleteGlobalRef(*out_class);
        *out_class = nullptr;
        return false;
    }

    CHAMS_LOG("item: hooked %s.%s [%s] for %s", cls, method, used->range, label);
    return true;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
bool install(JNIEnv* env)
{
    if (g_installed.load(std::memory_order_acquire)) return true;
    if (!env) return false;

    JavaVM* vm = nullptr;
    if (env->GetJavaVM(&vm) != JNI_OK || !vm) return false;

    const jnihook_result_t ir = JNIHook_Init(vm);
    if (ir != JNIHOOK_OK && ir != JNIHOOK_ERR_ADD_JVMTI_CAPS) {
        CHAMS_RL(dbg::kSlotCapSkip, 5000,
                 "item: JNIHook_Init failed (%d) -- equipment stays as the game has it",
                 (int)ir);
        return false;
    }

    // The flush target. Optional on purpose: without it the brackets still run
    // and simply decline to move the boundary, which is inert, not harmful.
    if (jclass imm = ::vanta::jvm::findClassAnyLoader(env, kImmediate)) {
        g_draw = env->GetMethodID(imm, kDraw, "()V");
        if (env->ExceptionCheck()) env->ExceptionClear();
        if (g_draw) g_immediate_class = static_cast<jclass>(env->NewGlobalRef(imm));
        ::vanta::desklog::jni_lookup(dbg::ch(), "method", kImmediate, kDraw, "()V",
                                     g_draw != nullptr);
        env->DeleteLocalRef(imm);
    } else {
        ::vanta::desklog::jni_lookup(dbg::ch(), "class", kImmediate, "<self>", "", false);
    }

    ::vanta::desklog::section(dbg::ch(), "player equipment hooks");

    // PlayerEntityRenderState.id -- the per-entity ownership signal.
    //
    // class_10055 extends class_10034 extends class_10426, so the object handed
    // to BOTH hooked feature renderers for a player IS one of these, and
    // field_53528 is assigned Entity.getId() when the state is extracted. That
    // makes it an exact identity AND an exact self test, taken adjacent to the
    // submission instead of from a frame-global flag written in a different
    // pass entirely.
    //
    // Optional: if either lookup fails, owner_of() returns -1 for every entity,
    // no equipment is redirected, and the module falls back to texture identity
    // alone -- the behaviour it had before the queue path existed.
    if (jclass ps = ::vanta::jvm::findClassAnyLoader(env, "net/minecraft/class_10055")) {
        g_pstate_id = env->GetFieldID(ps, "field_53528", "I");
        if (env->ExceptionCheck()) env->ExceptionClear();
        if (g_pstate_id) g_pstate_class = static_cast<jclass>(env->NewGlobalRef(ps));
        ::vanta::desklog::jni_lookup(dbg::ch(), "field", "net/minecraft/class_10055",
                                     "field_53528", "I  PlayerEntityRenderState.id",
                                     g_pstate_id != nullptr);
        env->DeleteLocalRef(ps);
    } else {
        ::vanta::desklog::jni_lookup(dbg::ch(), "class", "net/minecraft/class_10055",
                                     "<self>", "", false);
    }


    // ── 1.21.9+ command-queue path ───────────────────────────────────────────
    //
    // Absent on 1.21.0 .. 1.21.8, where none of these classes exist. Resolving
    // nothing there is the expected outcome, not a failure, and the flush path
    // above carries those versions on its own.
    if (jclass ocq = ::vanta::jvm::findClassAnyLoader(env, kOrderedQueue)) {
        g_get_batching = env->GetMethodID(ocq, kGetBatching, kGetBatchingSig);
        if (env->ExceptionCheck()) env->ExceptionClear();
        if (g_get_batching) g_ocq_class = static_cast<jclass>(env->NewGlobalRef(ocq));
        ::vanta::desklog::jni_lookup(dbg::ch(), "method", kOrderedQueue, kGetBatching,
                                     kGetBatchingSig, g_get_batching != nullptr);
        env->DeleteLocalRef(ocq);
    }

    if (g_get_batching) {
        // BatchingRenderCommandQueue, NOT the RenderCommandQueue interface. The
        // interface declares these abstract; detouring it would intercept
        // nothing. Established by reading the 1.21.11 class files.
        if (jclass bq = ::vanta::jvm::findClassAnyLoader(env, kBatchQueue)) {
            g_bq_class = static_cast<jclass>(env->NewGlobalRef(bq));

            attach_on(env, bq, kBatchQueue, "method_73480",
                "(Lnet/minecraft/class_4587;Lnet/minecraft/class_811;III[ILjava/util/List;"
                "Lnet/minecraft/class_1921;Lnet/minecraft/class_10444$class_10445;)V",
                reinterpret_cast<void*>(&hkSubmitItem), &ORIG_submitItem, "submitItem");

            // submitModel IS armour's real door -- that finding is solid, and
            // hkArmour2 is a no-op without it. But attaching it crashed the game
            // within a second of arming, hard enough that the vectored handler
            // never ran: no CRASH section, the log simply stops after the first
            // frame. That signature is a fatal JVM error or a stack overflow,
            // not an access violation in our own code.
            //
            // The reason it is dangerous where the other five are not: every one
            // of those is equipment-only and fires a handful of times per player.
            // submitModel is how LivingEntityRenderer submits every BODY in the
            // world, so the detour runs thousands of times a frame, on the
            // render thread, through a JNIHook-rewritten method. Whatever is
            // wrong with it is wrong at that volume immediately.
            //
            // Left attached-but-disabled rather than deleted: the analysis that
            // produced it is correct and the hook is the only route to armour
            // trims and the armour glint on 1.21.9+. It needs its own
            // investigation -- most likely the generic <S> erasure, JNIHook's
            // handling of a fifth retransformation of this class, or local-ref
            // pressure at that call rate -- not another blind attempt.
            //
            // With it off, armour_queue_armed() is false, so chams.cpp keeps
            // folding the shared armour ids into the player set and armour
            // renders exactly as it did before this build.
            if (false) {
                attach_on(env, bq, kBatchQueue, "method_73490",
                    "(Lnet/minecraft/class_3879;Ljava/lang/Object;Lnet/minecraft/class_4587;"
                    "Lnet/minecraft/class_1921;IIILnet/minecraft/class_1058;I"
                    "Lnet/minecraft/class_11683$class_11792;)V",
                    reinterpret_cast<void*>(&hkSubmitModel), &ORIG_submitModel, "submitModel");
            }
            CHAMS_LOG("item: submitModel hook is DISABLED -- it crashed the game on "
                      "1.21.11. Armour keeps the shared-texture path; trims and the "
                      "armour glint stay uncovered on this version.");

            attach_on(env, bq, kBatchQueue, "method_73484",
                "(Lnet/minecraft/class_4587;Lnet/minecraft/class_1921;"
                "Lnet/minecraft/class_1087;FFFIII)V",
                reinterpret_cast<void*>(&hkSubmitBSM), &ORIG_submitBSM,
                "submitBlockStateModel");

            // method_73491/73492/73493 are deliberately NOT attached, and their
            // absence is correct rather than a gap. All three are interface
            // DEFAULT methods on RenderCommandQueue whose entire bodies widen
            // into the 11-arg overload below -- the only one declared on
            // BatchingRenderCommandQueue. Every submitModelPart dispatch in the
            // game therefore terminates in the hook that does take. Attaching
            // them only drove JNIHook through RetransformClasses three times at
            // startup and printed an error line that read as a defect, which
            // sent two investigations down the wrong path.
            attach_on(env, bq, kBatchQueue, "method_73494",
                "(Lnet/minecraft/class_630;Lnet/minecraft/class_4587;"
                "Lnet/minecraft/class_1921;IILnet/minecraft/class_1058;ZZI"
                "Lnet/minecraft/class_11683$class_11792;I)V",
                reinterpret_cast<void*>(&hkSubmitMP4), &ORIG_submitMP4, "submitModelPart#4");

            env->DeleteLocalRef(bq);
        }

        // The emit side. Each command TYPE has its own renderer and a player's
        // equipment lands in several of them, so all three are bracketed. A
        // redirect whose renderer is not bracketed would move the geometry into
        // our ordering slot and give it no chams -- a change for nothing.
        constexpr const char* kBatchRenderSig =
            "(Lnet/minecraft/class_11788;Lnet/minecraft/class_4597$class_4598;"
            "Lnet/minecraft/class_4618;Lnet/minecraft/class_4597$class_4598;)V";

        if (jclass icr = ::vanta::jvm::findClassAnyLoader(env, kItemCmdRenderer)) {
            g_icr_class = static_cast<jclass>(env->NewGlobalRef(icr));
            attach_on(env, icr, kItemCmdRenderer, kIcrRender, kIcrRenderSig,
                      reinterpret_cast<void*>(&hkIcrRender), &ORIG_icrRender,
                      "ItemCommandRenderer.render");
            env->DeleteLocalRef(icr);
        }
        if (jclass mcr = ::vanta::jvm::findClassAnyLoader(env, "net/minecraft/class_11683")) {
            g_mcr_class = static_cast<jclass>(env->NewGlobalRef(mcr));
            attach_on(env, mcr, "net/minecraft/class_11683", "method_73001", kBatchRenderSig,
                      reinterpret_cast<void*>(&hkMcrRender), &ORIG_mcrRender,
                      "ModelCommandRenderer.render");
            env->DeleteLocalRef(mcr);
        }
        if (jclass mpr = ::vanta::jvm::findClassAnyLoader(env, "net/minecraft/class_11793")) {
            g_mpr_class = static_cast<jclass>(env->NewGlobalRef(mpr));
            attach_on(env, mpr, "net/minecraft/class_11793", "method_73543", kBatchRenderSig,
                      reinterpret_cast<void*>(&hkMprRender), &ORIG_mprRender,
                      "ModelPartCommandRenderer.render");
            env->DeleteLocalRef(mpr);
        }

        // Both or neither. A redirect with no bracket on the other end would
        // move player items into a queue nothing then treats specially -- they
        // would still render, but in our ordering slot and with no chams, which
        // is a change for nothing.
        const bool any_submit = ORIG_submitItem || ORIG_submitBSM || ORIG_submitMP1
                             || ORIG_submitMP2 || ORIG_submitMP3 || ORIG_submitMP4;
        const bool any_render = ORIG_icrRender || ORIG_mcrRender || ORIG_mprRender;

        if (!any_submit || !any_render) {
            CHAMS_LOG("item: 1.21.9+ queue path INCOMPLETE (anySubmit=%d anyRender=%d) "
                      "-- disabled; the game's own ordering is left untouched.",
                      (int)any_submit, (int)any_render);
            g_pstate_id = nullptr;
            g_owner_id  = -1;
            ORIG_submitItem = ORIG_submitBSM = ORIG_submitModel = nullptr;
            ORIG_submitMP1 = ORIG_submitMP2 = ORIG_submitMP3 = ORIG_submitMP4 = nullptr;
            ORIG_icrRender = ORIG_mcrRender = ORIG_mprRender = nullptr;
        } else {
            // The player model arrives part by part on this branch, so the
            // minimum-geometry filter has nothing left to separate. Measured:
            // real player parts at 24 indices, a placed head at 72.
            gl::set_size_filter(false);
            CHAMS_LOG("item: minimum-geometry filter DISABLED -- on this version the "
                      "player model is submitted part by part, so player geometry and "
                      "a placed head overlap in size and no threshold separates them.");
            CHAMS_LOG("item: 1.21.9+ queue path armed on key 0x%08X. submit: item=%d "
                      "blockStateModel=%d modelPart=%d/%d/%d/%d | emit: item=%d model=%d "
                      "modelPart=%d",
                      (unsigned)kVantaOrder,
                      ORIG_submitItem != nullptr, ORIG_submitBSM != nullptr,
                      ORIG_submitMP1 != nullptr, ORIG_submitMP2 != nullptr,
                      ORIG_submitMP3 != nullptr, ORIG_submitMP4 != nullptr,
                      ORIG_icrRender != nullptr, ORIG_mcrRender != nullptr,
                      ORIG_mprRender != nullptr);
        }
    }

    const bool held = attach_one(env, "net/minecraft/class_989", "method_17162",
                                 kHeldVariants, (int)(sizeof kHeldVariants / sizeof kHeldVariants[0]),
                                 reinterpret_cast<void*>(&hkHeld2),
                                 reinterpret_cast<void*>(&hkHeld6),
                                 &g_held_class, &ORIG_held, "held items");

    const bool armour = attach_one(env, "net/minecraft/class_970", "method_17157",
                                   kArmourVariants, (int)(sizeof kArmourVariants / sizeof kArmourVariants[0]),
                                   reinterpret_cast<void*>(&hkArmour2),
                                   reinterpret_cast<void*>(&hkArmour6),
                                   &g_armour_class, &ORIG_armour, "armour (enchantment glint)");

    if (!held && !armour) {
        CHAMS_LOG("item: neither feature renderer could be hooked -- equipment is not "
                  "covered. Players and armour geometry are unaffected.");
        return false;
    }

    g_installed.store(true, std::memory_order_release);
    CHAMS_LOG("item: equipment bracketing armed. heldItems=%d armour=%d flushAvailable=%d "
              "queuePath=%d",
              (int)held, (int)armour, g_draw != nullptr,
              (ORIG_submitItem && ORIG_icrRender) ? 1 : 0);
    return true;
}

void on_frame()
{
    if (f_redirects && !f_brackets)
        c_frames_redirect_no_bracket.fetch_add(1, std::memory_order_relaxed);

    // The failure that matters after the ownership change: a player's feature
    // renderer ran and produced NO redirect. The older counter is blind to it,
    // because a zero-redirect frame reconciles clean -- which is exactly how
    // lostFrames read 0 for 47 seconds while the item was vanishing.
    if (f_renders && !f_redirects)
        c_frames_render_no_redirect.fetch_add(1, std::memory_order_relaxed);

    f_redirects = 0;
    f_brackets  = 0;
    f_renders   = 0;
}

bool armour_queue_armed()
{
    return g_installed.load(std::memory_order_acquire)
        && ORIG_submitModel != nullptr
        && (ORIG_mcrRender != nullptr || ORIG_mprRender != nullptr);
}

bool installed()         { return g_installed.load(std::memory_order_acquire); }
void set_active(bool on) { g_active.store(on, std::memory_order_release); }

void log_summary()
{
    if (!g_installed.load(std::memory_order_acquire)) return;
    CHAMS_RL(dbg::kSlotLiveModel, 2000,
             "item: equipmentRenders=%llu bufferFlushes=%llu noFlushTarget=%llu "
             "queueRedirects=%llu queueBrackets=%llu lostFrames=%llu "
             "deadFrames=%llu selfSkipped=%llu",
             c_renders.load(), c_flushes.load(), c_no_flush.load(),
             c_redirects.load(), c_queue_brackets.load(),
             c_frames_redirect_no_bracket.load(),
             c_frames_render_no_redirect.load(), c_self_skipped.load());
}

void shutdown()
{
    if (!g_installed.exchange(false, std::memory_order_acq_rel)) return;
    g_active.store(false, std::memory_order_release);
    ::vanta::jvm::JniScope s;
    if (s) {
        JNIEnv* env = s.get();
        if (g_held_class)      { env->DeleteGlobalRef(g_held_class);      g_held_class = nullptr; }
        if (g_armour_class)    { env->DeleteGlobalRef(g_armour_class);    g_armour_class = nullptr; }
        if (g_immediate_class) { env->DeleteGlobalRef(g_immediate_class); g_immediate_class = nullptr; }
        if (g_bq_class)        { env->DeleteGlobalRef(g_bq_class);        g_bq_class = nullptr; }
        if (g_icr_class)       { env->DeleteGlobalRef(g_icr_class);       g_icr_class = nullptr; }
        if (g_mcr_class)       { env->DeleteGlobalRef(g_mcr_class);       g_mcr_class = nullptr; }
        if (g_mpr_class)       { env->DeleteGlobalRef(g_mpr_class);       g_mpr_class = nullptr; }
        if (g_pstate_class)    { env->DeleteGlobalRef(g_pstate_class);    g_pstate_class = nullptr; }
        if (g_ocq_class)       { env->DeleteGlobalRef(g_ocq_class);       g_ocq_class = nullptr; }
        if (g_our_queue)       { env->DeleteGlobalRef(g_our_queue);       g_our_queue = nullptr; }
    }
    ORIG_held = ORIG_armour = nullptr;
    ORIG_submitMP1 = ORIG_submitMP2 = ORIG_submitMP3 = ORIG_submitMP4 = nullptr;
    ORIG_icrRender = ORIG_mcrRender = ORIG_mprRender = nullptr;
    g_get_batching = nullptr;
    g_ordered = nullptr;
    g_draw = nullptr;
    CHAMS_LOG("item: shutdown");
}

} // namespace vanta::modules::chams::item
