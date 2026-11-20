// Native offline NFS HP2 PS2 old-ZDIR utility. Not linked into the game runtime.
#include "Sha256.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#elif defined(__linux__)
#include <cerrno>
#include <fcntl.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
namespace {
using hp2archive::Sha256;
constexpr std::size_t BufferSize = 1024 * 1024;
constexpr const char* ManifestName = "NFSHP2_EXTRACTION_MANIFEST.json";
constexpr const char* PalIndexSha = "9e26f340e1fbd11407fbafe848f1077a6d27b6301b23288f28634c548b7d8f8c";
static_assert(sizeof(std::streamoff) >= 8, "The archive reader needs 64-bit offsets");

struct Failure final : std::runtime_error { using std::runtime_error::runtime_error; };
void Require(bool condition, const std::string& message) { if (!condition) throw Failure(message); }
std::string Display(const fs::path& path) { return path.u8string(); }

std::string Upper(std::string value)
{
    for (auto& ch : value) if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 'a' + 'A');
    return value;
}
std::string Hex32(std::uint32_t value)
{
    std::ostringstream text; text << std::uppercase << std::hex << std::setw(8) << std::setfill('0') << value;
    return text.str();
}
std::string RowNumber(std::size_t value)
{
    std::ostringstream text; text << std::setw(6) << std::setfill('0') << value; return text.str();
}
std::string Json(const std::string& text)
{
    std::string escaped="\"";
    static constexpr char digits[]="0123456789abcdef";
    for (const unsigned char ch : text) {
        switch (ch) {
        case '"': escaped+="\\\""; break;
        case '\\': escaped+="\\\\"; break;
        case '\n': escaped+="\\n"; break;
        case '\r': escaped+="\\r"; break;
        case '\t': escaped+="\\t"; break;
        default:
            if (ch < 32) { escaped+="\\u00"; escaped+=digits[ch >> 4]; escaped+=digits[ch & 15]; }
            else escaped+=static_cast<char>(ch);
        }
    }
    return escaped+'"';
}
bool IsLink(const fs::path& path)
{
    const auto status=fs::symlink_status(path);
    if (fs::is_symlink(status)) return true;
#ifdef _WIN32
    const DWORD attributes=GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) return true;
#endif
    return false;
}
fs::path AbsoluteChecked(const fs::path& path)
{
    const auto absolute=fs::absolute(path).lexically_normal();
    fs::path part=absolute.root_path();
    for (const auto& component : absolute.relative_path()) {
        part/=component;
        Require(!IsLink(part),"Symbolic links/junctions are not accepted: "+Display(part));
    }
    return absolute;
}
void RequireFile(const fs::path& path)
{
    (void)AbsoluteChecked(path);
    Require(fs::is_regular_file(path),"File not found or not regular: "+Display(path));
}
std::uint64_t FileSize(const fs::path& path)
{
    RequireFile(path);return static_cast<std::uint64_t>(fs::file_size(path));
}
std::string FileHash(const fs::path& path)
{
    RequireFile(path);
    std::ifstream stream(path,std::ios::binary);
    Require(stream.is_open(),"Could not open file: "+Display(path));
    Sha256 digest;
    std::vector<char> buffer(BufferSize);
    while (stream) {
        stream.read(buffer.data(),static_cast<std::streamsize>(buffer.size()));
        const auto read=stream.gcount();
        if (read>0) digest.Update(buffer.data(),static_cast<std::size_t>(read));
    }
    Require(stream.eof() && !stream.bad(),"Read failed: "+Display(path));
    return digest.Hex();
}
std::uint32_t NameHash(const std::string& path)
{
    std::uint32_t hash=0xffffffffu;
    for (unsigned char ch : path) {
        if (ch == '/') ch='\\';
        if (ch>='a' && ch<='z') ch=static_cast<unsigned char>(ch-'a'+'A');
        Require(ch<128,"Archive dictionary paths must be ASCII: "+path);
        hash=hash*33u+ch;
    }
    return hash;
}
std::string SafeName(std::string name)
{
    std::replace(name.begin(),name.end(),'\\','/');
    name=Upper(name);
    Require(!name.empty(),"Empty archive path in dictionary");
    std::size_t begin=0;
    for (;;) {
        const auto slash=name.find('/',begin);
        const auto component=name.substr(begin,slash==std::string::npos?slash:slash-begin);
        Require(!component.empty() && component!="." && component!=".." && component.back()!='.' && component.back()!=' ',
                "Unsafe relative archive path: "+name);
        for (unsigned char ch : component)
            Require(ch>=32 && ch<127 && std::string("<>:\"|?*").find(static_cast<char>(ch))==std::string::npos,
                    "Unsafe Windows archive path: "+name);
        const auto stem=component.substr(0,component.find('.'));
        const bool port=(stem.size()==4 && (stem.compare(0,3,"COM")==0 || stem.compare(0,3,"LPT")==0)
                         && stem[3]>='1' && stem[3]<='9');
        Require(stem!="CON" && stem!="PRN" && stem!="AUX" && stem!="NUL" && !port,
                "Reserved Windows archive path: "+name);
        if (slash==std::string::npos) break;
        begin=slash+1;
    }
    Require(name!=ManifestName && name!="NFSHP2_EXTRACTION_MANIFEST.JSON" &&
            name.compare(0,10,"__UNKNOWN/")!=0 && name.compare(0,13,"__DUPLICATES/")!=0 &&
            name.compare(0,10,"__ARCHIVE/")!=0 && name!="__ARCHIVE" &&
            name.compare(0,8,".__WORK/")!=0 && name!=".__WORK",
            "Archive path conflicts with utility metadata: "+name);
    return name;
}

struct ResourceNames {
    std::string original;
    std::vector<std::string> aliases;
};
using Names=std::map<std::uint32_t,ResourceNames>;
void InsertName(Names& names,const std::string& candidate,bool original)
{
    const auto name=SafeName(candidate);
    const auto hash=NameHash(name);
    auto& entry=names[hash];
    if (original) {
        Require(entry.original.empty() || entry.original==name,"Dictionary hash collision: "+Hex32(hash)+" -> "+name);
        entry.original=name;
    } else if (entry.original!=name && std::find(entry.aliases.begin(),entry.aliases.end(),name)==entry.aliases.end()) {
        entry.aliases.push_back(name);
    }
}
std::string JsonStrings(const std::vector<std::string>& values)
{
    std::string result="[";
    for (std::size_t i=0;i<values.size();++i)result+=(i?",":"")+Json(values[i]);
    return result+"]";
}
Names LoadNames(const fs::path& extra)
{
    static constexpr const char* knownNames[]={
#include "KnownNames.inc"
    };
    Names names;
    for (const auto* name : knownNames) InsertName(names,name,true);
    if (!extra.empty()) {
        RequireFile(extra);
        std::ifstream stream(extra,std::ios::binary);Require(stream.is_open(),"Could not open name dictionary");
        std::string line;bool first=true;
        while (std::getline(stream,line)) {
            if (first && line.compare(0,3,"\xef\xbb\xbf")==0) line.erase(0,3);
            first=false;
            if (!line.empty() && line.back()=='\r') line.pop_back();
            if (!line.empty() && line.front()!='#') InsertName(names,line,false);
        }
        Require(stream.eof() && !stream.bad(),"Name dictionary read failed");
    }
    return names;
}

