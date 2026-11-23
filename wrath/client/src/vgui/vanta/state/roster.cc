#include "../../includes.hh"

#include "jvm/JniHelper.h"
#include "jvm/JvmContext.h"
#include "util/Log.h"
#include "core/profile/PlayerAppearance.h"

#include <jni.h>
#include <algorithm>

namespace vanta
{
	namespace
	{
		struct cache_t
		{
			jclass    mc_class = nullptr;      // global ref
			jmethodID mc_instance = nullptr;
			jmethodID mc_network = nullptr;
			jmethodID net_playerList = nullptr;
			jmethodID ple_profile = nullptr;
			jmethodID ple_latency = nullptr;
			jmethodID profile_name = nullptr;
			jmethodID profile_id = nullptr, uuid_string = nullptr, ple_display = nullptr, text_string = nullptr;
			jmethodID coll_iterator = nullptr;
			jmethodID iter_hasNext = nullptr;
			jmethodID iter_next = nullptr;
			jfieldID  mc_player = nullptr;        // field_1724, optional
			jmethodID player_profile = nullptr;   // class_1657 method_7334, optional; self is unmarked without it

			/* the loaded-entity walk; all optional, a miss just leaves m_loaded false */
			jfieldID  mc_world         = nullptr;   // field_1687
			jmethodID world_getPlayers = nullptr;   // method_18456 ()Ljava/util/List;

			/* class_1309 LivingEntity — the same pair the ESP's health bar reads, and the
			   same intermediary names on every version from 1.21 to 1.21.11. */
			jmethodID liv_getHealth    = nullptr;   // method_6032 ()F
			jmethodID liv_getMaxHealth = nullptr;   // method_6063 ()F
		};

		cache_t C{};

		/* Every miss is named. A tab list that comes up empty and a tab list whose lookup failed
		   look identical on screen, and only this tells them apart. */
		bool want(jmethodID id, const char* what)
		{
			if (!id) VLOG_WARN("roster", "tab list: {} did not resolve", what);
			return id != nullptr;
		}

		std::string read_string(JNIEnv* env, jstring js)
		{
			if (!js) return {};
			const char* c = env->GetStringUTFChars(js, nullptr);
			std::string s = c ? c : "";
			if (c) env->ReleaseStringUTFChars(js, c);
			env->DeleteLocalRef(js);
			return s;
		}

        jmethodID optional(JNIEnv* e, jclass cls, const char* name, const char* signature) {
            if (!cls) return nullptr;
            auto method = e->GetMethodID(cls, name, signature);
            if (e->ExceptionCheck()) e->ExceptionClear();
            return method;
        }
        std::string texture_property(JNIEnv* e, jobject profile) {
            if (e->PushLocalFrame(24) < 0) return {};
            std::string result;
            auto cls = e->GetObjectClass(profile);
            auto properties = optional(e, cls, "getProperties", "()Lcom/mojang/authlib/properties/PropertyMap;");
            if (!properties) properties = optional(e, cls, "properties", "()Lcom/mojang/authlib/properties/PropertyMap;");
            auto map = properties ? e->CallObjectMethod(profile, properties) : nullptr;
            if (map && !e->ExceptionCheck()) {
                auto get = optional(e, e->GetObjectClass(map), "get", "(Ljava/lang/Object;)Ljava/util/Collection;");
                auto key = e->NewStringUTF("textures");
                auto values = get ? e->CallObjectMethod(map, get, key) : nullptr;
                auto iterator = values && !e->ExceptionCheck() ? optional(e, e->GetObjectClass(values), "iterator", "()Ljava/util/Iterator;") : nullptr;
                auto it = iterator ? e->CallObjectMethod(values, iterator) : nullptr;
                if (it && !e->ExceptionCheck()) {
                    auto more = optional(e, e->GetObjectClass(it), "hasNext", "()Z");
                    auto next = optional(e, e->GetObjectClass(it), "next", "()Ljava/lang/Object;");
                    if (more && next && e->CallBooleanMethod(it, more)) {
                        auto property = e->CallObjectMethod(it, next);
                        if (property && !e->ExceptionCheck()) {
                            auto value = optional(e, e->GetObjectClass(property), "value", "()Ljava/lang/String;");
                            if (!value) value = optional(e, e->GetObjectClass(property), "getValue", "()Ljava/lang/String;");
                            if (value) result = read_string(e, static_cast<jstring>(e->CallObjectMethod(property, value)));
                        }
                    }
                }
            }
            if (e->ExceptionCheck()) { e->ExceptionClear(); result.clear(); }
            e->PopLocalFrame(nullptr);
            if (result.size() > 32768) result.clear();
            return result;
        }
	}

