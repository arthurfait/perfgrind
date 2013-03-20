#include "AddressResolver.h"

#include <algorithm>
#include <map>
#include <sstream>
#include <cstring>

#include <fcntl.h>

#include <elfutils/libdwfl.h>

#ifndef NDEBUG
#include <iostream>
#endif

enum Section {
  SymTab = 0,
  DynSym,
  DebugInfo,
  DebugLink,
  PrelinkUndo,
  SectionCount
};

class ElfHolder
{
public:
  explicit ElfHolder(const char* fileName);
  ~ElfHolder();

  void open(const char* fileName);
  void close();

  Elf* get() { return elf_; }
  Elf_Scn* getSection(Section section) { return sections_[section]; }
  uint64_t getBaseAddress() const { return baseAddress_; }
private:
  ElfHolder(const ElfHolder&);
  ElfHolder& operator=(const ElfHolder&);

  void loadInfo();

  uint64_t baseAddress_;
  Elf* elf_;
  Elf_Scn* sections_[SectionCount];
  int fd_;
};

ElfHolder::ElfHolder(const char *fileName)
  : baseAddress_(0)
{
  memset(sections_, 0, sizeof(sections_));
  fd_ = ::open(fileName, O_RDONLY);
  elf_ = elf_begin(fd_, ELF_C_READ, 0);
  loadInfo();
}

ElfHolder::~ElfHolder()
{
  elf_end(elf_);
  ::close(fd_);
}

void ElfHolder::open(const char *fileName)
{
  elf_end(elf_);
  if (fd_)
    ::close(fd_);

  baseAddress_ = 0;
  memset(sections_, 0, sizeof(sections_));

  fd_ = ::open(fileName, O_RDONLY);
  elf_ = elf_begin(fd_, ELF_C_READ, 0);
  loadInfo();
}

void ElfHolder::close()
{
  elf_end(elf_);
  ::close(fd_);
  elf_ = 0;
  fd_ = -1;
  baseAddress_ = 0;
  memset(sections_, 0, sizeof(sections_));
}

void ElfHolder::loadInfo()
{
  if (!elf_)
    return;

  // Detect base address
  size_t phCount;
  elf_getphdrnum(elf_, &phCount);
  for (size_t i = 0; i < phCount; i++)
  {
    GElf_Phdr phdr;
    gelf_getphdr(elf_, i, &phdr);
    if (phdr.p_type == PT_LOAD)
    {
      baseAddress_ = phdr.p_vaddr;
      break;
    }
  }

  // Find sections we are interested in
  GElf_Ehdr ehdr;
  gelf_getehdr(elf_, &ehdr);

  unsigned needToFind = SectionCount;
  size_t shCount;
  elf_getshdrnum(elf_, &shCount);
  for (size_t i = 0; i < shCount && needToFind > 0; i++)
  {
    Elf_Scn* scn = elf_getscn(elf_, i);
    GElf_Shdr shdr;
    gelf_getshdr(scn, &shdr);

    switch (shdr.sh_type)
    {
    case SHT_SYMTAB:
      sections_[SymTab] = scn;
      needToFind--;
      break;
    case SHT_DYNSYM:
      sections_[DynSym] = scn;
      needToFind--;
      break;
    case SHT_PROGBITS:
      const char* sectionName = elf_strptr(elf_, ehdr.e_shstrndx, shdr.sh_name);
      if (strcmp(sectionName, ".debug_info") == 0)
      {
        sections_[DebugInfo] = scn;
        needToFind--;
      }
      else if (strcmp(sectionName, ".gnu_debuglink") == 0)
      {
        sections_[DebugLink] = scn;
        needToFind--;
      }
      else if (strcmp(sectionName, ".gnu.prelink_undo") == 0)
      {
        sections_[PrelinkUndo] = scn;
        needToFind--;
      }
      break;
    }
  }
}

struct ARSymbolData
{
  explicit ARSymbolData(const GElf_Sym& elfSymbol)
    : size(elfSymbol.st_size)
    , binding(GELF_ST_BIND(elfSymbol.st_info))
  {}
  explicit ARSymbolData(uint64_t _size)
    : size(_size)
  {}
  uint64_t size;
  std::string name;
  unsigned char binding;
};

typedef std::map<Range, ARSymbolData> ARSymbolStorage;
typedef ARSymbolStorage::value_type ARSymbol;

struct AddressResolverPrivate
{
  AddressResolverPrivate()
    : baseAddress(0)
    , origBaseAddress(0)
  {}

  void loadSymbolsFromSection(Elf* elf, Elf_Scn* section);
  const char* getDebugLink(Elf_Scn* section);
  void setOriginalBaseAddress(Elf* elf, Elf_Scn* section);

