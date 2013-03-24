#include "Profile.h"

#include <algorithm>
#include <vector>
#include <climits>
#include <linux/perf_event.h>

#ifndef PERF_MAX_STACK_DEPTH
#define PERF_MAX_STACK_DEPTH 127
#endif

#ifndef NDEBUG
#include <iostream>
#endif

namespace pe {

/// Data about mmap event
struct mmap_event
{
  __u32 pid;
  __u32 tid;
  __u64 address;
  __u64 length;
  /// @todo Determine how to handle pgoff
  __u64 pageOffset;
  char fileName[PATH_MAX];
};

/// Data about sample event
/** We have enabled only PERF_SAMPLE_IP and PERF_SAMPLE_CALLCHAIN in \ref createPerfEvent in \ref pgcollect.c,
 *  so we define only this small subset of fields. */
struct sample_event
{
  __u64   ip;
  __u64   callchainSize;
  __u64   callchain[PERF_MAX_STACK_DEPTH];
};

struct perf_event
{
  struct perf_event_header header;
  union {
    mmap_event mmap;
    sample_event sample;
  };
};

std::istream& operator>>(std::istream& is, perf_event& event)
{
  is.read((char*)&event, sizeof(perf_event_header));
  is.read(((char*)&event) + sizeof(perf_event_header), event.header.size - sizeof(perf_event_header));
  return is;
}

}

void EntryData::appendBranch(Address address, Count count)
{
  BranchStorage::iterator branchIt = branches_.find(address);
  if (branchIt == branches_.end())
    branches_.insert(Branch(address, count));
  else
    branchIt->second += count;
}

EntryData& MemoryObjectData::appendEntry(Address address, Count count)
{
  EntryStorage::iterator entryIt = entries_.find(address);
  if (entryIt == entries_.end())
    entryIt = entries_.insert(Entry(address, EntryData(count))).first;
  else
    entryIt->second.addCount(count);

  return entryIt->second;
}

void MemoryObjectData::appendBranch(Address from, Address to, Count count)
{
  appendEntry(from, 0).appendBranch(to, count);
}

void MemoryObjectData::fixupBranches(const SymbolStorage& symbols)
{
  // Fixup branches
  // Call "to" address should point to first address of called function,
  // this will allow group them as well
  for (EntryStorage::iterator entryIt = entries_.begin(); entryIt != entries_.end(); ++entryIt)
  {
    EntryData& entryData = entryIt->second;
    if (entryData.branches().size() == 0)
      continue;

    EntryData fixedEntry(entryData.count());
    for (BranchStorage::const_iterator branchIt = entryData.branches().begin(); branchIt != entryData.branches().end();
         ++branchIt)
    {
      SymbolStorage::const_iterator symIt = symbols.find(Range(branchIt->first));
      if (symIt != symbols.end())
        fixedEntry.appendBranch(symIt->first.start, branchIt->second);
      else
        fixedEntry.appendBranch(branchIt->first, branchIt->second);
    }
    entryData.swap(fixedEntry);
  }
}

class ProfilePrivate
{
  friend class Profile;
  ProfilePrivate()
    : mmapEventCount(0)
    , goodSamplesCount(0)
    , badSamplesCount(0)
  {}

  void processMmapEvent(const pe::mmap_event &event);
  void processSampleEvent(const pe::sample_event &event, Profile::Mode mode);

  MemoryObjectStorage memoryObjects;
  SymbolStorage symbols;

  size_t mmapEventCount;
  size_t goodSamplesCount;
  size_t badSamplesCount;
};