	bool c_roster::resolve(JNIEnv* env)
	{
		if (this->m_resolved) return true;
		if (!env) return false;

		jclass mc  = ::vanta::jvm::findClassAnyLoader(env, "net/minecraft/class_310");
		jclass net = ::vanta::jvm::findClassAnyLoader(env, "net/minecraft/class_634");
		jclass ple = ::vanta::jvm::findClassAnyLoader(env, "net/minecraft/class_640");
		if (env->ExceptionCheck()) env->ExceptionClear();

		/* Before a world exists these simply are not loaded yet, which is not a failure — the
		   caller keeps asking. Only a resolution that got the classes and then missed a member
		   is worth complaining about. */
		if (!mc || !net || !ple) return false;

		this->m_attempted = true;

		C.mc_instance    = env->GetStaticMethodID(mc, "method_1551", "()Lnet/minecraft/class_310;");

		/* @who is "you": the local player's PROFILE name, the same string the entries carry.

		   Best effort on purpose. A miss only costs the Client mark on your own card; the list
		   itself does not depend on it. */
		C.mc_player = env->GetFieldID(mc, "field_1724", "Lnet/minecraft/class_746;");
		C.mc_world  = env->GetFieldID(mc, "field_1687", "Lnet/minecraft/class_638;");
		if (env->ExceptionCheck()) env->ExceptionClear();

		if (jclass wc = ::vanta::jvm::findClassAnyLoader(env, "net/minecraft/class_638"))
		{
			C.world_getPlayers = env->GetMethodID(wc, "method_18456", "()Ljava/util/List;");
			env->DeleteLocalRef(wc);
		}
		if (env->ExceptionCheck()) env->ExceptionClear();

		if (jclass lv = ::vanta::jvm::findClassAnyLoader(env, "net/minecraft/class_1309"))
		{
			C.liv_getHealth    = env->GetMethodID(lv, "method_6032", "()F");
			C.liv_getMaxHealth = env->GetMethodID(lv, "method_6063", "()F");
			env->DeleteLocalRef(lv);
		}
		if (env->ExceptionCheck()) env->ExceptionClear();
		if (jclass pl = ::vanta::jvm::findClassAnyLoader(env, "net/minecraft/class_1657"))
		{
			C.player_profile = env->GetMethodID(pl, "method_7334", "()Lcom/mojang/authlib/GameProfile;");
			env->DeleteLocalRef(pl);
		}
		if (env->ExceptionCheck()) env->ExceptionClear();
		C.mc_network     = env->GetMethodID(mc, "method_1562", "()Lnet/minecraft/class_634;");
		C.mc_player      = env->GetFieldID(mc, "field_1724", "Lnet/minecraft/class_746;");
		C.net_playerList = env->GetMethodID(net, "method_2880", "()Ljava/util/Collection;");
		C.ple_profile    = env->GetMethodID(ple, "method_2966", "()Lcom/mojang/authlib/GameProfile;");
		C.ple_latency    = env->GetMethodID(ple, "method_2959", "()I");
		if (env->ExceptionCheck()) env->ExceptionClear();
        C.ple_display = optional(env, ple, "method_2971", "()Lnet/minecraft/class_2561;");
        if (auto text = ::vanta::jvm::findClassAnyLoader(env, "net/minecraft/class_2561")) {
            C.text_string = optional(env, text, "getString", "()Ljava/lang/String;"); env->DeleteLocalRef(text);
        }
        if (env->ExceptionCheck()) env->ExceptionClear();
        if (auto uuid = env->FindClass("java/util/UUID")) {
            C.uuid_string = optional(env, uuid, "toString", "()Ljava/lang/String;"); env->DeleteLocalRef(uuid);
        }

		if (jclass gp = ::vanta::jvm::findClassAnyLoader(env, "com/mojang/authlib/GameProfile"))
		{
			/* Authlib changed GameProfile from the legacy bean accessor getName()
			   to the record-style name() accessor. Support both so the roster works
			   across the launchers used by Minecraft 1.21.x. */
			for (const char* method : { "getName", "name" })
			{
				C.profile_name = env->GetMethodID(gp, method, "()Ljava/lang/String;");
				if (env->ExceptionCheck()) env->ExceptionClear();
				if (C.profile_name) break;
			}
            C.profile_id = optional(env, gp, "getId", "()Ljava/util/UUID;");
            if (!C.profile_id) C.profile_id = optional(env, gp, "id", "()Ljava/util/UUID;");
			env->DeleteLocalRef(gp);
		}
		if (jclass coll = env->FindClass("java/util/Collection"))
		{
			C.coll_iterator = env->GetMethodID(coll, "iterator", "()Ljava/util/Iterator;");
			env->DeleteLocalRef(coll);
		}
		if (jclass it = env->FindClass("java/util/Iterator"))
		{
			C.iter_hasNext = env->GetMethodID(it, "hasNext", "()Z");
			C.iter_next    = env->GetMethodID(it, "next", "()Ljava/lang/Object;");
			env->DeleteLocalRef(it);
		}
		if (env->ExceptionCheck()) env->ExceptionClear();

		const bool ok =
			want(C.mc_instance,    "MinecraftClient.getInstance")   &
			want(C.mc_network,     "MinecraftClient.getNetworkHandler") &
			want(C.net_playerList, "ClientPlayNetworkHandler.getPlayerList") &
			want(C.ple_profile,    "PlayerListEntry.getProfile")    &
			want(C.profile_name,   "GameProfile.getName/name")      &
			want(C.coll_iterator,  "Collection.iterator")           &
			want(C.iter_hasNext,   "Iterator.hasNext")              &
			want(C.iter_next,      "Iterator.next");

		if (!ok) return false;

		C.mc_class = static_cast<jclass>(env->NewGlobalRef(mc));
		if (!C.mc_class) return false;

		this->m_resolved = true;
		VLOG_INFO("roster", "tab list resolved");
		return true;
	}

