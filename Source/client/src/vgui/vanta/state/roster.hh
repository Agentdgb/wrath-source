#pragma once

/* @vanta::c_roster:
	who is actually on the server, read from the TAB LIST.

	deliberately not the entities around you. the entity list holds whoever is loaded in your own
	render distance — a handful on a big server — while the tab list is the roster the server
	itself publishes, which is what "the players" means and what the count in the header has to
	agree with.

	kept separate from c_social on purpose. c_social is a persistent address book: people you have
	decided something about, surviving disconnects. this is a live snapshot, replaced wholesale
	every walk and meaningless the moment you leave. one is memory, the other is a reading, and
	merging them would mean deciding when a stranger who walked past becomes a contact.

	render-thread owned. the only writer and the only reader are both inside c_app::frame(), so
	there is no lock and no need for one.

	intermediary names, grepped out of tools/genmaps/.cache/<ver>/yarn_merged.tiny and identical
	on 1.21, 1.21.5 and 1.21.11:
	    class_310  MinecraftClient           method_1562  getNetworkHandler ()Lclass_634;
	    class_634  ClientPlayNetworkHandler  method_2880  getPlayerList ()Ljava/util/Collection;
	    class_640  PlayerListEntry           method_2966  getProfile ()Lcom/mojang/authlib/GameProfile;
	                                         method_2959  getLatency ()I
*/

struct JNIEnv_;
typedef JNIEnv_ JNIEnv;

namespace vanta
{
	struct roster_entry_t
	{
		std::string m_name{};
		int  m_ping{ 0 };
		bool m_self{ false };

		/* @is this person actually IN the world, not just on the tab list.

		   The tab list names everyone on the server; only the ones inside your
		   render distance have an entity, and only those can be spectated or
		   drawn. The panel's range filter is this flag. */
		bool m_loaded{ false };

		/* @health, off the same entity, in the same walk.

		   m_max_health is the "do we know" flag as well as the denominator: it
		   is zero for everyone the tab list names but the world has not loaded,
		   and a living entity's max is never zero. Testing that instead of
		   m_loaded keeps the two independent — the entity can be there while
		   the health lookup missed. */
		float m_health{ 0.f };
		float m_max_health{ 0.f };
	};

	class c_roster
	{
	public:
		/* @poll_if_due:
			walks the tab list at most four times a second, and only when `wanted`.

			gated because a roster nobody is looking at is a few hundred jni calls a frame for a
			list that is not on screen. four hertz because people join and leave on a human
			timescale; the panel is not an fps counter.
		*/
		void poll_if_due(double now_seconds, bool wanted);

		const std::vector<roster_entry_t>& entries() const { return this->m_entries; }

		/* false once resolve() has run and missed. the view uses it to say WHY the list is empty
		   rather than showing an empty grid that looks the same as an empty server. */
		bool resolved() const { return this->m_resolved; }
		bool attempted() const { return this->m_attempted; }

	private:
		bool resolve(JNIEnv* env);
		void walk(JNIEnv* env);

		/* @the world pass, split out because it runs at its OWN rate.

			the tab list is a human-timescale thing — people join and leave — but health is not,
			and a list that showed someone at full a quarter of a second after they were beaten
			down is a list you cannot use for the one thing you would open it for.

			so this half runs on its own clock. it is also much the cheaper half: getPlayers() is
			only what your render distance holds, a dozen or so, against sixty on the tab list.

			`into` is scanned in place — every entry's m_loaded and health are cleared first, so
			walking out of render distance takes the flag away again.
		*/
		void scan_world(JNIEnv* env, void* mc, std::vector<roster_entry_t>& into);

		std::vector<roster_entry_t> m_entries{};

		double m_next_poll{ 0.0 };
		double m_next_health{ 0.0 };
		bool   m_resolved{ false };
		bool   m_attempted{ false };
	};

	inline auto g_roster = std::make_unique<c_roster>();
}
