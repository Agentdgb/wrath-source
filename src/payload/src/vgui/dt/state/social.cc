#include "../../includes.hh"

#include "core/social/FriendList.h"

namespace dt
{
	/* @empty on purpose.

	   This used to seed Kaori, Nyx, Ryu, Mira and Volk, three of them with
	   m_friend = true. That was harmless only while the flag went nowhere. Now
	   that it reaches targeting and the profile file, seeded fakes would be
	   written to disk as real friends on the first autosave and would then be
	   indistinguishable from names the user actually picked. */
	c_social::c_social() = default;

	person_t* c_social::find(const std::string& name)
	{
		for (auto& person : this->m_people)
		{
			if (person.m_name == name)
				return &person;
		}

		return nullptr;
	}

	person_t* c_social::remember(const std::string& name, const std::string& avatar)
	{
		if (person_t* existing = this->find(name); existing != nullptr)
			return existing;

		person_t person{};
		person.m_name = name;
		person.m_avatar = avatar;
		person.m_status = "Met in chat";

		this->m_people.push_back(std::move(person));
		return &this->m_people.back();
	}

	/* @the flag goes straight to core, and it no longer needs the roster.

	   The old body did nothing at all unless find() already had the name, which
	   is why player_list.cc has to call remember() first (that call is now
	   redundant but harmless) and why chat.cc's "Add friend" — which does NOT
	   call remember() — silently did nothing for anybody who was not one of the
	   seeded fakes. */
	void c_social::set_friend(const std::string& name, bool value)
	{
		if (name.empty())
			return;

		::dt::FriendList::get().set(name, value);
	}

	bool c_social::is_friend(const std::string& name)
	{
		return ::dt::FriendList::get().contains(name);
	}

	std::vector<person_t*> c_social::friends()
	{
		std::vector<person_t*> result{};

		for (auto& person : this->m_people)
		{
			if (::dt::FriendList::get().contains(person.m_name))
				result.push_back(&person);
		}

		// online first, then alphabetical, which is the order a roster is expected to be read in
		std::sort(result.begin(), result.end(), [](const person_t* a, const person_t* b)
			{
				if (a->m_online != b->m_online)
					return a->m_online;

				return a->m_name < b->m_name;
			});

		return result;
	}

	std::vector<person_t*> c_social::suggestions()
	{
		std::vector<person_t*> result{};

		for (auto& person : this->m_people)
		{
			if (!::dt::FriendList::get().contains(person.m_name))
				result.push_back(&person);
		}

		return result;
	}
}
