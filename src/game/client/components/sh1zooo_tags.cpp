/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include "sh1zooo_tags.h"

#include <base/log.h>

#include <engine/shared/linereader.h>

static const char *SH1ZOOO_TAGS_FILENAME = "sh1zooo_tags.txt";

void CSh1zoooTags::Load(IStorage *pStorage)
{
	m_pStorage = pStorage;
	m_vTags.clear();

	if(m_pStorage == nullptr)
		return;

	IOHANDLE File = m_pStorage->OpenFile(SH1ZOOO_TAGS_FILENAME, IOFLAG_READ, IStorage::TYPE_SAVE);
	if(!File)
	{
		// No file yet - not an error, the list is simply empty.
		return;
	}

	CLineReader LineReader;
	LineReader.OpenFile(File);
	const char *pLine;
	while((pLine = LineReader.Get()) != nullptr)
	{
		const char *pTag = str_utf8_skip_whitespaces(pLine);
		if(pTag[0] == '\0' || pTag[0] == '#')
			continue;

		int TagType = TAG_NONE;
		if(pTag[0] == 'W' || pTag[0] == 'w')
			TagType = TAG_WAR;
		else if(pTag[0] == 'T' || pTag[0] == 't')
			TagType = TAG_TEAM;
		if(TagType == TAG_NONE)
			continue;

		// Everything after the first tab is the name (names may contain
		// spaces, so the whole rest of the line is used).
		const char *pName = str_utf8_skip_whitespaces(pTag + 1);
		if(pName[0] == '\0')
			continue;

		STag Entry;
		str_copy(Entry.m_aName, pName, sizeof(Entry.m_aName));
		str_utf8_trim_right(Entry.m_aName);
		if(Entry.m_aName[0] == '\0')
			continue;
		Entry.m_Tag = TagType;

		// Ignore duplicate entries, the first one wins.
		if(Tag(Entry.m_aName) != TAG_NONE)
			continue;
		m_vTags.push_back(Entry);
	}

	io_close(File);
	log_info("sh1zooo", "Loaded %d player tag(s)", (int)m_vTags.size());
}

void CSh1zoooTags::Save() const
{
	if(m_pStorage == nullptr)
		return;

	IOHANDLE File = m_pStorage->OpenFile(SH1ZOOO_TAGS_FILENAME, IOFLAG_WRITE, IStorage::TYPE_SAVE);
	if(!File)
	{
		log_error("sh1zooo", "Could not write '%s'", SH1ZOOO_TAGS_FILENAME);
		return;
	}

	for(const STag &Entry : m_vTags)
	{
		char aBuf[MAX_NAME_LENGTH + 8];
		str_format(aBuf, sizeof(aBuf), "%c\t%s\n",
			Entry.m_Tag == TAG_WAR ? 'W' : 'T',
			Entry.m_aName);
		io_write(File, aBuf, str_length(aBuf));
	}

	io_close(File);
}

int CSh1zoooTags::Tag(const char *pName) const
{
	if(pName == nullptr || pName[0] == '\0')
		return TAG_NONE;
	for(const STag &Entry : m_vTags)
	{
		if(str_comp(Entry.m_aName, pName) == 0)
			return Entry.m_Tag;
	}
	return TAG_NONE;
}

void CSh1zoooTags::SetTag(const char *pName, int Tag)
{
	if(pName == nullptr || pName[0] == '\0')
		return;

	for(size_t i = 0; i < m_vTags.size(); i++)
	{
		if(str_comp(m_vTags[i].m_aName, pName) == 0)
		{
			if(Tag == TAG_NONE)
				m_vTags.erase(m_vTags.begin() + i);
			else
				m_vTags[i].m_Tag = Tag;
			Save();
			return;
		}
	}

	if(Tag == TAG_NONE)
		return;

	STag Entry;
	str_copy(Entry.m_aName, pName, sizeof(Entry.m_aName));
	Entry.m_Tag = Tag;
	m_vTags.push_back(Entry);
	Save();
}