fs::path FindChild(const fs::path& folder,const std::string& name)
{
    if (!fs::is_directory(folder)) return {};
    fs::path found;
    const auto upper=Upper(name);
    for (const auto& item : fs::directory_iterator(folder)) {
        if (Upper(item.path().filename().u8string())!=upper) continue;
        Require(found.empty(),"Case-insensitive duplicate filename: "+Display(folder/name));
        Require(!IsLink(item.path()),"Symbolic link/junction rejected: "+Display(item.path()));
        found=item.path();
    }
    return found;
}
fs::path FindRelative(const fs::path& root,const std::string& relative)
{
    fs::path path=root;
    std::size_t begin=0;
    for (;;) {
        const auto slash=relative.find('/',begin);
        const auto part=relative.substr(begin,slash==std::string::npos?slash:slash-begin);
        path=FindChild(path,part);
        if (path.empty() || slash==std::string::npos) return path;
        begin=slash+1;
    }
}

struct Options {
    fs::path input,index,data,output,verify,names,report,hash,inventory;
    fs::path rawOutput;
    std::string lookupPath;
    std::optional<std::uint32_t> lookupId;
    std::optional<std::size_t> record;
    bool list{},help{},interactive{};
};
template<class Integer>
Integer ParseNumber(const std::string& text,int base,const char* description)
{
    Integer value{};
    const auto parsed=std::from_chars(text.data(),text.data()+text.size(),value,base);
    Require(!text.empty() && parsed.ec==std::errc{} && parsed.ptr==text.data()+text.size(),description);
    return value;
}
Options Parse(const std::vector<std::string>& arguments)
{
    Options options;options.interactive=arguments.empty();
    std::set<std::string> seen;
    for (std::size_t i=0;i<arguments.size();++i) {
        const auto& argument=arguments[i];
        if (argument=="--help" || argument=="-h") {options.help=true;continue;}
        if (argument=="--list") {options.list=true;continue;}
        if (argument.size()>2 && argument.substr(0,2)=="--") {
            Require(seen.insert(argument).second,"Repeated option: "+argument);
            Require(i+1<arguments.size(),"Missing value for "+argument);
            const auto& text=arguments[++i];
            const auto value=fs::u8path(text);
            if (argument=="--input")options.input=value;
            else if (argument=="--zdir")options.index=value;
            else if (argument=="--zzdata")options.data=value;
            else if (argument=="--output")options.output=value;
            else if (argument=="--verify")options.verify=value;
            else if (argument=="--names")options.names=value;
            else if (argument=="--report")options.report=value;
            else if (argument=="--hash")options.hash=value;
            else if (argument=="--inventory")options.inventory=value;
            else if (argument=="--lookup-path") {
                Require(!text.empty(),"--lookup-path requires a nonempty logical path");
                options.lookupPath=text;
            }
            else if (argument=="--lookup-id") {
                Require(text.size()==8,"--lookup-id requires exactly eight hexadecimal digits");
                options.lookupId=ParseNumber<std::uint32_t>(text,16,"Invalid archive ID");
            }
            else if (argument=="--record")options.record=ParseNumber<std::size_t>(text,10,"Invalid archive record number");
            else if (argument=="--raw-output")options.rawOutput=value;
            else throw Failure("Unknown option: "+argument);
        } else {
            Require(options.input.empty(),"Unexpected argument: "+argument);
            options.input=fs::u8path(argument);
        }
    }
    Require(options.verify.empty() || options.output.empty(),"--verify and --output cannot be combined");
    Require(options.report.empty() || !options.verify.empty(),"--report requires --verify");
    Require(options.inventory.empty() || (options.verify.empty() && options.output.empty() && !options.list && options.hash.empty()),
            "--inventory cannot be combined with --verify, --output, --list or --hash");
    const bool lookup=!options.lookupPath.empty() || options.lookupId.has_value();
    Require(options.lookupPath.empty() || !options.lookupId,"--lookup-path and --lookup-id cannot be combined");
    Require(!lookup || (options.inventory.empty() && options.verify.empty() && options.output.empty() &&
                        !options.list && options.hash.empty()),
            "Resource lookup cannot be combined with extraction, verification, inventory, list or file hash modes");
    Require(lookup || (options.rawOutput.empty() && !options.record),"--raw-output and --record require resource lookup");
    return options;
}

fs::path ExecutableDirectory(const std::string& firstArgument)
{
#ifdef _WIN32
    std::vector<wchar_t> buffer(32768);
    const DWORD count=GetModuleFileNameW(nullptr,buffer.data(),static_cast<DWORD>(buffer.size()));
    Require(count!=0 && count<buffer.size(),"Cannot obtain the utility executable path");
    (void)firstArgument;
    return AbsoluteChecked(fs::path(buffer.data(),buffer.data()+count).parent_path());
#elif defined(__linux__)
    std::vector<char> buffer(65536);
    const auto count=readlink("/proc/self/exe",buffer.data(),buffer.size());
    if (count>0 && static_cast<std::size_t>(count)<buffer.size())
        return AbsoluteChecked(fs::u8path(std::string(buffer.data(),static_cast<std::size_t>(count))).parent_path());
    return AbsoluteChecked(fs::u8path(firstArgument).parent_path());
#else
    return AbsoluteChecked(fs::u8path(firstArgument).parent_path());
#endif
}
#ifdef _WIN32
fs::path ChooseArchive()
{
    std::array<wchar_t,32768> filename{};
    OPENFILENAMEW dialog{};dialog.lStructSize=sizeof(dialog);dialog.lpstrFile=filename.data();
    dialog.nMaxFile=static_cast<DWORD>(filename.size());
    dialog.lpstrFilter=L"NFS HP2 archive (ZZDATA.BIN)\0ZZDATA.BIN\0Binary files\0*.BIN\0All files\0*.*\0";
    dialog.lpstrTitle=L"Select the original ZZDATA.BIN (ZDIR.BIN must be beside it)";
    dialog.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&dialog)) {
        const auto error=CommDlgExtendedError();
        if (error)throw Failure("Archive selection failed, Windows error "+std::to_string(error));
        throw Failure("No archive selected; no files were changed");
    }
    return fs::path(filename.data());
}
#endif

