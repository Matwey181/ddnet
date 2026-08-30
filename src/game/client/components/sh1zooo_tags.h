/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef GAME_CLIENT_COMPONENTS_SH1ZOOO_TAGS_H
#define GAME_CLIENT_COMPONENTS_SH1ZOOO_TAGS_H

#include <base/detect.h>
#include <base/system.h>

#include <engine/shared/protocol.h>
#include <engine/storage.h>

#include <vector>

/**
 * sh1zooo client: persistent WAR/TEAM tags for players.
 *
 * Players tagged as WAR show up with a "[WAR]" prefix (red) and an angry
 * tee, players tagged as TEAM show up with a "[TEAM]" prefix (green) and a
 * happy tee. The tags are stored by player name in
 * "sh1zooo_tags.txt" inside the save directory and survive restarts.
 *
 * The tag list is small (a handful of entries), so lookups do a linear
 * search by name, which is called at most a few dozen times per frame.
 */
class CSh1zoooTags
{
public:
	enum
	{
		TAG_NONE = 0,
		TAG_WAR = 1,
		TAG_TEAM = 2,
	};

	struct STag
	{
		char m_aName[MAX_NAME_LENGTH];
		int m_Tag;
	};

private:
	std::vector<STag> m_vTags;
	IStorage *m_pStorage = nullptr;

public:
	/**
	 * Loads the tags from "sh1zooo_tags.txt" in the save directory and
	 * remembers the storage pointer for future saves.
	 */
	void Load(IStorage *pStorage);

	/**
	 * Writes the current tag list to disk. Called automatically whenever
	 * the list changes.
	 */
	void Save() const;

	/**
	 * Returns TAG_NONE, TAG_WAR or TAG_TEAM for the given player name.
	 * Case-sensitive, like the game itself treats names.
	 */
	int Tag(const char *pName) const;

	/**
	 * Sets or removes the tag for the given player name and saves the
	 * list. TAG_NONE removes an existing tag.
	 */
	void SetTag(const char *pName, int Tag);

	/**
	 * Number of tagged players.
	 */
	size_t Size() const { return m_vTags.size(); }
};

#endif // GAME_CLIENT_COMPONENTS_SH1ZOOO_TAGS_H