	void c_roster::walk(JNIEnv* env)
	{
		/* One frame for the whole walk. A full server is sixty entries and each one costs an
		   entry ref, a profile ref and a string; the default frame is sixteen. */
		if (env->PushLocalFrame(256) != 0)
		{
			if (env->ExceptionCheck()) env->ExceptionClear();
			return;
		}

		struct scoped { JNIEnv* e; ~scoped() { e->PopLocalFrame(nullptr); } } frame{ env };

		jobject mc = env->CallStaticObjectMethod(C.mc_class, C.mc_instance);
		if (env->ExceptionCheck()) { env->ExceptionClear(); return; }
		if (!mc) return;

		jobject net = env->CallObjectMethod(mc, C.mc_network);
		if (env->ExceptionCheck()) { env->ExceptionClear(); return; }

		/* Null between worlds, and on the title screen. Clearing here rather than leaving the
		   last server's roster up is the difference between "nobody is on" and a ghost list. */
		if (!net) { this->m_entries.clear(); return; }

		jobject list = env->CallObjectMethod(net, C.net_playerList);
		if (env->ExceptionCheck()) { env->ExceptionClear(); return; }
		if (!list) { this->m_entries.clear(); return; }

		jobject it = env->CallObjectMethod(list, C.coll_iterator);
		if (env->ExceptionCheck()) { env->ExceptionClear(); return; }
		if (!it) return;

		std::string self_name;
		if (C.mc_player && C.player_profile)
		{
			if (jobject self = env->GetObjectField(mc, C.mc_player))
			{
				if (jobject profile = env->CallObjectMethod(self, C.player_profile))
				{
					self_name = read_string(env,
						static_cast<jstring>(env->CallObjectMethod(profile, C.profile_name)));
					env->DeleteLocalRef(profile);
				}
				env->DeleteLocalRef(self);
			}
			if (env->ExceptionCheck()) env->ExceptionClear();
		}

		std::vector<roster_entry_t> fresh;
		fresh.reserve(64);

		int guard = 0;
		while (guard++ < 1024)
		{
			const jboolean more = env->CallBooleanMethod(it, C.iter_hasNext);
			if (env->ExceptionCheck()) { env->ExceptionClear(); break; }
			if (!more) break;

			jobject entry = env->CallObjectMethod(it, C.iter_next);
			if (env->ExceptionCheck()) { env->ExceptionClear(); break; }
			if (!entry) continue;

			roster_entry_t e{};

			if (jobject profile = env->CallObjectMethod(entry, C.ple_profile))
			{
				e.m_name = read_string(env,
					static_cast<jstring>(env->CallObjectMethod(profile, C.profile_name)));
                if (env->ExceptionCheck()) env->ExceptionClear();
                if (C.profile_id && C.uuid_string) {
                    auto uuid = env->CallObjectMethod(profile, C.profile_id);
                    if (uuid && !env->ExceptionCheck()) {
                        e.m_uuid = read_string(env, static_cast<jstring>(env->CallObjectMethod(uuid, C.uuid_string)));
                        env->DeleteLocalRef(uuid);
                    }
                    if (env->ExceptionCheck()) env->ExceptionClear();
                }
                if (e.m_name == appearance::selectedName()) {
                    e.m_textures = texture_property(env, profile);
                    e.m_details_ready = true;
                }
				env->DeleteLocalRef(profile);
			}
			if (env->ExceptionCheck()) env->ExceptionClear();

			if (C.ple_latency)
			{
				e.m_ping = static_cast<int>(env->CallIntMethod(entry, C.ple_latency));
				if (env->ExceptionCheck()) { env->ExceptionClear(); e.m_ping = 0; }
			}

            if (C.ple_display && C.text_string) {
                auto display = env->CallObjectMethod(entry, C.ple_display);
                if (display && !env->ExceptionCheck()) {
                    e.m_display_name = read_string(env, static_cast<jstring>(env->CallObjectMethod(display, C.text_string)));
                    env->DeleteLocalRef(display);
                }
                if (env->ExceptionCheck()) env->ExceptionClear();
            }
            if (e.m_display_name.empty()) e.m_display_name = e.m_name;
			env->DeleteLocalRef(entry);

			e.m_self = !self_name.empty() && e.m_name == self_name;

			if (!e.m_name.empty()) fresh.push_back(std::move(e));
		}

		this->scan_world(env, mc, fresh);

		/* Alphabetical, case-insensitively. The server's own order is its internal map order —
		   it is stable for nobody and reshuffles as people join, which in a grid means cards
		   jumping under the cursor while you are reaching for one. */
		std::sort(fresh.begin(), fresh.end(),
			[](const roster_entry_t& a, const roster_entry_t& b)
			{
				return std::lexicographical_compare(
					a.m_name.begin(), a.m_name.end(), b.m_name.begin(), b.m_name.end(),
					[](char x, char y)
					{
						return std::tolower(static_cast<unsigned char>(x))
						     < std::tolower(static_cast<unsigned char>(y));
					});
			});

		this->m_entries.swap(fresh);
	}