void ResolveInput(Options& options,const fs::path& executable)
{
    if (!options.input.empty()) {
        Require(options.data.empty(),"Use either --input or --zzdata");
        auto input=AbsoluteChecked(options.input);
        if (fs::is_directory(input)) {
            options.data=FindChild(input,"ZZDATA.BIN");
            if (options.index.empty())options.index=FindChild(input,"ZDIR.BIN");
        } else if (Upper(input.filename().u8string())=="ZDIR.BIN") {
            Require(options.index.empty(),"Index specified twice");options.index=input;
            options.data=FindChild(input.parent_path(),"ZZDATA.BIN");
        } else options.data=input;
    }
    if (options.data.empty() && !options.index.empty())
        options.data=FindChild(AbsoluteChecked(options.index).parent_path(),"ZZDATA.BIN");
    if (options.data.empty() && options.input.empty() && options.index.empty()) {
        const auto current=AbsoluteChecked(fs::current_path());
        const std::array<fs::path,6> roots{{executable,current,executable/"NFSHP2",current/"NFSHP2",
                                         executable.parent_path(),current.parent_path()}};
        for (const auto& root : roots) {
            const auto data=FindChild(root,"ZZDATA.BIN");
            const auto index=FindChild(root,"ZDIR.BIN");
            if (!data.empty() && !index.empty()) {options.data=data;options.index=index;break;}
        }
#ifdef _WIN32
        if (options.data.empty() && options.interactive) options.data=ChooseArchive();
#endif
    }
    Require(!options.data.empty(),"ZZDATA.BIN was not found. Put this EXE beside ZDIR.BIN/ZZDATA.BIN or use --input <ZZDATA.BIN>.");
    options.data=AbsoluteChecked(options.data);RequireFile(options.data);
    if (options.index.empty())options.index=FindChild(options.data.parent_path(),"ZDIR.BIN");
    Require(!options.index.empty(),"ZDIR.BIN was not found beside "+Display(options.data)+". The index is required; resources cannot be split by guessed offsets.");
    options.index=AbsoluteChecked(options.index);RequireFile(options.index);
    Require(options.index!=options.data,"ZDIR and ZZDATA must be different files");
}

struct Entry {
    std::size_t index{};
    std::uint32_t hash{},sector{},size{};
    std::uint64_t offset{};
    std::string name,output,sha,status;
    std::optional<std::size_t> duplicateOf;
    std::vector<std::string> aliases,aliasStatus;
    bool named{};
};
struct Archive {
    fs::path indexPath,dataPath;
    std::vector<Entry> entries;
    std::string indexSha;
    std::uint64_t dataSize{};
    fs::file_time_type dataTime;
    std::vector<std::uint8_t> indexBytes;
    std::set<std::uint32_t> ambiguousHashes;
    struct Sidecar {std::string path,expectedSha,actualSha,status;std::uint64_t size{};};
    std::vector<Sidecar> sidecars;
};
std::uint32_t U32(const std::uint8_t* value)
{
    return std::uint32_t{value[0]}|(std::uint32_t{value[1]}<<8)|(std::uint32_t{value[2]}<<16)|(std::uint32_t{value[3]}<<24);
}
Archive ReadArchive(const Options& options,const Names& names)
{
    Archive archive;archive.indexPath=options.index;archive.dataPath=options.data;
    const auto length=FileSize(options.index);
    Require(length!=0 && length<=32u*1024u*1024u && length%12==0,"ZDIR is not a complete HP2 old-format index (12 bytes per record): "+Display(options.index));
    archive.dataSize=FileSize(options.data);archive.dataTime=fs::last_write_time(options.data);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
    std::ifstream index(options.index,std::ios::binary);Require(index.is_open(),"Cannot open ZDIR");
    index.read(reinterpret_cast<char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));
    Require(static_cast<std::size_t>(index.gcount())==bytes.size(),"ZDIR was truncated while reading");
    Sha256 hash;hash.Update(bytes.data(),bytes.size());archive.indexSha=hash.Hex();
    archive.entries.reserve(bytes.size()/12);
    for (std::size_t i=0;i<bytes.size();i+=12) {
        Entry entry;entry.index=i/12;entry.hash=U32(bytes.data()+i);entry.sector=U32(bytes.data()+i+4);
        entry.size=U32(bytes.data()+i+8);entry.offset=std::uint64_t{entry.sector}*2048u;
        Require(entry.offset<=archive.dataSize && entry.size<=archive.dataSize-entry.offset,
                "Truncated/wrong ZZDATA: index entry "+std::to_string(entry.index)+" hash "+Hex32(entry.hash)+
                " needs bytes ["+std::to_string(entry.offset)+", "+std::to_string(entry.offset+entry.size)+
                "), archive has "+std::to_string(archive.dataSize)+" bytes. No output was created.");
        const auto found=names.find(entry.hash);
        entry.named=found!=names.end() && !found->second.original.empty();
        entry.name=entry.named?found->second.original:"__Unknown/"+Hex32(entry.hash)+".bin";
        if (found!=names.end())entry.aliases=found->second.aliases;
        archive.entries.push_back(std::move(entry));
    }
    archive.indexBytes=std::move(bytes);
    return archive;
}
void ConfirmUnchanged(const Archive& archive)
{
    Require(FileSize(archive.dataPath)==archive.dataSize && fs::last_write_time(archive.dataPath)==archive.dataTime,
            "ZZDATA changed while it was being read; output was not committed");
    Require(FileHash(archive.indexPath)==archive.indexSha,"ZDIR changed while it was being read; output was not committed");
}