void ProfilePrivate::processMmapEvent(const pe::mmap_event &event)
{
#ifndef NDEBUG
  std::pair<MemoryObjectStorage::const_iterator, bool> insRes =
#endif
  memoryObjects.insert(MemoryObject(Range(event.address, event.address + event.length),
                                    new MemoryObjectData(event.fileName)));
#ifndef NDEBUG
  if (!insRes.second)
  {
    std::cerr << "Memory object was not inserted! " << event.address << " " << event.length << " "
              << event.fileName << '\n';
    std::cerr << "Already have another object: " << (insRes.first->first.start) << ' '
              << (insRes.first->first.end) << ' ' << insRes.first->second->fileName() << '\n';
    for (MemoryObjectStorage::const_iterator it = memoryObjects.begin(); it != memoryObjects.end(); ++it)
      std::cerr << it->first.start << ' ' << it->first.end << ' ' << it->second->fileName() << '\n';
    std::cerr << std::endl;
  }
#endif
  mmapEventCount++;
}

void ProfilePrivate::processSampleEvent(const pe::sample_event &event, Profile::Mode mode)
{
  if (event.callchain[0] != PERF_CONTEXT_USER || event.callchainSize < 2 || event.callchainSize > PERF_MAX_STACK_DEPTH)
  {
    badSamplesCount++;
    return;
  }

  MemoryObjectStorage::iterator objIt = memoryObjects.find(Range(event.ip));
  if (objIt == memoryObjects.end())
  {
    badSamplesCount++;
    return;
  }

  objIt->second->appendEntry(event.ip, 1);
  goodSamplesCount++;

  if (mode != Profile::CallGraph)
    return;

  bool skipFrame = false;
  Address callTo = event.ip;

  for (__u64 i = 2; i < event.callchainSize; ++i)
  {
    Address callFrom = event.callchain[i];
    if (callFrom > PERF_CONTEXT_MAX)
    {
      // Context switch, and we want only user level
      skipFrame = (callFrom != PERF_CONTEXT_USER);
      continue;
    }
    if (skipFrame || callFrom == callTo)
      continue;

    objIt = memoryObjects.find(Range(callFrom));
    if (objIt == memoryObjects.end())
      continue;

    objIt->second->appendBranch(callFrom, callTo, 1);

    callTo = callFrom;
  }
}

Profile::Profile() : d(new ProfilePrivate)
{}

Profile::~Profile()
{
  for (MemoryObjectStorage::iterator objIt = d->memoryObjects.begin(); objIt != d->memoryObjects.end(); ++objIt)
    delete objIt->second;
  delete d;
}

void Profile::load(std::istream &is, Mode mode)
{
  pe::perf_event event;
  while (!is.eof() && !is.fail())
  {
    is >> event;
    if (is.eof() || is.fail())
      break;
    switch (event.header.type)
    {
    case PERF_RECORD_MMAP:
      d->processMmapEvent(event.mmap);
      break;
    case PERF_RECORD_SAMPLE:
      d->processSampleEvent(event.sample, mode);
    }
  }

  // Drop memory objects that don't have any entries
  MemoryObjectStorage::iterator objIt = d->memoryObjects.begin();
  while (objIt != d->memoryObjects.end())
  {
    if (objIt->second->entries().size() == 0)
    {
      // With C++11 we can just do:
      // objIt = d->memoryObjects.erase(objIt);
      d->memoryObjects.erase(objIt++);
    }
    else
      ++objIt;
  }
}

size_t Profile::mmapEventCount() const
{
  return d->mmapEventCount;
}

size_t Profile::goodSamplesCount() const
{
  return d->goodSamplesCount;
}

size_t Profile::badSamplesCount() const
{
  return d->badSamplesCount;
}

void Profile::fixupBranches()
{
  for (MemoryObjectStorage::iterator objIt = d->memoryObjects.begin(); objIt != d->memoryObjects.end(); ++objIt)
    objIt->second->fixupBranches(d->symbols);
}

const MemoryObjectStorage& Profile::memoryObjects() const
{
  return d->memoryObjects;
}

MemoryObjectStorage& Profile::memoryObjects()
{
  return d->memoryObjects;
}

const SymbolStorage& Profile::symbols() const
{
  return d->symbols;
}

SymbolStorage& Profile::symbols()
{
  return d->symbols;
}