	/* @who is actually loaded, and how much life they have left.

	   world.getPlayers() is only the entities inside your render distance, so intersecting it
	   with the tab list is what tells the panel who can be spectated — and, on the way past,
	   what their health is. Skipped whole when the lookups missed; every field it fills is
	   optional and its absence costs a feature, not the list.

	   Matched on the PROFILE name because that is what the tab list carries — a server can
	   rewrite a display name to anything, and the two would stop lining up. */
	void c_roster::scan_world(JNIEnv* env, void* mc_raw, std::vector<roster_entry_t>& into)
	{
		if (!C.mc_world || !C.world_getPlayers || !C.player_profile || !C.coll_iterator) return;

		jobject mc = static_cast<jobject>(mc_raw);
		if (!mc || into.empty()) return;

		/* Cleared first: somebody who walked out of render distance has to lose the flag, and
		   stale health on a card is worse than none. */
		for (roster_entry_t& e : into)
		{
			e.m_loaded = false;
			e.m_health = e.m_max_health = 0.f;
		}

		jobject world = env->GetObjectField(mc, C.mc_world);
		if (env->ExceptionCheck()) { env->ExceptionClear(); return; }
		if (!world) return;

		/* NOT `near`: windef.h still defines that as an empty macro for 16-bit compatibility,
		   so the declaration silently became `jobject = ...` and the block failed to parse. */
		if (jobject loaded = env->CallObjectMethod(world, C.world_getPlayers))
		{
			if (jobject wit = env->CallObjectMethod(loaded, C.coll_iterator))
			{
				int guard = 0;
				while (guard++ < 1024)
				{
					const jboolean more = env->CallBooleanMethod(wit, C.iter_hasNext);
					if (env->ExceptionCheck()) { env->ExceptionClear(); break; }
					if (!more) break;

					jobject p = env->CallObjectMethod(wit, C.iter_next);
					if (env->ExceptionCheck()) { env->ExceptionClear(); break; }
					if (!p) continue;

					std::string pn;
					if (jobject profile = env->CallObjectMethod(p, C.player_profile))
					{
						pn = read_string(env,
							static_cast<jstring>(env->CallObjectMethod(profile, C.profile_name)));
						env->DeleteLocalRef(profile);
					}
					if (env->ExceptionCheck()) env->ExceptionClear();

					float hp = 0.f, mhp = 0.f;
					if (C.liv_getHealth && C.liv_getMaxHealth)
					{
						hp  = static_cast<float>(env->CallFloatMethod(p, C.liv_getHealth));
						mhp = static_cast<float>(env->CallFloatMethod(p, C.liv_getMaxHealth));
						if (env->ExceptionCheck()) { env->ExceptionClear(); hp = mhp = 0.f; }
					}

					env->DeleteLocalRef(p);

					if (pn.empty()) continue;
					for (roster_entry_t& e : into)
						if (e.m_name == pn)
						{
							e.m_loaded     = true;
							e.m_health     = hp;
							e.m_max_health = mhp;
							break;
						}
				}
				env->DeleteLocalRef(wit);
			}
			env->DeleteLocalRef(loaded);
		}
		env->DeleteLocalRef(world);

		if (env->ExceptionCheck()) env->ExceptionClear();
	}