std::string ReadExtent(std::ifstream& input,const Entry& entry,std::ofstream* output)
{
    input.clear();input.seekg(static_cast<std::streamoff>(entry.offset));
    Require(static_cast<bool>(input),"Seek failed at index entry "+std::to_string(entry.index));
    std::uint64_t left=entry.size;Sha256 digest;
    std::vector<char> buffer(BufferSize);
    while (left!=0) {
        const auto take=static_cast<std::size_t>(std::min<std::uint64_t>(left,buffer.size()));
        input.read(buffer.data(),static_cast<std::streamsize>(take));
        Require(static_cast<std::size_t>(input.gcount())==take,
                "Unexpected EOF/read failure at index entry "+std::to_string(entry.index)+"; nonempty data is never replaced by an empty file");
        digest.Update(buffer.data(),take);
        if (output) {output->write(buffer.data(),static_cast<std::streamsize>(take));Require(static_cast<bool>(*output),"Output write failed (disk full or permissions)");}
        left-=take;
    }
    return digest.Hex();
}
void RenameNoReplace(const fs::path& source,const fs::path& destination)
{
    Require(!fs::exists(destination) && !IsLink(destination),"Destination already exists: "+Display(destination));
#ifdef _WIN32
    const BOOL moved=MoveFileExW(source.c_str(),destination.c_str(),MOVEFILE_WRITE_THROUGH);
    const DWORD error=moved?ERROR_SUCCESS:GetLastError();
    Require(moved!=FALSE,"Could not publish output without replacing existing data, Windows error "+std::to_string(error));
#elif defined(__linux__)
    const long result=syscall(SYS_renameat2,AT_FDCWD,source.c_str(),AT_FDCWD,destination.c_str(),1u);
    Require(result==0,"Could not publish output without replacing existing data: "+std::string(std::strerror(errno)));
#else
    throw Failure("Atomic no-replace publication is supported on Windows and Linux only");
#endif
}
std::string UnresolvedPayloadKind(std::ifstream& input,const Entry& entry)
{
    if (!entry.size)return "empty record";
    std::array<std::uint8_t,4096> bytes{};
    const auto count=std::min<std::size_t>(bytes.size(),entry.size);
    input.clear();input.seekg(static_cast<std::streamoff>(entry.offset));
    Require(static_cast<bool>(input),"Cannot seek while inspecting unresolved record");
    input.read(reinterpret_cast<char*>(bytes.data()),static_cast<std::streamsize>(count));
    Require(static_cast<std::size_t>(input.gcount())==count,"Unresolved record header was truncated");
    if (count>=12 && std::memcmp(bytes.data(),"RIFF",4)==0 && std::memcmp(bytes.data()+8,"AVI ",4)==0)
        return "RIFF/AVI movie";
    if (count>=12 && U32(bytes.data())==0 && U32(bytes.data()+4)==0 && U32(bytes.data()+8)==0x39100u)
        return "replay snapshot stream";
    if (count>=16 && U32(bytes.data())==0x14200u && U32(bytes.data()+4)==4)
        return "chunked audio data";
    if (count>=28 && U32(bytes.data())==1000 && U32(bytes.data()+4)>0 && U32(bytes.data()+4)<100000)
        return "binary event table";
    const auto text=std::all_of(bytes.begin(),bytes.begin()+static_cast<std::ptrdiff_t>(count),
        [](std::uint8_t value){return (value>=32 && value<127) || value==9 || value==10 || value==13;});
    return text ? "ASCII text":"unclassified binary";
}
void WriteNameInventory(const Archive& archive,const Names& names,const Options& options)
{
    const auto output=AbsoluteChecked(options.inventory);
    Require(fs::is_directory(output.parent_path()) && !fs::exists(output),
            "Inventory must be a NEW file in an existing directory");
    std::ifstream input(archive.dataPath,std::ios::binary);Require(input.is_open(),"Cannot open ZZDATA");
    std::set<std::uint32_t> hashes,namedHashes,unknownHashes;
    std::map<std::string,std::size_t> kindCounts;
    std::size_t named=0,unknown=0;
    std::size_t originalPaths=0,aliasPaths=0;
    for (const auto& name:names) {
        if (!name.second.original.empty())++originalPaths;
        aliasPaths+=name.second.aliases.size();
    }
    std::vector<std::string> kinds(archive.entries.size());
    for (const auto& entry:archive.entries) {
        hashes.insert(entry.hash);
        if (entry.named) {++named;namedHashes.insert(entry.hash);}
        else {
            ++unknown;unknownHashes.insert(entry.hash);
            kinds[entry.index]=UnresolvedPayloadKind(input,entry);
            ++kindCounts[kinds[entry.index]];
        }
    }
    const auto temporary=output.parent_path()/
        (".__nfshp2-names-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".tmp");
    Require(!fs::exists(temporary) && !IsLink(temporary),"Temporary inventory already exists");
    try {
        std::ofstream stream(temporary,std::ios::binary);Require(stream.is_open(),"Cannot create naming inventory");
        stream<<"{\n  \"schema\": \"NFSHP2_ARCHIVE_NAME_INVENTORY/v2\",\n"
              <<"  \"status\": "<<Json(unknown?"INDEX_VALID_NAMES_PARTIAL":"INDEX_VALID_DICTIONARY_COVERAGE_COMPLETE")
              <<",\n  \"index\": "<<Json(Display(archive.indexPath))
              <<",\n  \"index_sha256\": "<<Json(archive.indexSha)
              <<",\n  \"archive\": "<<Json(Display(archive.dataPath))
              <<",\n  \"archive_bytes\": "<<archive.dataSize
              <<",\n  \"index_record_bytes\": 12,\n  \"index_fields\": [\"name_hash\",\"sector\",\"size\"],\n"
              <<"  \"index_stores_original_path_strings\": false,\n"
              <<"  \"name_scope\": \"Original-name coverage uses only the retained built-in dictionary. User aliases never establish original names or replace canonical ID paths. ZDIR stores no pathname strings.\",\n"
              <<"  \"extra_name_list\": "<<Json(Display(options.names))
              <<",\n  \"dictionary_paths\": "<<originalPaths+aliasPaths
              <<",\n  \"dictionary_hashes\": "<<names.size()
              <<",\n  \"original_dictionary_paths\": "<<originalPaths
              <<",\n  \"external_alias_paths\": "<<aliasPaths
              <<",\n  \"records\": "<<archive.entries.size()
              <<",\n  \"unique_archive_hashes\": "<<hashes.size()
              <<",\n  \"named_records\": "<<named
              <<",\n  \"named_unique_hashes\": "<<namedHashes.size()
              <<",\n  \"unresolved_records\": "<<unknown
              <<",\n  \"unresolved_unique_hashes\": "<<unknownHashes.size()
              <<",\n  \"all_records_have_dictionary_names\": "<<(unknown?"false":"true")
              <<",\n  \"payload_verification_performed\": false,\n"
              <<"  \"default_paths_require_payload_checks_for_differing_duplicates\": true,\n"
              <<"  \"unresolved_kind_scope\": \"Hints inspect at most4096 original bytes. They identify data signatures, not filenames or extraction extensions.\",\n"
              <<"  \"unresolved_payload_kinds\": {";
        std::size_t kindIndex=0;
        for (const auto& kind:kindCounts)
            stream<<(kindIndex++?",":"")<<Json(kind.first)<<':'<<kind.second;
        stream<<"},\n  \"entries\": [\n";
        for (std::size_t index=0;index<archive.entries.size();++index) {
            const auto& entry=archive.entries[index];
            stream<<"    {\"index\":"<<entry.index<<",\"hash\":"<<Json(Hex32(entry.hash))
                  <<",\"sector\":"<<entry.sector<<",\"offset\":"<<entry.offset<<",\"size\":"<<entry.size
                  <<",\"name_resolved\":"<<(entry.named?"true":"false")
                  <<",\"dictionary_path\":"<<(entry.named?Json(entry.name):"null")
                  <<",\"external_aliases\":"<<JsonStrings(entry.aliases)
                  <<",\"external_aliases_are_original_name_evidence\":false"
                  <<",\"default_extraction_path\":"<<Json(entry.name)
                  <<",\"unresolved_payload_kind\":"<<(entry.named?"null":Json(kinds[entry.index]))
                  <<'}'<<(index+1<archive.entries.size()?",":"")<<'\n';
        }
        stream<<"  ]\n}\n";stream.flush();Require(static_cast<bool>(stream),"Naming inventory write failed");
        stream.close();ConfirmUnchanged(archive);RenameNoReplace(temporary,output);
    } catch (...) {std::error_code error;fs::remove(temporary,error);throw;}
    std::cout<<"Naming coverage: "<<named<<'/'<<archive.entries.size()<<" records, "<<unknown
             <<" unresolved ("<<unknownHashes.size()<<" unique hashes). Inventory: "<<Display(output)
             <<"\nPayloads were not extracted or verified.\n";
}
fs::path NewOutput(const Options& options)
{
    if (!options.output.empty()) {
        const auto output=AbsoluteChecked(options.output);
        Require(!fs::exists(output),"Output directory already exists. Choose a NEW directory; existing files are not overwritten: "+Display(output));
        Require(fs::is_directory(output.parent_path()),"Output parent directory does not exist: "+Display(output.parent_path()));
        return output;
    }
    for (unsigned i=0;i<10000;++i) {
        const auto name=i==0?std::string("@ZZDATA"):i==1?std::string("@ZZDATA_verified"):"@ZZDATA_verified_"+std::to_string(i);
        const auto output=AbsoluteChecked(options.data.parent_path()/name);
        if (!fs::exists(output))return output;
    }
    throw Failure("Could not choose a new output folder; use --output");
}
struct Stage final {
    fs::path path;
    bool committed{};
    explicit Stage(const fs::path& output)
    {
        const auto now=std::chrono::steady_clock::now().time_since_epoch().count();
        for (unsigned i=0;i<10000;++i) {
            auto candidate=output.parent_path()/fs::u8path(output.filename().u8string()+".partial-"+std::to_string(now)+"-"+std::to_string(i));
            if (fs::create_directory(candidate)) {path=std::move(candidate);return;}
        }
        throw Failure("Could not reserve a temporary extraction directory");
    }
    ~Stage() { if (!committed) {std::error_code error;fs::remove_all(path,error);} }
};

std::vector<std::uint8_t> AmbiguityBytes(const Archive& archive)
{
    std::vector<std::uint8_t> bytes;
    for (const auto hash : archive.ambiguousHashes)
        for (unsigned shift=0;shift<32;shift+=8)bytes.push_back(static_cast<std::uint8_t>(hash>>shift));
    return bytes;
}
std::string HashBytes(const std::vector<std::uint8_t>& bytes)
{
    Sha256 hash;if (!bytes.empty())hash.Update(bytes.data(),bytes.size());return hash.Hex();
}
void WriteSidecar(Archive& archive,const fs::path& root,const std::string& name,const std::vector<std::uint8_t>& bytes)
{
    const auto path=root/fs::u8path(name);fs::create_directories(path.parent_path());
    Require(!fs::exists(path),"Sidecar path already exists: "+Display(path));
    std::ofstream output(path,std::ios::binary);Require(output.is_open(),"Cannot create archive sidecar");
    if (!bytes.empty())output.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));
    output.flush();Require(static_cast<bool>(output),"Archive sidecar write failed");output.close();
    const auto expected=HashBytes(bytes),actual=FileHash(path);
    Require(expected==actual && FileSize(path)==bytes.size(),"Archive sidecar verification failed");
    archive.sidecars.push_back({name,expected,actual,"VERIFIED",static_cast<std::uint64_t>(bytes.size())});
}
std::size_t VerifySidecars(Archive& archive,const fs::path& root)
{
    const auto index=FindRelative(root,"__archive/ZDIR.BIN");
    const auto marker=FindRelative(root,"__archive/AMBIGUOUS_HASHES.BIN");
    if (index.empty() && marker.empty() && FindChild(root,ManifestName).empty()) {
        archive.sidecars.push_back({"__archive/ZDIR.BIN",archive.indexSha,"","ABSENT_LEGACY_EXTRACTION",
                                   static_cast<std::uint64_t>(archive.indexBytes.size())});
        return 0;
    }
    std::size_t failures=0;
    auto compare=[&](const fs::path& path,const std::string& name,const std::vector<std::uint8_t>& bytes,bool required) {
        const auto expected=HashBytes(bytes);
        const auto actual=path.empty()?std::string{}:FileHash(path);
        const bool valid=path.empty()?!required:(actual==expected && FileSize(path)==bytes.size());
        archive.sidecars.push_back({name,expected,actual,valid?"VERIFIED":"MISMATCH",static_cast<std::uint64_t>(bytes.size())});
        if (!valid) {++failures;std::cerr<<"SIDECAR_MISMATCH: "<<name<<'\n';}
    };
    compare(index,"__archive/ZDIR.BIN",archive.indexBytes,true);
    if (!marker.empty() || !archive.ambiguousHashes.empty())
        compare(marker,"__archive/AMBIGUOUS_HASHES.BIN",AmbiguityBytes(archive),!archive.ambiguousHashes.empty());
    return failures;
}

