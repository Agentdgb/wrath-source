#pragma once

/* @dt::c_social:
	the people the client knows about, and which of them are friends.

	shared state rather than a member of either view: the chat adds people to the roster when it
	meets them, the friends page reads that roster, and both can flip the friend flag.
*/
namespace dt
{
	struct person_t
	{
		std::string m_name{};
		std::string m_avatar{};
		std::string m_status{};

		/* NO m_friend. The friend flag lives in dt::FriendList (core/social/
		   FriendList.h) because the modules and ConfigManager both need it and
		   neither may include a vgui header. set_friend()/is_friend() below
		   forward to it, so this struct holds presentation only. */
		bool m_online{ true };
	};

	class c_social
	{
	public:
		c_social();

		/* @find:
			the roster entry for a name, or null when nobody answers to it
		*/
		person_t* find(const std::string& name);

		/* @remember:
			adds someone to the roster if they are new, and returns them either way
		*/
		person_t* remember(const std::string& name, const std::string& avatar);

		void set_friend(const std::string& name, bool value);
		bool is_friend(const std::string& name);

		std::vector<person_t*> friends();
		std::vector<person_t*> suggestions();

		const std::vector<person_t>& people() const { return this->m_people; }

	private:
		std::vector<person_t> m_people{};
	};
	inline auto g_social = std::make_unique<c_social>();
}