	void c_roster::poll_if_due(double now_seconds, bool wanted)
	{
		if (!wanted)
		{
			/* Not cleared. Re-opening the panel onto the list you last saw, for the quarter
			   second before the first walk lands, beats opening onto an empty grid that fills
			   in — which reads as the feature having been broken and then recovered. */
			return;
		}

		/* @two clocks, because the two halves age at completely different speeds.

		   Between full walks, the world pass runs on its own — it is the cheap half (a dozen
		   loaded entities against sixty tab entries, and no per-entry latency read) and it
		   carries the one number that moves in real time. At four hertz a health readout is a
		   quarter of a second behind, which over a burst of hits is the difference between
		   "he is nearly dead" and "he was". */
		if (now_seconds >= this->m_next_poll)
		{
			this->m_next_poll   = now_seconds + 0.25;
			this->m_next_health = now_seconds + 0.1;

			::vanta::jvm::JniScope scope;
			if (!scope) return;

			if (!this->resolve(scope.get())) return;
			this->walk(scope.get());
			return;
		}

		if (now_seconds < this->m_next_health) return;
		this->m_next_health = now_seconds + 0.1;

		if (!this->m_resolved || this->m_entries.empty()) return;

		::vanta::jvm::JniScope scope;
		if (!scope) return;

		JNIEnv* env = scope.get();
		if (env->PushLocalFrame(64) != 0)
		{
			if (env->ExceptionCheck()) env->ExceptionClear();
			return;
		}
		struct scoped { JNIEnv* e; ~scoped() { e->PopLocalFrame(nullptr); } } frame{ env };

		jobject mc = env->CallStaticObjectMethod(C.mc_class, C.mc_instance);
		if (env->ExceptionCheck()) { env->ExceptionClear(); return; }
		if (!mc) return;

		this->scan_world(env, mc, this->m_entries);
	}
}