void WriteManifest(const fs::path& file,const Archive& archive,const std::string& operation,
                   std::size_t failures=0,const fs::path& verifiedRoot={})
{
    Require(!fs::exists(file),"Manifest/report already exists: "+Display(file));
    std::ofstream stream(file,std::ios::binary);Require(stream.is_open(),"Cannot create manifest/report");
    const auto zero=std::count_if(archive.entries.begin(),archive.entries.end(),[](const Entry& e){return e.size==0;});
    const auto unknown=std::count_if(archive.entries.begin(),archive.entries.end(),[](const Entry& e){return !e.named;});
    stream<<"{\n  \"schema\": \"NFSHP2_NATIVE_ARCHIVE/v2\",\n  \"operation\": "<<Json(operation)
          <<",\n  \"status\": "<<Json(failures?"FAIL_PAYLOAD_VERIFICATION":"PASS")
          <<",\n  \"index\": "<<Json(Display(archive.indexPath))
          <<",\n  \"index_sha256\": "<<Json(archive.indexSha)
          <<",\n  \"archive\": "<<Json(Display(archive.dataPath))
          <<",\n  \"archive_bytes\": "<<archive.dataSize
          <<",\n  \"entries\": "<<archive.entries.size()
          <<",\n  \"authored_zero_length_entries\": "<<zero
          <<",\n  \"unresolved_name_entries\": "<<unknown
          <<",\n  \"verification_failures\": "<<failures
          <<",\n  \"verified_directory\": "<<Json(Display(verifiedRoot))
          <<",\n  \"LZC_COMP_decompressed\": false,\n  \"nonempty_payload_replaced_with_empty\": false,\n"
          <<"  \"ambiguous_hash_count\": "<<archive.ambiguousHashes.size()
          <<",\n  \"duplicate_identity_scope\": \"Same-hash extents were compared by actual SHA256. Differing bytes are preserved and marked unusable as one unique runtime resource.\",\n"
          <<"  \"external_alias_scope\": \"User-supplied names are optional additional copies, not original-name evidence. Canonical payload paths are retained. Differing same-ID payloads require explicit record selection and receive no aliases. Verify aliases by supplying the same --names list.\",\n"
          <<"  \"runtime_sidecars\": [\n";
    for (std::size_t i=0;i<archive.sidecars.size();++i) {
        const auto& sidecar=archive.sidecars[i];
        stream<<"    {\"path\":"<<Json(sidecar.path)<<",\"size\":"<<sidecar.size
              <<",\"expected_sha256\":"<<Json(sidecar.expectedSha)<<",\"actual_sha256\":"<<Json(sidecar.actualSha)
              <<",\"status\":"<<Json(sidecar.status)<<'}'<<(i+1<archive.sidecars.size()?",":"")<<'\n';
    }
    stream<<"  ],\n  \"records\": [\n";
    for (std::size_t i=0;i<archive.entries.size();++i) {
        const auto& e=archive.entries[i];
        stream<<"    {\"index\":"<<e.index<<",\"index_offset\":"<<e.index*12
              <<",\"hash\":"<<Json(Hex32(e.hash))<<",\"sector\":"<<e.sector<<",\"offset\":"<<e.offset
              <<",\"size\":"<<e.size<<",\"original_name_known\":"<<(e.named?"true":"false")
              <<",\"authored_empty\":"<<(e.size==0?"true":"false")<<",\"name\":"<<Json(e.name)
              <<",\"output\":"<<Json(e.output)<<",\"sha256\":"<<Json(e.sha)<<",\"status\":"<<Json(e.status)
              <<",\"external_aliases\":"<<JsonStrings(e.aliases)
              <<",\"external_alias_status\":"<<JsonStrings(e.aliasStatus)
              <<",\"duplicate_of\":";
        if (e.duplicateOf)stream<<*e.duplicateOf;else stream<<"null";
        stream<<'}'<<(i+1<archive.entries.size()?",":"")<<'\n';
    }
    stream<<"  ]\n}\n";stream.flush();Require(static_cast<bool>(stream),"Manifest/report write failed");
}
void Progress(std::size_t done,std::size_t total)
{
    if (done==total || done%32==0)std::cout<<"Checked "<<done<<" / "<<total<<" indexed resources\n"<<std::flush;
}