  void constructFakeSymbols(uint64_t objectSize, const char* baseName);

  uint64_t baseAddress;
  uint64_t origBaseAddress;

//  Dwfl* dwfl_;
//  Dwfl_Module* dwMod_;
//  GElf_Addr dwBias_;

  ARSymbolStorage symbols;
};


AddressResolver::AddressResolver(const char *fileName, uint64_t objectSize)
  : d(new AddressResolverPrivate)

{
  elf_version(EV_CURRENT);
  ElfHolder elfh(fileName);
  d->origBaseAddress = d->baseAddress = elfh.getBaseAddress();

  bool symTabLoaded = false;
  // Try to load .symtab from main file
  if (elfh.getSection(SymTab))
  {
    d->loadSymbolsFromSection(elfh.get(), elfh.getSection(SymTab));
    symTabLoaded = true;
  }
  else if (elfh.getSection(DynSym))
    // Try to load .dynsym from main file
    d->loadSymbolsFromSection(elfh.get(), elfh.getSection(DynSym));

  if (elfh.getSection(PrelinkUndo) && elfh.getSection(DebugLink))
    d->setOriginalBaseAddress(elfh.get(), elfh.getSection(PrelinkUndo));

  if (elfh.getSection(DebugLink))
  {
    // Get name
    std::string debugModuleName = "/usr/lib/debug";
    debugModuleName.append(fileName);
    debugModuleName.append(".debug");
    /// @todo Use debug link

    if (!symTabLoaded)
    {
      elfh.close();
      elfh.open(debugModuleName.c_str());
      if (elfh.getSection(SymTab))
        d->loadSymbolsFromSection(elfh.get(), elfh.getSection(SymTab));
    }
  }

  elfh.close();

  d->constructFakeSymbols(objectSize, basename(fileName));

  // Setup dwfl for sources positions fetching
  //  dwfl_ = dwfl_begin(&callbacks);
  //  dwMod_ = dwfl_report_offline(dwfl_, "", object.fileName().c_str(), -1);
  /// @todo What it is bias and how to use it?
}

AddressResolver::~AddressResolver()
{
  delete d;
//  if (dwfl_)
//    dwfl_report_end(dwfl_, 0, 0);
//  dwfl_end(dwfl_);
}

static std::string constructSymbolName(uint64_t address)
{
  std::stringstream ss;
  ss << "func_" << std::hex << address;
  return ss.str();
}

void AddressResolver::resolve(EntryStorage::const_iterator first, EntryStorage::const_iterator last, uint64_t loadBase,
                              SymbolStorage& symbols)
{
  uint64_t adjust = loadBase - d->baseAddress;
  while (first != last)
  {
    ARSymbolStorage::const_iterator arSymIt = d->symbols.find(Range(first->first - adjust));
    if (arSymIt == d->symbols.end())
    {
#ifndef NDEBUG
    std::cerr << "Can't resolve symbol for address " << std::hex << first->first - adjust
              << ", load base: " << loadBase << std::dec << '\n';
#endif
    ++first;
    continue;
    }

    Symbol symbol(Range(arSymIt->first.start + adjust, arSymIt->first.end + adjust),
                  SymbolData(arSymIt->second.name.empty() ? constructSymbolName(arSymIt->first.start) :
                                                            arSymIt->second.name));
    symbols.insert(symbol);

    do { ++first; }
    while (first != last && first->first - adjust < arSymIt->first.end);
  }
}

void AddressResolverPrivate::loadSymbolsFromSection(Elf* elf, Elf_Scn *section)
{
  symbols.clear();

  GElf_Shdr sectionHeader;
  gelf_getshdr(section, &sectionHeader);

  Elf_Data* sectionData = elf_getdata(section, 0);
  size_t symbolCount = sectionHeader.sh_size / (sectionHeader.sh_entsize ? sectionHeader.sh_entsize : 1);

  for (size_t symIdx = 0; symIdx < symbolCount; symIdx++)
  {
    GElf_Sym elfSymbol;
    gelf_getsym(sectionData, symIdx, &elfSymbol);

    if (GELF_ST_TYPE(elfSymbol.st_info) != STT_FUNC || elfSymbol.st_shndx == SHN_UNDEF)
      continue;

    ARSymbolData symbolData(elfSymbol);
    uint64_t symStart = elfSymbol.st_value - origBaseAddress + baseAddress;
    uint64_t symEnd = symStart + (elfSymbol.st_size ?: 1);

    std::pair<ARSymbolStorage::iterator, bool> insResult =
        symbols.insert(ARSymbol(Range(symStart, symEnd), symbolData));
    if (insResult.second)
      insResult.first->second.name = elf_strptr(elf, sectionHeader.sh_link, elfSymbol.st_name);
    else
    {
      const ARSymbolData& oldSymbolData = insResult.first->second;
      // Sized functions better that asm labels and higer binding is also better
      if ((oldSymbolData.size == 0 && symbolData.size != 0) || (oldSymbolData.binding < symbolData.binding))
      {
        symbols.erase(insResult.first);
        symbolData.name = elf_strptr(elf, sectionHeader.sh_link, elfSymbol.st_name);
        symbols.insert(ARSymbol(Range(symStart, symEnd), symbolData));
      }
    }
  }
}