void LookupResource(Archive& archive,const Options& options)
{
    // This is the original OpenCDFile address boundary: normalized pathname
    // bytes produce one 32-bit key. The archive never supplies a reverse name.
    // Direct ID/record selection is an explicit offline PC tool operation.
    if (!options.lookupPath.empty())
        for (unsigned char ch : options.lookupPath)
            Require(ch>=32 && ch<127,"Lookup paths must contain printable ASCII bytes");
    const auto id=options.lookupId?*options.lookupId:NameHash(options.lookupPath);
    std::vector<Entry*> matches;
    for (auto& entry : archive.entries)
        if (entry.hash==id && (!options.record || entry.index==*options.record))matches.push_back(&entry);
    Require(!matches.empty(),"Archive ID/record not found: "+Hex32(id));
    std::ifstream input(archive.dataPath,std::ios::binary);Require(input.is_open(),"Cannot open ZZDATA");
    bool identical=true;
    for (auto* entry : matches) {
        entry->sha=ReadExtent(input,*entry,nullptr);
        identical=identical && entry->size==matches.front()->size && entry->sha==matches.front()->sha;
    }
    ConfirmUnchanged(archive);
    if (!options.rawOutput.empty()) {
        Require(identical,"Different payloads share this ID; select the exact --record before raw extraction");
        const auto output=AbsoluteChecked(options.rawOutput);
        Require(fs::is_directory(output.parent_path()) && !fs::exists(output),
                "Raw output must be a NEW file in an existing directory");
        Stage stage(output);const auto temporary=stage.path/"payload";
        const auto& entry=*matches.front();
        std::ofstream file(temporary,std::ios::binary);Require(file.is_open(),"Cannot create temporary raw output");
        const auto actual=ReadExtent(input,entry,&file);
        file.flush();Require(static_cast<bool>(file),"Could not flush raw output");
        file.close();Require(static_cast<bool>(file),"Could not close raw output");
        Require(actual==entry.sha && FileSize(temporary)==entry.size && FileHash(temporary)==entry.sha,
                "Raw payload verification failed");
        ConfirmUnchanged(archive);
        RenameNoReplace(temporary,output);
    }
    std::cout<<"{\n  \"schema\": \"NFSHP2_RESOURCE_LOOKUP/v1\",\n  \"id\": "<<Json(Hex32(id))
             <<",\n  \"index_sha256\": "<<Json(archive.indexSha)
             <<",\n  \"requested_path\": "<<(options.lookupPath.empty()?"null":Json(options.lookupPath))
             <<",\n  \"query_is_original_name_evidence\": false"
             <<",\n  \"raw_output\": "<<(options.rawOutput.empty()?"null":Json(Display(AbsoluteChecked(options.rawOutput))))
             <<",\n  \"payloads_identical\": "<<(identical?"true":"false")<<",\n  \"records\": [\n";
    for (std::size_t i=0;i<matches.size();++i) {
        const auto& entry=*matches[i];
        std::cout<<"    {\"index\":"<<entry.index<<",\"sector\":"<<entry.sector<<",\"offset\":"<<entry.offset
                 <<",\"size\":"<<entry.size<<",\"dictionary_path\":"<<(entry.named?Json(entry.name):"null")
                 <<",\"external_aliases\":"<<JsonStrings(entry.aliases)
                 <<",\"preserved_path\":"<<Json(entry.name)<<",\"sha256\":"<<Json(entry.sha)<<'}'
                 <<(i+1<matches.size()?",":"")<<'\n';
    }
    std::cout<<"  ]\n}\n";
}

std::size_t ProcessAliases(Archive& archive,const fs::path& root,bool create)
{
    // Finish duplicate comparison first: an alias cannot select a version of
    // an ambiguous ID. Canonical paths remain the runtime's address boundary.
    std::size_t failures=0;
    for (auto& entry:archive.entries) {
        entry.aliasStatus.clear();
        for (const auto& alias:entry.aliases) {
            if (archive.ambiguousHashes.count(entry.hash)!=0) {
                entry.aliasStatus.push_back("REQUIRES_EXPLICIT_RECORD");
                continue;
            }
            auto destination=FindRelative(root,alias);
            if (create && destination.empty()) {
                const auto source=root/fs::u8path(entry.output);
                destination=root/fs::u8path(alias);
                fs::create_directories(destination.parent_path());
                Require(fs::copy_file(source,destination,fs::copy_options::none),"Alias copy failed: "+Display(destination));
            }
            std::string status;
            if (destination.empty())status="MISSING_ALIAS";
            else if (!fs::is_regular_file(destination))status="ALIAS_NOT_A_REGULAR_FILE";
            else if (FileSize(destination)!=entry.size)status="ALIAS_SIZE_MISMATCH";
            else if (FileHash(destination)!=entry.sha)status="ALIAS_CONTENT_MISMATCH";
            else status="VERIFIED_ALIAS";
            entry.aliasStatus.push_back(status);
            if (status!="VERIFIED_ALIAS") {
                ++failures;
                Require(!create,status+": "+alias);
                std::cerr<<status<<": index "<<entry.index<<" "<<alias<<'\n';
            }
        }
    }
    return failures;
}

fs::path Extract(Archive& archive,const Options& options)
{
    const auto output=NewOutput(options);
    Stage stage(output);const auto temporary=stage.path/".__work";fs::create_directory(temporary);
    std::ifstream input(archive.dataPath,std::ios::binary);Require(input.is_open(),"Cannot open ZZDATA");
    std::map<std::string,std::size_t> first;
    std::cout<<"Extracting "<<archive.entries.size()<<" indexed resources to "<<Display(output)<<'\n';
    for (auto& e : archive.entries) {
        const auto raw=temporary/(RowNumber(e.index)+".part");
        std::ofstream file(raw,std::ios::binary);Require(file.is_open(),"Cannot create temporary output: "+Display(raw));
        e.sha=ReadExtent(input,e,&file);file.flush();Require(static_cast<bool>(file),"Could not flush output: "+Display(raw));file.close();
        Require(FileSize(raw)==e.size,"Written resource size differs from its ZDIR record");
        e.output=e.name;e.status=e.size==0?"AUTHORED_EMPTY":"EXTRACTED";
        const auto found=first.find(Upper(e.name));
        if (found!=first.end()) {
            const auto& prior=archive.entries[found->second];e.duplicateOf=prior.index;
            if (e.sha==prior.sha && e.size==prior.size) {
                e.output=prior.output;e.status="IDENTICAL_DUPLICATE";fs::remove(raw);
            } else {
                archive.ambiguousHashes.insert(e.hash);
                e.output="__duplicates/"+RowNumber(e.index)+"/"+e.name;e.status="DIFFERENT_DUPLICATE_PRESERVED";
                const auto destination=stage.path/fs::u8path(e.output);fs::create_directories(destination.parent_path());
                RenameNoReplace(raw,destination);
            }
        } else {
            first.emplace(Upper(e.name),e.index);
            const auto destination=stage.path/fs::u8path(e.output);fs::create_directories(destination.parent_path());
            RenameNoReplace(raw,destination);
        }
        // Read back the actual written file; successful writes alone are not verification.
        const auto written=stage.path/fs::u8path(e.output);
        Require(FileSize(written)==e.size && FileHash(written)==e.sha,"Written payload verification failed: "+Display(written));
        Progress(e.index+1,archive.entries.size());
    }
    fs::remove(temporary);
    (void)ProcessAliases(archive,stage.path,true);
    ConfirmUnchanged(archive);
    WriteSidecar(archive,stage.path,"__archive/ZDIR.BIN",archive.indexBytes);
    if (!archive.ambiguousHashes.empty())
        WriteSidecar(archive,stage.path,"__archive/AMBIGUOUS_HASHES.BIN",AmbiguityBytes(archive));
    WriteManifest(stage.path/ManifestName,archive,"extract");
    RenameNoReplace(stage.path,output);stage.committed=true;
    return output;
}