//const char* AddressResolver::getDebugLink(Elf_Scn* section)
//{
//  Elf_Data* sectionData = elf_rawdata(section, 0);
//  return (char*)sectionData->d_buf;
//}

void AddressResolverPrivate::setOriginalBaseAddress(Elf *elf, Elf_Scn* section)
{
  Elf_Data* sectionData = elf_rawdata(section, 0);
  // Allmost direct copy-paste from elfutils/libdwfl/dwfl_module_getdwarf.c
  union
  {
    Elf32_Ehdr e32;
    Elf64_Ehdr e64;
  } ehdr;

  Elf_Data destination;
  destination.d_buf = &ehdr;
  destination.d_size = sizeof(ehdr);
  destination.d_type = ELF_T_EHDR;
  destination.d_version = EV_CURRENT;

  Elf_Data source = *sectionData;
  source.d_size = gelf_fsize(elf, ELF_T_EHDR, 1, EV_CURRENT);
  source.d_type = ELF_T_EHDR;

  unsigned int encode = elf_getident(elf, NULL)[EI_DATA];

  gelf_xlatetom(elf, &destination, &source, encode);

  unsigned phnum;
  if (ehdr.e32.e_ident[EI_CLASS] == ELFCLASS32)
    phnum = ehdr.e32.e_phnum;
  else
    phnum = ehdr.e64.e_phnum;

  size_t phentsize = gelf_fsize(elf, ELF_T_PHDR, 1, EV_CURRENT);
  source.d_buf = (char*)source.d_buf + source.d_size;
  source.d_type = ELF_T_PHDR;
  source.d_size = phnum * phentsize;

  Elf64_Phdr phdr64[phnum];
  Elf32_Phdr* phdr32 = (Elf32_Phdr*)phdr64;
  destination.d_buf = &phdr64;
  destination.d_size = sizeof(phdr64);
  gelf_xlatetom(elf, &destination, &source, encode);
  if (ehdr.e32.e_ident[EI_CLASS] == ELFCLASS32)
  {
    for (unsigned i = 0; i < phnum; ++i)
      if (phdr32[i].p_type == PT_LOAD)
      {
        origBaseAddress = phdr32[i].p_vaddr;
        break;
      }
  }
  else
    for (unsigned i = 0; i < phnum; ++i)
      if (phdr64[i].p_type == PT_LOAD)
      {
        origBaseAddress = phdr64[i].p_vaddr;
        break;
      }
}

void AddressResolverPrivate::constructFakeSymbols(uint64_t objectSize, const char* baseName)
{
  // Create fake symbols to cover gaps
  ARSymbolStorage newSymbols;
  uint64_t prevEnd = baseAddress;
  for (ARSymbolStorage::iterator symIt = symbols.begin(); symIt != symbols.end(); ++symIt)
  {
    const Range& symRange = symIt->first;
    if (symRange.start - prevEnd >= 4)
      newSymbols.insert(ARSymbol(Range(prevEnd, symRange.start), ARSymbolData(symRange.start - prevEnd)));

    // Expand asm label to next symbol
    if (symIt->second.size == 0)
    {
      ARSymbolStorage::iterator nextSymIt = symIt;
      ++nextSymIt;
      uint64_t newEnd;
      if (nextSymIt == symbols.end())
        newEnd = baseAddress + objectSize;
      else
        newEnd = nextSymIt->first.start;

      ARSymbolData newSymbolData(newEnd - symRange.start);
      newSymbolData.name = symIt->second.name.append(1, '@').append(baseName);

      newSymbols.insert(ARSymbol(Range(symRange.start, newEnd), newSymbolData));

      prevEnd = newEnd;
    }
    else
    {
      newSymbols.insert(*symIt);
      prevEnd = symRange.end;
    }
  }
  if (baseAddress + objectSize - prevEnd >= 4)
    newSymbols.insert(ARSymbol(Range(prevEnd, baseAddress + objectSize), ARSymbolData(baseAddress + objectSize - prevEnd)));

  symbols.swap(newSymbols);
}