std::optional<std::string> LegacyPalName(const Archive& archive,const Entry& entry)
{
    if (archive.indexSha!=PalIndexSha)return std::nullopt;
    struct Alias {std::size_t index;std::uint32_t hash;const char* name;};
    static constexpr Alias aliases[]={
#include "LegacyPalNames.inc"
    };
    for (const auto& alias : aliases)if (alias.index==entry.index && alias.hash==entry.hash)return alias.name;
    return std::nullopt;
}
fs::path VerificationRoot(const fs::path& input)
{
    const auto root=AbsoluteChecked(input);Require(fs::is_directory(root),"Verification directory not found: "+Display(root));
    const auto name=Upper(root.filename().u8string());
    if (name=="@ZZDATA" || name=="ZZDATA")return root;
    std::vector<fs::path> wrappers;
    for (const auto* wrapper : {"@ZZDATA","ZZDATA"}) {
        const auto found=FindChild(root,wrapper);
        if (!found.empty() && fs::is_directory(found))wrappers.push_back(found);
    }
    Require(wrappers.size()<2,"Two extracted directories found; pass the intended @ZZDATA/ZZDATA directory explicitly");
    return wrappers.empty()?root:AbsoluteChecked(wrappers.front());
}
std::pair<fs::path,std::size_t> Verify(Archive& archive,const Options& options)
{
    const auto root=VerificationRoot(options.verify);
    auto report=options.report.empty()?root/"NFSHP2_VERIFY.json":AbsoluteChecked(options.report);
    if (options.report.empty()) {
        unsigned suffix=1;
        while (fs::exists(report))report=root/("NFSHP2_VERIFY_"+std::to_string(++suffix)+".json");
    }
    Require(fs::is_directory(report.parent_path()) && !fs::exists(report),"Report path must be a NEW file in an existing directory");
    (void)AbsoluteChecked(report);
    std::ifstream input(archive.dataPath,std::ios::binary);Require(input.is_open(),"Cannot open ZZDATA");
    std::size_t failures=0;
    std::map<std::string,std::size_t> first;
    for (auto& e : archive.entries) {
        e.sha=ReadExtent(input,e,nullptr);
        std::vector<std::string> candidates;
        const auto old=first.find(Upper(e.name));
        if (old!=first.end()) {
            const auto& prior=archive.entries[old->second];e.duplicateOf=prior.index;
            if (prior.sha!=e.sha || prior.size!=e.size) {
                archive.ambiguousHashes.insert(e.hash);
                candidates.push_back("__duplicates/"+RowNumber(e.index)+"/"+e.name);
            }
        } else first.emplace(Upper(e.name),e.index);
        candidates.push_back(e.name);
        if (const auto legacy=LegacyPalName(archive,e))candidates.push_back(*legacy);
        candidates.push_back("__Unknown/"+Hex32(e.hash));
        candidates.push_back("__Unknown/"+Hex32(e.hash)+".bin");
        fs::path found;
        for (const auto& candidate : candidates) {
            found=FindRelative(root,candidate);
            if (!found.empty()) {e.output=Display(fs::relative(found,root));break;}
        }
        if (found.empty())e.status="MISSING";
        else if (!fs::is_regular_file(found))e.status="NOT_A_REGULAR_FILE";
        else {
            const auto actual=FileSize(found);
            if (actual==0 && e.size!=0)e.status="UNEXPECTED_EMPTY";
            else if (actual!=e.size)e.status="SIZE_MISMATCH";
            else if (FileHash(found)!=e.sha)e.status="CONTENT_MISMATCH";
            else e.status=e.size==0?"AUTHORED_EMPTY_VERIFIED":"VERIFIED";
        }
        if (e.status!="VERIFIED" && e.status!="AUTHORED_EMPTY_VERIFIED") {
            ++failures;std::cerr<<e.status<<": index "<<e.index<<" "<<e.name<<" (expected "<<e.size<<" bytes)\n";
        }
        Progress(e.index+1,archive.entries.size());
    }
    failures+=ProcessAliases(archive,root,false);
    failures+=VerifySidecars(archive,root);
    ConfirmUnchanged(archive);
    // The report itself is published only after all reads complete.
    const auto temporary=report.parent_path()/(".__nfshp2-verify-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".tmp");
    try {WriteManifest(temporary,archive,"verify",failures,root);RenameNoReplace(temporary,report);}
    catch (...) {std::error_code error;fs::remove(temporary,error);throw;}
    return {report,failures};
}

void Help()
{
    std::cout<<"NFS HP2 Archive Utility\n"
             <<"Double-click beside ZDIR.BIN and ZZDATA.BIN to extract into a new @ZZDATA directory.\n"
             <<"NFSHP2Unpack --input <ZZDATA.BIN> [--output <NEW directory>]\n"
             <<"NFSHP2Unpack --zdir <index> --zzdata <archive> [--output <NEW directory>]\n"
             <<"NFSHP2Unpack --input <ZZDATA.BIN> --verify <extracted directory> [--report <NEW JSON>]\n"
             <<"NFSHP2Unpack --input <ZZDATA.BIN> --inventory <NEW JSON>\n"
             <<"NFSHP2Unpack --input <ZZDATA.BIN> --lookup-path <logical path>\n"
             <<"NFSHP2Unpack --input <ZZDATA.BIN> --lookup-id <8 hex digits> [--record <index>] [--raw-output <NEW file>]\n"
             <<"Options: --names <user alias list; not original-name evidence>, --list, --hash <file>, --help\n"
             <<"Existing resources are never overwritten. Unknown names are preserved by hash.\n"
             <<"Files with ZDIR size 0 are genuine empty entries; nonempty entries must copy exactly.\n"
             <<"COMP/LZC bytes are extracted without changing their compression format.\n";
}
int Run(const std::vector<std::string>& arguments,const std::string& firstArgument)
{
    auto options=Parse(arguments);
    if (options.help) {Help();return 0;}
    if (!options.hash.empty()) {std::cout<<FileHash(AbsoluteChecked(options.hash))<<'\n';return 0;}
    ResolveInput(options,ExecutableDirectory(firstArgument));
    const auto names=LoadNames(options.names);
    auto archive=ReadArchive(options,names);
    if (!options.lookupPath.empty() || options.lookupId) {LookupResource(archive,options);return 0;}
    if (!options.inventory.empty()) {WriteNameInventory(archive,names,options);return 0;}
    if (options.list) {
        std::cout<<"index,hash,offset,size,name\n";
        for (const auto& e : archive.entries)std::cout<<e.index<<','<<Hex32(e.hash)<<','<<e.offset<<','<<e.size<<','<<e.name<<'\n';
        return 0;
    }
    if (!options.verify.empty()) {
        const auto result=Verify(archive,options);
        std::cout<<(result.second?"FAILED":"VERIFIED")<<": "<<result.second<<" mismatching entries. Report: "<<Display(result.first)<<'\n';
        return result.second?3:0;
    }
    const auto output=Extract(archive,options);
    const auto zeros=std::count_if(archive.entries.begin(),archive.entries.end(),[](const Entry& e){return e.size==0;});
    const auto unknown=std::count_if(archive.entries.begin(),archive.entries.end(),[](const Entry& e){return !e.named;});
    std::cout<<"SUCCESS: "<<archive.entries.size()<<" records checked and extracted; "<<zeros
             <<" authored empty entries.\nUnknown filenames: "<<unknown
             <<"; all preserved under __Unknown by archive hash. Skipped records: 0.\nOutput: "
             <<Display(output)<<"\nManifest: "<<Display(output/ManifestName)<<'\n';
#ifdef _WIN32
    if (options.interactive) {
        const auto message=L"Extraction completed and SHA256 checked.\nRecords checked: "+
                           std::to_wstring(archive.entries.size())+L"\nUnknown filenames preserved in __Unknown: "+
                           std::to_wstring(unknown)+L"\nSkipped records: 0\n\n"+output.wstring()+
                           L"\n\nOriginal size-zero records are documented in NFSHP2_EXTRACTION_MANIFEST.json.";
        MessageBoxW(nullptr,message.c_str(),L"NFS HP2 archive extracted",MB_OK|MB_ICONINFORMATION);
    }
#endif
    return 0;
}
} // namespace

#ifdef _WIN32
std::string Utf8(const wchar_t* text)
{
    const int bytes=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,text,-1,nullptr,0,nullptr,nullptr);
    if (bytes<=0)throw Failure("Invalid Unicode command line");
    std::string result(static_cast<std::size_t>(bytes),'\0');
    if (!WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,text,-1,result.data(),bytes,nullptr,nullptr))throw Failure("Command line conversion failed");
    result.pop_back();return result;
}
int wmain(int argc,wchar_t** argv)
{
    SetConsoleOutputCP(CP_UTF8);
    try {
        std::vector<std::string> arguments;
        for (int i=1;i<argc;++i)arguments.push_back(Utf8(argv[i]));
        return Run(arguments,Utf8(argv[0]));
    } catch (const std::exception& error) {
        std::cerr<<"NFSHP2Unpack failed: "<<error.what()<<'\n';
        if (argc==1)MessageBoxA(nullptr,error.what(),"NFS HP2 archive error",MB_OK|MB_ICONERROR);
        return 2;
    }
}
#else
int main(int argc,char** argv)
{
    try {return Run(std::vector<std::string>(argv+1,argv+argc),argv[0]);}
    catch (const std::exception& error) {std::cerr<<"NFSHP2Unpack failed: "<<error.what()<<'\n';return 2;}
}
#endif
