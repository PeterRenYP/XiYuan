// 原生安装宿主：批准的 HTML 界面、分块校验解压、系统级安装与卸载。
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <bcrypt.h>
#include <compressapi.h>
#include <tlhelp32.h>
#include <WebView2.h>
#include <json.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace fs=std::filesystem;
using json=nlohmann::json;
static HWND windowHandle=nullptr;
static ICoreWebView2Controller *controller=nullptr;
static ICoreWebView2 *webview=nullptr;
static fs::path executable,sessionDirectory;
static std::atomic<bool> installing=false,cancelled=false;
static std::thread installThread;
static json payloadManifest;
static uint64_t hostSize=0;
static uint64_t primarySize=0;
static fs::path companionPath;
static std::wstring testRegistryRoot,trustedUri;
static constexpr UINT Notify=WM_APP+1;

// 协议与磁盘清单统一使用 UTF-8，Windows API 使用 UTF-16。
static std::wstring wide(const std::string &value){
    if(value.empty())return {};
    int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),int(value.size()),nullptr,0);
    if(!n)throw std::runtime_error("文本编码无效");
    std::wstring result(n,L'\0');MultiByteToWideChar(CP_UTF8,0,value.data(),int(value.size()),result.data(),n);return result;
}
static std::string utf8(const std::wstring &value){
    int n=WideCharToMultiByte(CP_UTF8,0,value.data(),int(value.size()),nullptr,0,nullptr,nullptr);
    std::string result(n,'\0');WideCharToMultiByte(CP_UTF8,0,value.data(),int(value.size()),result.data(),n,nullptr,nullptr);return result;
}

// 安装错误返回页面内的短提示，详细系统错误同时保留错误码。
static void require(bool value,const char *message){if(!value)throw std::runtime_error(std::string(message)+"（"+std::to_string(GetLastError())+"）");}
static fs::path known(REFKNOWNFOLDERID id){
    PWSTR text=nullptr;require(SUCCEEDED(SHGetKnownFolderPath(id,0,nullptr,&text)),"无法读取用户目录");
    fs::path path(text);CoTaskMemFree(text);return path;
}

class Sha256 {
    BCRYPT_ALG_HANDLE algorithm=nullptr;
    BCRYPT_HASH_HANDLE hash=nullptr;
  public:
    // 每个文件边解压边计算内容校验，不重复加载整个文件。
    Sha256(){require(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)>=0,"无法初始化内容校验");require(BCryptCreateHash(algorithm,&hash,nullptr,0,nullptr,0,0)>=0,"无法创建内容校验");}
    ~Sha256(){if(hash)BCryptDestroyHash(hash);if(algorithm)BCryptCloseAlgorithmProvider(algorithm,0);}
    void add(const void *data,size_t size){require(BCryptHashData(hash,(PUCHAR)data,ULONG(size),0)>=0,"内容校验失败");}
    std::string finish(){std::array<unsigned char,32> value{};require(BCryptFinishHash(hash,value.data(),32,0)>=0,"内容校验失败");std::ostringstream text;for(auto n:value)text<<std::hex<<std::setw(2)<<std::setfill('0')<<int(n);return text.str();}
};

// 仅解包自身资源；UI 和 WebView 加载器不会来自目标安装目录。
static void resourceTo(int id,const fs::path &path){
    auto resource=FindResourceW(nullptr,MAKEINTRESOURCEW(id),RT_RCDATA);require(resource,"安装资源缺失");
    auto data=LockResource(LoadResource(nullptr,resource));DWORD bytes=SizeofResource(nullptr,resource);
    std::ofstream output(path,std::ios::binary);require(bool(output),"无法创建临时资源");output.write((const char*)data,bytes);require(bool(output),"临时资源写入失败");
}
static void send(json value){PostMessageW(windowHandle,Notify,0,reinterpret_cast<LPARAM>(new json(std::move(value))));}

// 路径来自清单仍逐项校验，拒绝绝对路径、父目录跳转和重复目标。
static fs::path relativePath(const std::string &name){
    fs::path path=wide(name);
    if(path.empty()||path.is_absolute()||path.has_root_name()||name.find(':')!=std::string::npos||name.find('\\')!=std::string::npos)throw std::runtime_error("安装清单包含无效路径");
    for(const auto &part:path)if(part==L".."||part==L".")throw std::runtime_error("安装清单路径越界");
    return path;
}

// 文件尾清单具有独立 SHA-256，所有大文件偏移保持 64 位。
static void readPayload(bool notify=false){
    auto size=fs::file_size(executable);if(size<64)throw std::runtime_error("安装包不完整");
    std::ifstream input(executable,std::ios::binary);input.seekg(std::streamoff(size-64));
    uint64_t manifestSize;std::array<unsigned char,32> expected{};char magic[16];
    input.read((char*)&hostSize,8);input.read((char*)&manifestSize,8);input.read((char*)expected.data(),32);input.read(magic,16);
    const bool split=std::string(magic,16)==std::string("XIYUAN_SETUP_V2!\0",16);
    if((!split&&std::string(magic,16)!=std::string("XIYUAN_SETUP_V1!\0",16))||manifestSize>16*1024*1024||manifestSize>size-64||hostSize>size-64-manifestSize)throw std::runtime_error("安装包清单无效");
    input.seekg(std::streamoff(size-64-manifestSize));std::string data(manifestSize,'\0');input.read(data.data(),std::streamsize(data.size()));
    Sha256 digest;digest.add(data.data(),data.size());std::ostringstream hash;for(auto n:expected)hash<<std::hex<<std::setw(2)<<std::setfill('0')<<int(n);
    if(digest.finish()!=hash.str())throw std::runtime_error("安装清单校验失败，请重新下载安装包");
    auto manifest=json::parse(data);std::vector<std::wstring> paths;uint64_t total=0;
    primarySize=size-64-manifestSize;uint64_t logicalSize=primarySize;companionPath.clear();
    if(split){
        if(manifest.at("schema")!=2||manifest.at("primary_size").get<uint64_t>()!=primarySize)throw std::runtime_error("安装包分片信息无效");
        companionPath=executable;companionPath.replace_extension(L".data");
        const auto filename=utf8(companionPath.filename().wstring());
        std::error_code error;auto bytes=fs::file_size(companionPath,error);
        if(error)throw std::runtime_error("缺少配套数据包："+filename+"。请将两个文件下载到同一文件夹后重试。");
        const uint64_t expectedSize=manifest.at("data").at("size");
        if(bytes!=expectedSize||bytes>UINT64_MAX-primarySize)throw std::runtime_error("配套数据包大小不符，请重新下载："+filename);
        // 写入安装目录前完整校验数据包，防止混用版本、截断或下载损坏。
        if(notify)send({{"type","progress"},{"progress",0},{"message","校验配套数据包"}});
        std::ifstream companion(companionPath,std::ios::binary);require(bool(companion),"无法打开配套数据包");
        Sha256 checksum;std::vector<char> buffer(4*1024*1024);uint64_t left=bytes;
        while(left){if(cancelled)throw std::runtime_error("安装已取消");auto count=std::min<uint64_t>(left,buffer.size());companion.read(buffer.data(),count);require(bool(companion),"配套数据包读取失败");checksum.add(buffer.data(),count);left-=count;}
        if(checksum.finish()!=manifest.at("data").at("sha256"))throw std::runtime_error("配套数据包校验失败，请重新下载："+filename);
        logicalSize+=bytes;
    }
    for(const auto &file:manifest.at("files")){
        auto path=relativePath(file.at("path"));auto key=path.wstring();std::transform(key.begin(),key.end(),key.begin(),::towlower);
        if(std::find(paths.begin(),paths.end(),key)!=paths.end())throw std::runtime_error("安装清单目标重复");paths.push_back(key);
        uint64_t fileSize=0;
        for(const auto &block:file.at("blocks")){
            uint64_t offset=block[0],stored=block[1],raw=block[2];
            if(offset<hostSize||stored>5*1024*1024||raw>4*1024*1024||offset>logicalSize||stored>logicalSize-offset||(!block[3].get<int>()&&stored!=raw))throw std::runtime_error("安装文件偏移无效");
            fileSize+=raw;
        }
        if(fileSize!=file.at("size").get<uint64_t>())throw std::runtime_error("安装文件大小无效");total+=fileSize;
    }
    if(total!=manifest.at("total").get<uint64_t>())throw std::runtime_error("安装包总大小无效");
    payloadManifest=std::move(manifest);
}

// 解压仅写入本次新建的临时目录；取消或失败时不触碰既有安装。
static void extract(const fs::path &stage,bool notify){
    DECOMPRESSOR_HANDLE decompressor=nullptr;require(CreateDecompressor(COMPRESS_ALGORITHM_XPRESS_HUFF,nullptr,&decompressor),"无法初始化解压器");
    std::unique_ptr<std::remove_pointer_t<DECOMPRESSOR_HANDLE>,decltype(&CloseDecompressor)> cleanup(decompressor,CloseDecompressor);
    std::ifstream input(executable,std::ios::binary);uint64_t done=0,total=payloadManifest.at("total");int previous=-1;
    std::ifstream companion;if(!companionPath.empty()){companion.open(companionPath,std::ios::binary);require(bool(companion),"无法打开配套数据包");}
    for(const auto &file:payloadManifest.at("files")){
        const auto path=stage/relativePath(file.at("path"));fs::create_directories(path.parent_path());
        std::ofstream output(path,std::ios::binary);require(bool(output),"无法写入安装目录");Sha256 digest;
        for(const auto &block:file.at("blocks")){
            if(cancelled)throw std::runtime_error("安装已取消");
            uint64_t offset=block[0];size_t stored=block[1],raw=block[2];
            // 将两部分视为连续载荷；跨分界的压缩块也只读取一次，无需合并文件。
            std::vector<char> compressed(stored),decoded(raw);size_t first=offset<primarySize?size_t(std::min<uint64_t>(stored,primarySize-offset)):0;
            if(first){input.seekg(std::streamoff(offset));input.read(compressed.data(),first);require(bool(input),"安装包读取失败");}
            if(first<stored){companion.seekg(std::streamoff(offset+first-primarySize));companion.read(compressed.data()+first,stored-first);require(bool(companion),"配套数据包读取失败");}
            if(block[3].get<int>()){
                SIZE_T actual=0;require(Decompress(decompressor,compressed.data(),stored,decoded.data(),raw,&actual)&&actual==raw,"安装文件解压失败");
            }else decoded=std::move(compressed);
            output.write(decoded.data(),std::streamsize(raw));require(bool(output),"磁盘写入失败");digest.add(decoded.data(),raw);done+=raw;
            int progress=total?int(done*95/total):95;
            if(notify&&progress!=previous){send({{"type","progress"},{"progress",progress},{"message","放置程序文件"}});previous=progress;}
        }
        output.close();if(digest.finish()!=file.at("sha256"))throw std::runtime_error("安装文件校验失败："+file.at("path").get<std::string>());
    }
}

// 正式安装写系统注册信息；测试仍隔离在当前用户分支，不改默认应用。
static HKEY registryHive(){return testRegistryRoot.empty()?HKEY_LOCAL_MACHINE:HKEY_CURRENT_USER;}

// 从当前安装范围读取注册值，缺失时返回空文本。
static std::wstring registryString(const std::wstring &key,const wchar_t *name=nullptr){
    const auto path=testRegistryRoot.empty()?key:testRegistryRoot+L"\\"+key;
    DWORD bytes=0,type=0;if(RegGetValueW(registryHive(),path.c_str(),name,RRF_RT_REG_SZ,&type,nullptr,&bytes)!=ERROR_SUCCESS)return {};
    std::wstring value(bytes/sizeof(wchar_t),L'\0');if(RegGetValueW(registryHive(),path.c_str(),name,RRF_RT_REG_SZ,&type,value.data(),&bytes)!=ERROR_SUCCESS)return {};
    while(!value.empty()&&value.back()==L'\0')value.pop_back();return value;
}
// 所有安装信息沿用同一范围，卸载时可准确恢复关联。
static void setRegistry(const std::wstring &key,const wchar_t *name,const std::wstring &value){
    const auto path=testRegistryRoot.empty()?key:testRegistryRoot+L"\\"+key;
    HKEY handle=nullptr;require(RegCreateKeyExW(registryHive(),path.c_str(),0,nullptr,0,KEY_SET_VALUE,nullptr,&handle,nullptr)==ERROR_SUCCESS,"无法创建应用注册信息");
    auto code=RegSetValueExW(handle,name,0,REG_SZ,(const BYTE*)value.c_str(),DWORD((value.size()+1)*sizeof(wchar_t)));RegCloseKey(handle);require(code==ERROR_SUCCESS,"无法写入应用注册信息");
}

// 读写及卸载统一使用相同测试分支，测试不能清理正式注册信息。
static std::wstring registryPath(const std::wstring &key){return testRegistryRoot.empty()?key:testRegistryRoot+L"\\"+key;}
static void deleteRegistry(const std::wstring &key){RegDeleteTreeW(registryHive(),registryPath(key).c_str());}

// 安装记录必须仍指向程序或安装清单，已删除的旧目录不能阻止重新安装。
static bool hasInstallation(const fs::path &path){
    std::error_code error;
    return path.is_absolute()&&(fs::is_regular_file(path/L".xiyuan-install.json",error)||fs::is_regular_file(path/L"Xiyuan.exe",error));
}

// 分别读取系统、用户和两种注册表视图；测试分支始终与正式安装隔离。
static fs::path registeredInstallation(bool user,REGSAM view){
    const std::wstring key=L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Xiyuan";
    auto root=user?HKEY_CURRENT_USER:HKEY_LOCAL_MACHINE;
    const auto path=testRegistryRoot.empty()?key:testRegistryRoot+(user?L"\\LegacyUser\\":L"\\")+key;
    if(!testRegistryRoot.empty())root=HKEY_CURRENT_USER;
    HKEY handle=nullptr;if(RegOpenKeyExW(root,path.c_str(),0,KEY_QUERY_VALUE|view,&handle)!=ERROR_SUCCESS)return {};
    wchar_t location[32768]={};DWORD bytes=sizeof(location);
    auto code=RegGetValueW(handle,nullptr,L"InstallLocation",RRF_RT_REG_SZ,nullptr,location,&bytes);RegCloseKey(handle);
    if(code!=ERROR_SUCCESS)return {};
    fs::path result(location);return hasInstallation(result)?result.lexically_normal():fs::path();
}

// 旧用户安装允许在原目录迁移为系统安装。
static fs::path legacyUserInstallation(){
    for(auto view:{KEY_WOW64_64KEY,KEY_WOW64_32KEY}){auto path=registeredInstallation(true,view);if(!path.empty())return path;}return {};
}

// 首选有效注册记录，再兼容默认位置的安装清单，首次安装仍使用系统程序目录。
static fs::path existingInstallation(){
    for(bool user:{false,true})for(auto view:{KEY_WOW64_64KEY,KEY_WOW64_32KEY}){auto path=registeredInstallation(user,view);if(!path.empty())return path;}
    if(testRegistryRoot.empty())for(const auto &path:{known(FOLDERID_ProgramFiles)/L"XiYuan",known(FOLDERID_LocalAppData)/L"Programs/XiYuan",known(FOLDERID_LocalAppData)/L"XiYuan"})if(fs::is_regular_file(path/L".xiyuan-install.json"))return path;
    return {};
}

// 路径比较忽略大小写及末尾分隔符，兼容安装记录中的目录写法。
static bool sameInstallation(const fs::path &left,const fs::path &right){
    auto normalize=[](fs::path path){path=path.lexically_normal();while(path!=path.root_path()&&path.filename().empty())path=path.parent_path();return path;};
    return _wcsicmp(normalize(left).c_str(),normalize(right).c_str())==0;
}

struct RunningProgram {DWORD pid;std::wstring path;};
struct BusyError:std::runtime_error {using std::runtime_error::runtime_error;};

// 从进程句柄核对实际可执行路径，避免按名称误伤其他位置的同名程序。
static bool processInInstallation(HANDLE process,const fs::path &target,std::wstring *image=nullptr){
    wchar_t buffer[32768];DWORD size=32768;if(!QueryFullProcessImageNameW(process,0,buffer,&size))return false;
    std::error_code error;auto root=fs::weakly_canonical(target,error);if(error)root=target.lexically_normal();
    auto prefix=root.wstring();if(prefix.back()!=L'\\')prefix+=L'\\';
    auto path=fs::weakly_canonical(fs::path(buffer),error).wstring();if(error)path=fs::path(buffer).lexically_normal().wstring();
    if(path.size()<=prefix.size()||_wcsnicmp(path.c_str(),prefix.c_str(),prefix.size())!=0)return false;
    if(image)*image=path;return true;
}

// 无窗口的运行时与辅助进程也必须检测，安装器自身始终排除。
static std::vector<RunningProgram> runningPrograms(const fs::path &target){
    std::vector<RunningProgram> result;auto snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);require(snapshot!=INVALID_HANDLE_VALUE,"无法检查运行中的程序");
    PROCESSENTRY32W entry{};entry.dwSize=sizeof(entry);
    if(Process32FirstW(snapshot,&entry))do{if(entry.th32ProcessID==GetCurrentProcessId())continue;auto process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,entry.th32ProcessID);if(!process)continue;std::wstring image;if(processInInstallation(process,target,&image))result.push_back({entry.th32ProcessID,image});CloseHandle(process);}while(Process32NextW(snapshot,&entry));
    CloseHandle(snapshot);return result;
}

// 仅在用户选择强制关闭后执行，并在终止前重新核对进程路径。
static void ensureProgramsClosed(const fs::path &target,bool force){
    for(int attempt=0;attempt<3;++attempt){auto programs=runningPrograms(target);if(programs.empty())return;
        if(!force)throw BusyError("检测到析元或后台组件仍在运行，请关闭后重试，或选择强制关闭后继续。");
        std::vector<HANDLE> handles;
        try{for(const auto &program:programs){auto process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|PROCESS_TERMINATE|SYNCHRONIZE,FALSE,program.pid);if(!process){if(GetLastError()==ERROR_INVALID_PARAMETER)continue;require(false,"无法关闭析元进程");}handles.push_back(process);if(processInInstallation(process,target)&&WaitForSingleObject(process,0)==WAIT_TIMEOUT)require(TerminateProcess(process,1)||WaitForSingleObject(process,0)==WAIT_OBJECT_0,"无法强制关闭析元进程");}
            const auto deadline=GetTickCount64()+5000;for(auto process:handles){auto now=GetTickCount64();require(WaitForSingleObject(process,DWORD(now<deadline?deadline-now:0))==WAIT_OBJECT_0,"等待析元进程退出超时");}}
        catch(...){for(auto handle:handles)CloseHandle(handle);throw;}for(auto handle:handles)CloseHandle(handle);
    }
    if(!runningPrograms(target).empty())throw BusyError("析元后台组件仍在运行，请关闭后重试。");
}

// 共享冲突才属于占用；权限、只读属性和其他系统错误分别提示，并逐个释放句柄。
static void checkReplaceable(const fs::path &path){
    const auto attributes=GetFileAttributesW(path.c_str());
    if(attributes!=INVALID_FILE_ATTRIBUTES&&(attributes&FILE_ATTRIBUTE_READONLY))throw std::runtime_error("程序文件为只读，请取消只读属性后重试："+utf8(path.wstring()));
    auto handle=CreateFileW(path.c_str(),DELETE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(handle!=INVALID_HANDLE_VALUE){CloseHandle(handle);return;}
    const auto code=GetLastError();const auto name=utf8(path.wstring());
    if(code==ERROR_SHARING_VIOLATION||code==ERROR_LOCK_VIOLATION)throw BusyError("程序文件正在使用，请关闭占用程序后重试："+name+"（"+std::to_string(code)+"）");
    if(code==ERROR_ACCESS_DENIED){const auto attributes=GetFileAttributesW(path.c_str());throw std::runtime_error((attributes!=INVALID_FILE_ATTRIBUTES&&(attributes&FILE_ATTRIBUTE_READONLY)?"程序文件为只读，请取消只读属性后重试：":"没有替换程序文件的权限，请检查目录权限后重试：")+name+"（"+std::to_string(code)+"）");}
    throw std::runtime_error("无法检查程序文件，请检查磁盘与目录状态："+name+"（"+std::to_string(code)+"）");
}

// 不跟随目录联接或符号链接，避免卸载清单跨入用户其他目录。
static void checkReparse(const fs::path &path){
    auto current=path;
    while(!current.empty()){
        auto attributes=GetFileAttributesW(current.c_str());
        if(attributes!=INVALID_FILE_ATTRIBUTES&&(attributes&FILE_ATTRIBUTE_REPARSE_POINT))throw std::runtime_error("安装路径包含目录联接或链接，请使用普通目录");
        const auto parent=current.parent_path();if(parent==current)break;current=parent;
    }
}

// 快捷方式明确指向安装目录中的析元，工作目录也使用该目录。
static void shortcut(const fs::path &link,const fs::path &target,const wchar_t *arguments=L""){
    IShellLinkW *shell=nullptr;require(SUCCEEDED(CoCreateInstance(CLSID_ShellLink,nullptr,CLSCTX_INPROC_SERVER,IID_IShellLinkW,(void**)&shell)),"无法创建快捷方式");
    shell->SetPath(target.c_str());shell->SetArguments(arguments);shell->SetWorkingDirectory(target.parent_path().c_str());shell->SetDescription(L"析元");shell->SetIconLocation(target.c_str(),0);
    IPersistFile *file=nullptr;shell->QueryInterface(IID_IPersistFile,(void**)&file);fs::create_directories(link.parent_path());auto code=file->Save(link.c_str(),TRUE);file->Release();shell->Release();require(SUCCEEDED(code),"快捷方式保存失败");
}

// 手动输入盘符时补齐独立目录；浏览父目录时自动加入产品文件夹。
static fs::path installationPath(const std::wstring &input,bool browsing=false){
    fs::path path=input.size()==2&&input[1]==L':'?fs::path(input+L"\\"):fs::path(input);
    if(!path.is_absolute())throw std::runtime_error("请输入完整安装目录");
    path=path.lexically_normal();while(path!=path.root_path()&&path.filename().empty())path=path.parent_path();
    if(path==path.root_path()||(browsing&&_wcsicmp(path.filename().c_str(),L"XiYuan")!=0&&!fs::exists(path/L"Xiyuan.exe")&&!fs::exists(path/L".xiyuan-install.json")))path/=L"XiYuan";
    if(path.wstring().size()>200)throw std::runtime_error("请选择长度合适的独立安装目录");
    return path;
}

// 绑定目录和已有程序的实际内容，确认后目录变化时必须重新确认。
static std::string installationSignature(const fs::path &target){
    Sha256 digest;const auto name=utf8(target.wstring());digest.add(name.data(),name.size());bool found=false;
    for(const auto *file:{L"Xiyuan.exe",L"ModelObservatory.exe",L"ModelObservatory-Runtime.exe",L".xiyuan-install.json"}){
        auto path=target/file;checkReparse(path);if(!fs::exists(path))continue;
        if(!fs::is_regular_file(path))throw std::runtime_error("安装目录中存在同名文件夹");
        found=true;const auto label=utf8(file);digest.add(label.data(),label.size());
        std::ifstream input(path,std::ios::binary);require(bool(input),"无法读取已有程序");std::array<char,65536> buffer;
        while(input){input.read(buffer.data(),buffer.size());if(input.gcount())digest.add(buffer.data(),size_t(input.gcount()));}
        if(!input.eof())throw std::runtime_error("已有程序读取失败");
    }
    return found?digest.finish():std::string();
}

struct RegistryValue {HKEY hive=nullptr;std::wstring key,name;bool present=false,keyPresent=false;DWORD type=REG_SZ;std::vector<BYTE> bytes;};
// 保存将要修改的注册值；失败时连同原来不存在的值一起恢复。
static std::vector<RegistryValue> snapshotRegistration(bool association,HKEY hive=nullptr,bool legacy=false){
    if(!hive)hive=registryHive();
    std::vector<RegistryValue> values;
    auto capture=[&](const std::wstring &key,const wchar_t *name){
        RegistryValue value;value.hive=hive;value.key=legacy&&!testRegistryRoot.empty()?testRegistryRoot+L"\\LegacyUser\\"+key:registryPath(key);value.name=name?name:L"";HKEY handle=nullptr;
        if(RegOpenKeyExW(hive,value.key.c_str(),0,KEY_QUERY_VALUE,&handle)==ERROR_SUCCESS){
            value.keyPresent=true;DWORD bytes=0;
            if(RegQueryValueExW(handle,value.name.c_str(),nullptr,&value.type,nullptr,&bytes)==ERROR_SUCCESS){value.present=true;value.bytes.resize(bytes);auto code=RegQueryValueExW(handle,value.name.c_str(),nullptr,&value.type,value.bytes.data(),&bytes);RegCloseKey(handle);require(code==ERROR_SUCCESS,"无法保存原注册信息");}
            else RegCloseKey(handle);
        }
        values.push_back(std::move(value));
    };
    const std::wstring uninstall=L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Xiyuan";
    for(const auto *name:{L"DisplayName",L"DisplayVersion",L"Publisher",L"DisplayIcon",L"InstallLocation",L"UninstallString"})capture(uninstall,name);
    if(association){const std::wstring base=L"Software\\Classes\\Xiyuan.Safetensors";capture(base,nullptr);capture(base+L"\\DefaultIcon",nullptr);capture(base+L"\\shell\\open\\command",nullptr);capture(L"Software\\Classes\\.safetensors\\OpenWithProgids",L"Xiyuan.Safetensors");capture(L"Software\\Classes\\.safetensors",nullptr);}
    return values;
}

// 按相反顺序恢复注册值，仅清除本次创建且已为空的注册分支。
static bool restoreRegistration(const std::vector<RegistryValue> &values){
    bool restored=true;
    for(auto it=values.rbegin();it!=values.rend();++it){HKEY handle=nullptr;
        if(it->present){if(RegCreateKeyExW(it->hive,it->key.c_str(),0,nullptr,0,KEY_SET_VALUE,nullptr,&handle,nullptr)!=ERROR_SUCCESS){restored=false;continue;}if(RegSetValueExW(handle,it->name.c_str(),0,it->type,it->bytes.data(),DWORD(it->bytes.size()))!=ERROR_SUCCESS)restored=false;RegCloseKey(handle);}
        else if(RegOpenKeyExW(it->hive,it->key.c_str(),0,KEY_SET_VALUE,&handle)==ERROR_SUCCESS){auto code=RegDeleteValueW(handle,it->name.c_str());if(code!=ERROR_SUCCESS&&code!=ERROR_FILE_NOT_FOUND)restored=false;RegCloseKey(handle);if(!it->keyPresent)RegDeleteKeyW(it->hive,it->key.c_str());}
    }
    return restored;
}

struct ShortcutBackup {fs::path path,backup;bool existed=false,touched=false;};
// 测试快捷入口放在独立目录，正式安装使用系统桌面和开始菜单。
static std::vector<fs::path> shortcutPaths(const fs::path &target){
    auto test=target.parent_path()/(target.filename().wstring()+L".shortcuts-test");
    return {(testRegistryRoot.empty()?known(FOLDERID_PublicDesktop):test/L"桌面")/L"析元.lnk",
        (testRegistryRoot.empty()?known(FOLDERID_CommonPrograms):test/L"开始菜单")/L"析元/析元.lnk",
        (testRegistryRoot.empty()?known(FOLDERID_CommonPrograms):test/L"开始菜单")/L"析元/卸载析元.lnk"};
}

static void install(const json &options){
    // 只移动安装清单管理的文件，不移动或复制用户模型与项目。
    fs::path stage,backup,target;bool stageOwned=false,backupOwned=false,targetCreated=false,registrationTouched=false,committed=false;
    std::vector<fs::path> originals,published,newDirectories;std::vector<RegistryValue> registration,legacyRegistration;std::vector<ShortcutBackup> links;
    CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    auto ensureDirectory=[&](const fs::path &directory){std::vector<fs::path> missing;for(auto current=directory;!current.empty()&&!fs::exists(current);current=current.parent_path())missing.push_back(current);for(auto it=missing.rbegin();it!=missing.rend();++it){fs::create_directory(*it);newDirectories.push_back(*it);}};
    try{
        readPayload(true);target=installationPath(wide(options.at("path")));checkReparse(target);
        if(fs::exists(target)&&!fs::is_directory(target))throw std::runtime_error("安装位置不是文件夹，请更换目录");
        const auto existing=existingInstallation();
        const auto legacy=legacyUserInstallation();const bool migrating=!legacy.empty()&&sameInstallation(legacy,target);
        if((!legacy.empty()&&!migrating)||(!existing.empty()&&!sameInstallation(existing,target)))throw std::runtime_error("析元已安装，请选择原安装目录更新："+utf8((!legacy.empty()&&!migrating?legacy:existing).wstring()));
        const auto signature=installationSignature(target);
        if(!signature.empty()&&options.value("confirm_replace",std::string())!=signature){send({{"type","overwrite"},{"path",utf8(target.wstring())},{"signature",signature}});installing=false;CoUninitialize();return;}
        json previous;const auto oldManifest=target/L".xiyuan-install.json";
        if(fs::exists(oldManifest)){if(fs::file_size(oldManifest)>16*1024*1024)throw std::runtime_error("原安装信息无效，请先卸载原程序");std::ifstream in(oldManifest);previous=json::parse(in);if(_wcsicmp(fs::path(wide(previous.at("root"))).lexically_normal().c_str(),target.c_str())!=0)throw std::runtime_error("原安装目录记录不一致，请先卸载原程序");}
        std::vector<fs::path> current;for(const auto &file:payloadManifest.at("files"))current.push_back(relativePath(file.at("path")));current.push_back(L"Uninstall.exe");current.push_back(L".xiyuan-install.json");auto managed=current;
        // 旧清单中的过期程序文件也先备份，更新成功后才清除。
        if(!previous.is_null())for(const auto &file:previous.at("files")){auto path=relativePath(file.at("path"));if(std::find(managed.begin(),managed.end(),path)==managed.end())managed.push_back(path);}
        // 解压前先检查占用，避免等待完整安装包解压后才发现后台进程。
        auto preflight=[&](){ensureProgramsClosed(target,options.value("force_close",false));for(const auto &relative:managed){auto path=target/relative;checkReparse(path);if(!fs::exists(path))continue;if(!fs::is_regular_file(path))throw std::runtime_error("程序文件位置存在同名文件夹，请更换目录");if(signature.empty())throw std::runtime_error("目录中存在同名文件，请更换安装目录");checkReplaceable(path);}};
        preflight();
        ensureDirectory(target.parent_path());auto space=fs::space(target.parent_path());if(space.available<payloadManifest.at("total").get<uint64_t>()+256*1024*1024)throw std::runtime_error("安装磁盘空间不足");
        stage=target.parent_path()/(target.filename().wstring()+L".install-"+std::to_wstring(GetCurrentProcessId()));
        backup=target.parent_path()/(target.filename().wstring()+L".rollback-"+std::to_wstring(GetCurrentProcessId()));
        if(fs::exists(stage)||fs::exists(backup))throw std::runtime_error("临时安装目录已存在，请关闭其他安装器");
        if(!fs::create_directory(stage))throw std::runtime_error("无法创建临时安装目录");stageOwned=true;extract(stage,true);if(cancelled)throw std::runtime_error("安装已取消");
        // 卸载器仅保留宿主本身，无需重复保存数 GiB 的安装载荷。
        {std::ifstream input(executable,std::ios::binary);std::ofstream output(stage/L"Uninstall.exe",std::ios::binary);std::array<char,65536> buffer;uint64_t left=hostSize;
         while(left){auto count=std::min<uint64_t>(left,buffer.size());input.read(buffer.data(),count);require(bool(input),"无法读取卸载程序");output.write(buffer.data(),count);left-=count;}require(bool(output),"卸载器写入失败");}
        auto installed=payloadManifest;installed["options"]=options;installed["options"].erase("confirm_replace");installed["options"].erase("force_close");installed["scope"]="machine";installed["root"]=utf8(target.wstring());installed["shortcuts"]=json::array();
        auto paths=shortcutPaths(target);if(migrating){paths.push_back(known(FOLDERID_Desktop)/L"析元.lnk");paths.push_back(known(FOLDERID_Programs)/L"析元/析元.lnk");paths.push_back(known(FOLDERID_Programs)/L"析元/卸载析元.lnk");}std::vector<fs::path> selected;
        if(options.value("desktop",true))selected.push_back(paths[0]);if(options.value("menu",true)){selected.push_back(paths[1]);selected.push_back(paths[2]);}
        if(!previous.is_null())for(const auto &entry:previous.value("shortcuts",json::array())){auto link=fs::path(wide(entry));if(std::find(paths.begin(),paths.end(),link)!=paths.end()&&fs::exists(link)&&std::find(selected.begin(),selected.end(),link)==selected.end())selected.push_back(link);}
        for(const auto &link:selected)installed["shortcuts"].push_back(utf8(link.wstring()));
        installed["previous_association"]=!migrating&&!previous.is_null()?previous.value("previous_association",std::string()):utf8(registryString(L"Software\\Classes\\.safetensors"));
        {std::ofstream out(stage/L".xiyuan-install.json");out<<installed.dump(2);require(bool(out),"安装清单保存失败");}
        // 用户文件不能被隐式转换成程序目录，结构冲突在替换前直接拦截。
        for(const auto &relative:current)for(auto parent=(target/relative).parent_path();parent!=target&&parent!=parent.root_path();parent=parent.parent_path())if(fs::exists(parent)&&!fs::is_directory(parent))throw std::runtime_error("目录中存在同名用户文件，请更换安装目录");
        preflight();
        // 再次核对确认时的程序，解压期间被修改也不能静默覆盖。
        if(installationSignature(target)!=signature)throw std::runtime_error("原程序在安装期间发生变化，请重新确认安装");
        registration=snapshotRegistration(options.value("associate",false));if(migrating){legacyRegistration=snapshotRegistration(true,HKEY_CURRENT_USER,true);registration.insert(registration.end(),legacyRegistration.begin(),legacyRegistration.end());}
        if(!fs::create_directory(backup))throw std::runtime_error("无法创建程序备份目录");backupOwned=true;
        for(size_t i=0;i<selected.size();++i){const auto &link=selected[i];checkReparse(link);ShortcutBackup saved{link,backup/L"shortcuts"/std::to_wstring(i),fs::exists(link)};if(saved.existed){fs::create_directories(saved.backup.parent_path());fs::copy_file(link,saved.backup);}links.push_back(saved);}
        if(!fs::exists(target)){fs::create_directory(target);targetCreated=true;}
        for(const auto &relative:managed){auto path=target/relative;if(!fs::exists(path))continue;auto saved=backup/L"files"/relative;fs::create_directories(saved.parent_path());fs::rename(path,saved);originals.push_back(relative);}
        for(const auto &relative:current){if(cancelled)throw std::runtime_error("安装已取消");auto path=target/relative;ensureDirectory(path.parent_path());fs::rename(stage/relative,path);published.push_back(relative);}
        if(!testRegistryRoot.empty()&&options.value("test_failure",std::string())=="files")throw std::runtime_error("验收模拟：程序替换后失败");
        send({{"type","progress"},{"progress",97},{"message","设置快捷入口"}});
        for(size_t i=0;i<selected.size();++i){const auto &link=selected[i];links[i].touched=true;const bool removal=link.filename()==L"卸载析元.lnk";shortcut(link,target/(removal?L"Uninstall.exe":L"Xiyuan.exe"),removal?L"--uninstall":L"");}
        registrationTouched=true;const auto app=target/L"Xiyuan.exe";
        if(options.value("associate",false)){const std::wstring base=L"Software\\Classes\\Xiyuan.Safetensors";setRegistry(base,nullptr,L"析元模型权重");setRegistry(base+L"\\DefaultIcon",nullptr,L"\""+app.wstring()+L"\",0");setRegistry(base+L"\\shell\\open\\command",nullptr,L"\""+app.wstring()+L"\" --open-file \"%1\"");setRegistry(L"Software\\Classes\\.safetensors\\OpenWithProgids",L"Xiyuan.Safetensors",L"");setRegistry(L"Software\\Classes\\.safetensors",nullptr,L"Xiyuan.Safetensors");}
        const std::wstring uninstall=L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Xiyuan";
        setRegistry(uninstall,L"DisplayName",L"析元");setRegistry(uninstall,L"DisplayVersion",L"1.0.4");setRegistry(uninstall,L"Publisher",L"析元");setRegistry(uninstall,L"DisplayIcon",app.wstring());setRegistry(uninstall,L"InstallLocation",target.wstring());setRegistry(uninstall,L"UninstallString",L"\""+(target/L"Uninstall.exe").wstring()+L"\" --uninstall");
        // 同目录迁移旧按用户安装时，清理旧入口并恢复原用户级默认关联。
        if(migrating){
            bool restoreAssociation=false;for(const auto &value:legacyRegistration)if(value.key==L"Software\\Classes\\.safetensors"&&value.present&&value.type==REG_SZ&&value.bytes.size()>=sizeof(wchar_t)){const auto *text=reinterpret_cast<const wchar_t*>(value.bytes.data());restoreAssociation=std::wstring(text,(value.bytes.size()/sizeof(wchar_t))-1)==L"Xiyuan.Safetensors";}
            for(auto it=legacyRegistration.rbegin();it!=legacyRegistration.rend();++it){if(it->key==L"Software\\Classes\\.safetensors"&&!restoreAssociation)continue;HKEY handle=nullptr;if(RegOpenKeyExW(HKEY_CURRENT_USER,it->key.c_str(),0,KEY_SET_VALUE,&handle)==ERROR_SUCCESS){RegDeleteValueW(handle,it->name.c_str());RegCloseKey(handle);RegDeleteKeyW(HKEY_CURRENT_USER,it->key.c_str());}}
            const auto association=previous.value("previous_association",std::string());if(restoreAssociation&&!association.empty()){HKEY handle=nullptr;require(RegCreateKeyExW(HKEY_CURRENT_USER,L"Software\\Classes\\.safetensors",0,nullptr,0,KEY_SET_VALUE,nullptr,&handle,nullptr)==ERROR_SUCCESS,"无法恢复原用户文件关联");const auto value=wide(association);auto code=RegSetValueExW(handle,nullptr,0,REG_SZ,(const BYTE*)value.c_str(),DWORD((value.size()+1)*sizeof(wchar_t)));RegCloseKey(handle);require(code==ERROR_SUCCESS,"无法恢复原用户文件关联");}
        }
        if(!testRegistryRoot.empty()&&options.value("test_failure",std::string())=="registration")throw std::runtime_error("验收模拟：注册后失败");
        if(cancelled)throw std::runtime_error("安装已取消");
        committed=true;std::error_code ignored;fs::remove_all(stage,ignored);fs::remove_all(backup,ignored);SHChangeNotify(SHCNE_ASSOCCHANGED,SHCNF_IDLIST,nullptr,nullptr);send({{"type","complete"},{"path",utf8(target.wstring())}});
    }catch(const std::exception &error){
        // 恢复程序、快捷方式与注册值；恢复不完整时保留备份并明确告知位置。
        bool restored=true;std::error_code code;std::string recoveryErrors;
        if(!committed){if(registrationTouched&&!restoreRegistration(registration))restored=false;
            for(auto it=links.rbegin();it!=links.rend();++it){if(!it->touched)continue;if(it->existed){fs::remove(it->path,code);if(!code)fs::rename(it->backup,it->path,code);if(code){restored=false;recoveryErrors+="；快捷方式恢复失败（"+std::to_string(code.value())+"）："+utf8(it->path.wstring());}}else{fs::remove(it->path,code);if(code)restored=false;}}
            for(auto it=published.rbegin();it!=published.rend();++it){fs::remove(target/ *it,code);if(code)restored=false;}
            for(auto it=originals.rbegin();it!=originals.rend();++it){fs::rename(backup/L"files"/ *it,target/ *it,code);if(code)restored=false;}
            for(auto it=newDirectories.rbegin();it!=newDirectories.rend();++it){if(fs::is_empty(*it,code))fs::remove(*it,code);}
            if(targetCreated&&fs::is_empty(target,code))fs::remove(target,code);
            if(stageOwned)fs::remove_all(stage,code);if(backupOwned&&restored)fs::remove_all(backup,code);
        }
        auto message=std::string(error.what())+recoveryErrors;if(!restored)message+="；部分文件未能恢复，原程序备份位于："+utf8(backup.wstring());
        installing=false;
        if(dynamic_cast<const BusyError*>(&error)&&restored){json programs=json::array();for(const auto &program:runningPrograms(target)){const auto name=fs::path(program.path).filename().wstring();const auto label=_wcsicmp(name.c_str(),L"Xiyuan.exe")==0?"析元主程序":_wcsicmp(name.c_str(),L"ModelObservatory-Runtime.exe")==0?"析元运行时":_wcsicmp(name.c_str(),L"Xiyuan-MCP.exe")==0?"析元 MCP 服务":"析元辅助程序";programs.push_back({{"pid",program.pid},{"name",label}});}send({{"type","busy"},{"message",message},{"programs",programs},{"can_force",!programs.empty()}});}
        else send({{"type","error"},{"message",message}});
    }
    installing=false;CoUninitialize();
}

// 卸载只处理安装清单内的文件，用户在安装目录中新建的模型和项目会保留。
static int uninstall(const fs::path &root){
    auto absolute=fs::absolute(root).lexically_normal();std::ifstream stream(absolute/L".xiyuan-install.json");if(!stream)return 2;
    auto manifest=json::parse(stream);stream.close();if(_wcsicmp(fs::path(wide(manifest.at("root"))).lexically_normal().c_str(),absolute.c_str())!=0)return 3;
    auto app=absolute/L"Xiyuan.exe";
    checkReparse(absolute);
    // 卸载沿用同一占用判定，权限错误不再误报为程序正在运行。
    ensureProgramsClosed(absolute,false);
    for(const auto &value:manifest.at("files")){auto path=absolute/relativePath(value.at("path"));checkReparse(path);if(fs::exists(path))checkReplaceable(path);}
    for(const auto &value:manifest.at("files")){
        auto path=absolute/relativePath(value.at("path"));std::error_code error;fs::remove(path,error);
        if(error)throw std::runtime_error("文件正在使用，请退出析元后重试");
    }
    for(const auto &value:manifest.value("shortcuts",json::array())){
        fs::path link=wide(value);IShellLinkW *shell=nullptr;
        if(SUCCEEDED(CoCreateInstance(CLSID_ShellLink,nullptr,CLSCTX_INPROC_SERVER,IID_IShellLinkW,(void**)&shell))){
            IPersistFile *file=nullptr;shell->QueryInterface(IID_IPersistFile,(void**)&file);
            if(SUCCEEDED(file->Load(link.c_str(),STGM_READ))){wchar_t target[32768];WIN32_FIND_DATAW data;if(SUCCEEDED(shell->GetPath(target,32768,&data,SLGP_RAWPATH))&&(fs::path(target)==app||fs::path(target)==absolute/L"Uninstall.exe"))fs::remove(link);}
            file->Release();shell->Release();
        }
    }
    auto command=registryString(L"Software\\Classes\\Xiyuan.Safetensors\\shell\\open\\command");
    if(command==L"\""+app.wstring()+L"\" --open-file \"%1\""){
        deleteRegistry(L"Software\\Classes\\Xiyuan.Safetensors");
        HKEY extension=nullptr;if(RegOpenKeyExW(registryHive(),registryPath(L"Software\\Classes\\.safetensors\\OpenWithProgids").c_str(),0,KEY_SET_VALUE,&extension)==ERROR_SUCCESS){RegDeleteValueW(extension,L"Xiyuan.Safetensors");RegCloseKey(extension);}
        if(registryString(L"Software\\Classes\\.safetensors")==L"Xiyuan.Safetensors"){
            auto previous=wide(manifest.value("previous_association",std::string()));setRegistry(L"Software\\Classes\\.safetensors",nullptr,previous);
        }
    }
    if(registryString(L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Xiyuan",L"InstallLocation")==absolute.wstring())deleteRegistry(L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Xiyuan");
    SHChangeNotify(SHCNE_ASSOCCHANGED,SHCNF_IDLIST,nullptr,nullptr);
    fs::remove(absolute/L"Uninstall.exe");fs::remove(absolute/L".xiyuan-install.json");
    std::vector<fs::path> directories;for(auto it=fs::recursive_directory_iterator(absolute);it!=fs::recursive_directory_iterator();++it){auto attributes=GetFileAttributesW(it->path().c_str());if(attributes&FILE_ATTRIBUTE_REPARSE_POINT){it.disable_recursion_pending();continue;}if(it->is_directory())directories.push_back(it->path());}
    std::sort(directories.rbegin(),directories.rend());for(const auto &path:directories)if(fs::is_empty(path))fs::remove(path);if(fs::is_empty(absolute))fs::remove(absolute);
    return 0;
}

// 轻量 COM 回调避免额外引入 CLR 或框架运行时。
template<class Interface,const IID *Identity,class... Args>class Callback final:public Interface{
    std::atomic<ULONG> references{1};std::function<HRESULT(Args...)> function;
  public:
    explicit Callback(std::function<HRESULT(Args...)> value):function(std::move(value)){}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void **out)override{if(iid==IID_IUnknown||iid==*Identity){*out=static_cast<Interface*>(this);AddRef();return S_OK;}*out=nullptr;return E_NOINTERFACE;}
    ULONG STDMETHODCALLTYPE AddRef()override{return ++references;}
    ULONG STDMETHODCALLTYPE Release()override{auto n=--references;if(!n)delete this;return n;}
    HRESULT STDMETHODCALLTYPE Invoke(Args... args)override{return function(args...);}
};

class EnvironmentOptions final:public ICoreWebView2EnvironmentOptions{
    ULONG references=1;std::wstring arguments;
    // 环境选项不依赖 WRL，正式运行不开启远程调试。
    static HRESULT copy(const std::wstring &text,LPWSTR *out){*out=(LPWSTR)CoTaskMemAlloc((text.size()+1)*sizeof(wchar_t));if(!*out)return E_OUTOFMEMORY;wcscpy_s(*out,text.size()+1,text.c_str());return S_OK;}
  public:
    explicit EnvironmentOptions(std::wstring value):arguments(std::move(value)){}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void **out)override{if(iid==IID_IUnknown||iid==IID_ICoreWebView2EnvironmentOptions){*out=this;AddRef();return S_OK;}*out=nullptr;return E_NOINTERFACE;}
    ULONG STDMETHODCALLTYPE AddRef()override{return ++references;}
    ULONG STDMETHODCALLTYPE Release()override{auto n=--references;if(!n)delete this;return n;}
    HRESULT STDMETHODCALLTYPE get_AdditionalBrowserArguments(LPWSTR *out)override{return copy(arguments,out);}
    HRESULT STDMETHODCALLTYPE put_AdditionalBrowserArguments(LPCWSTR value)override{arguments=value?value:L"";return S_OK;}
    HRESULT STDMETHODCALLTYPE get_Language(LPWSTR *out)override{return copy(L"zh-CN",out);}
    HRESULT STDMETHODCALLTYPE put_Language(LPCWSTR)override{return S_OK;}
    HRESULT STDMETHODCALLTYPE get_TargetCompatibleBrowserVersion(LPWSTR *out)override{return copy(L"121.0.0.0",out);}
    HRESULT STDMETHODCALLTYPE put_TargetCompatibleBrowserVersion(LPCWSTR)override{return S_OK;}
    HRESULT STDMETHODCALLTYPE get_AllowSingleSignOnUsingOSPrimaryAccount(BOOL *out)override{*out=FALSE;return S_OK;}
    HRESULT STDMETHODCALLTYPE put_AllowSingleSignOnUsingOSPrimaryAccount(BOOL)override{return S_OK;}
};

// 只有内嵌安装页面发送的动作才会进入文件与注册表后端。
static void action(const json &value){
    auto name=value.value("action",std::string());
    if(name=="ready"){auto existing=existingInstallation();send({{"type","init"},{"path",utf8((existing.empty()?known(FOLDERID_ProgramFiles)/L"XiYuan":existing).wstring())},{"existing",!existing.empty()}});}
    else if(name=="inspect_path"&&!testRegistryRoot.empty())send({{"type","folder"},{"path",utf8(installationPath(wide(value.at("path")),value.value("browsing",false)).wstring())}});
    else if(name=="drag"){ReleaseCapture();SendMessageW(windowHandle,WM_NCLBUTTONDOWN,HTCAPTION,0);}
    else if(name=="browse"&&!installing){
        IFileDialog *dialog=nullptr;if(SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog,nullptr,CLSCTX_INPROC_SERVER,IID_IFileDialog,(void**)&dialog))){
            DWORD options;dialog->GetOptions(&options);dialog->SetOptions(options|FOS_PICKFOLDERS|FOS_FORCEFILESYSTEM);dialog->SetTitle(L"选择安装目录");
            if(SUCCEEDED(dialog->Show(windowHandle))){IShellItem *item=nullptr;dialog->GetResult(&item);PWSTR path=nullptr;item->GetDisplayName(SIGDN_FILESYSPATH,&path);send({{"type","folder"},{"path",utf8(installationPath(path,true).wstring())}});CoTaskMemFree(path);item->Release();}dialog->Release();
        }
    }else if(name=="install"&&!installing){if(installThread.joinable())installThread.join();cancelled=false;installing=true;installThread=std::thread(install,value);}
    else if(name=="finish"||name=="close"){
        if(installing){cancelled=true;return;}DestroyWindow(windowHandle);
    }
}

static LRESULT CALLBACK windowProc(HWND handle,UINT message,WPARAM wparam,LPARAM lparam){
    if(message==Notify){std::unique_ptr<json> value(reinterpret_cast<json*>(lparam));if(webview)webview->PostWebMessageAsJson(wide(value->dump()).c_str());return 0;}
    if(message==WM_SIZE&&controller){RECT bounds;GetClientRect(handle,&bounds);controller->put_Bounds(bounds);return 0;}
    if(message==WM_CLOSE){if(webview)send({{"type","closing"}});else DestroyWindow(handle);return 0;}
    if(message==WM_DESTROY){cancelled=true;PostQuitMessage(0);return 0;}
    return DefWindowProcW(handle,message,wparam,lparam);
}

int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR,int){
    // 测试入口与正式安装使用同一套校验解压逻辑，不修改系统关联。
    CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32|LOAD_LIBRARY_SEARCH_USER_DIRS);
    wchar_t path[32768];GetModuleFileNameW(nullptr,path,32768);executable=path;
    int count=0;auto arguments=CommandLineToArgvW(GetCommandLineW(),&count);
    for(int i=1;i<count;++i)if(std::wstring(arguments[i])==L"--test")testRegistryRoot=L"Software\\Xiyuan\\InstallerTests\\"+std::to_wstring(GetCurrentProcessId());
    try{
        if(count>=3&&std::wstring(arguments[1])==L"--extract-only"){
            readPayload();fs::path target=fs::absolute(arguments[2]);checkReparse(target);if(fs::exists(target))return 3;fs::create_directories(target);
            try{extract(target,false);}catch(...){std::error_code ignored;fs::remove_all(target,ignored);throw;}return 0;
        }
        if(count>=3&&std::wstring(arguments[1])==L"--remove-root"){
            if(count>=4&&std::wstring(arguments[3])!=L"--test"){auto parent=OpenProcess(SYNCHRONIZE,FALSE,DWORD(std::stoul(arguments[3])));if(parent){WaitForSingleObject(parent,10000);CloseHandle(parent);}}
            auto result=uninstall(arguments[2]);if(testRegistryRoot.empty())MessageBoxW(nullptr,result?L"卸载未完成":L"析元已卸载",L"析元",MB_OK);return result;
        }
        if(count>=2&&std::wstring(arguments[1])==L"--uninstall"){
            if(MessageBoxW(nullptr,L"卸载析元？用户模型和项目会保留。",L"析元",MB_YESNO|MB_ICONQUESTION)!=IDYES)return 0;
            auto temporary=fs::temp_directory_path()/(L"XiyuanUninstall-"+std::to_wstring(GetCurrentProcessId())+L".exe");fs::copy_file(executable,temporary,fs::copy_options::overwrite_existing);
            auto command=L"\""+temporary.wstring()+L"\" --remove-root \""+executable.parent_path().wstring()+L"\" "+std::to_wstring(GetCurrentProcessId())+(testRegistryRoot.empty()?L"":L" --test");
            STARTUPINFOW startup{sizeof(startup)};PROCESS_INFORMATION process{};require(CreateProcessW(temporary.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&process),"无法启动卸载器");CloseHandle(process.hProcess);CloseHandle(process.hThread);return 0;
        }
        // 临时目录使用进程编号与时间，其他进程不能替换安装器资源。
        sessionDirectory=fs::temp_directory_path()/(L"XiyuanSetup-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));fs::create_directory(sessionDirectory);
        resourceTo(101,sessionDirectory/L"installer.html");resourceTo(102,sessionDirectory/L"WebView2Loader.dll");
        auto loader=LoadLibraryExW((sessionDirectory/L"WebView2Loader.dll").c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);require(loader,"无法启动安装界面");
        using Check=HRESULT(STDMETHODCALLTYPE*)(PCWSTR,LPWSTR*);auto check=(Check)GetProcAddress(loader,"GetAvailableCoreWebView2BrowserVersionString");LPWSTR version=nullptr;
        if(FAILED(check(nullptr,&version))){
            resourceTo(103,sessionDirectory/L"MicrosoftEdgeWebview2Setup.exe");
            auto command=L"\""+(sessionDirectory/L"MicrosoftEdgeWebview2Setup.exe").wstring()+L"\" /silent /install";STARTUPINFOW startup{sizeof(startup)};PROCESS_INFORMATION process{};
            require(CreateProcessW(nullptr,command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&process),"无法安装界面运行库");auto waited=WaitForSingleObject(process.hProcess,180000);CloseHandle(process.hProcess);CloseHandle(process.hThread);require(waited==WAIT_OBJECT_0,"界面运行库安装超时，请检查网络后重试");
            require(SUCCEEDED(check(nullptr,&version)),"界面运行库安装失败，请联网后重试");
        }CoTaskMemFree(version);
        WNDCLASSW cls{};cls.lpfnWndProc=windowProc;cls.hInstance=instance;cls.lpszClassName=L"XiyuanSetup";cls.hIcon=LoadIconW(instance,MAKEINTRESOURCEW(1));cls.hCursor=LoadCursorW(nullptr,IDC_ARROW);cls.hbrBackground=(HBRUSH)GetStockObject(DKGRAY_BRUSH);RegisterClassW(&cls);
        const int dpi=GetDpiForSystem(),width=MulDiv(880,dpi,96),height=MulDiv(601,dpi,96);
        windowHandle=CreateWindowExW(WS_EX_APPWINDOW,cls.lpszClassName,L"析元安装",WS_POPUP|WS_MINIMIZEBOX,(GetSystemMetrics(SM_CXSCREEN)-width)/2,(GetSystemMetrics(SM_CYSCREEN)-height)/2,width,height,nullptr,nullptr,instance,nullptr);require(windowHandle,"无法创建安装窗口");
        ShowWindow(windowHandle,SW_SHOW);
        using Create=HRESULT(STDMETHODCALLTYPE*)(PCWSTR,PCWSTR,ICoreWebView2EnvironmentOptions*,ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler*);
        auto create=(Create)GetProcAddress(loader,"CreateCoreWebView2EnvironmentWithOptions");
        using EnvironmentCallback=Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler,&IID_ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler,HRESULT,ICoreWebView2Environment*>;
        auto environment=new EnvironmentCallback([](HRESULT code,ICoreWebView2Environment *environment)->HRESULT{
            if(FAILED(code)||!environment){MessageBoxW(windowHandle,L"安装界面启动失败",L"析元",MB_OK);DestroyWindow(windowHandle);return S_OK;}
            using ControllerCallback=Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler,&IID_ICoreWebView2CreateCoreWebView2ControllerCompletedHandler,HRESULT,ICoreWebView2Controller*>;
            auto ready=new ControllerCallback([](HRESULT code,ICoreWebView2Controller *value)->HRESULT{
                if(FAILED(code)||!value){DestroyWindow(windowHandle);return S_OK;}controller=value;controller->AddRef();controller->get_CoreWebView2(&webview);
                RECT bounds;GetClientRect(windowHandle,&bounds);controller->put_Bounds(bounds);
                ICoreWebView2Settings *settings=nullptr;webview->get_Settings(&settings);settings->put_AreDevToolsEnabled(FALSE);settings->put_AreDefaultContextMenusEnabled(FALSE);settings->put_IsStatusBarEnabled(FALSE);settings->put_IsZoomControlEnabled(FALSE);settings->Release();
                using MessageCallback=Callback<ICoreWebView2WebMessageReceivedEventHandler,&IID_ICoreWebView2WebMessageReceivedEventHandler,ICoreWebView2*,ICoreWebView2WebMessageReceivedEventArgs*>;
                auto messages=new MessageCallback([](ICoreWebView2*,ICoreWebView2WebMessageReceivedEventArgs *args)->HRESULT{
                    LPWSTR source=nullptr,text=nullptr;args->get_Source(&source);args->get_WebMessageAsJson(&text);
                    try{if(source&&std::wstring(source)==trustedUri&&text)action(json::parse(utf8(text)));}catch(const std::exception &error){send({{"type","error"},{"message",error.what()}});}
                    CoTaskMemFree(source);CoTaskMemFree(text);return S_OK;
                });EventRegistrationToken token;webview->add_WebMessageReceived(messages,&token);messages->Release();
                using NavigationCallback=Callback<ICoreWebView2NavigationStartingEventHandler,&IID_ICoreWebView2NavigationStartingEventHandler,ICoreWebView2*,ICoreWebView2NavigationStartingEventArgs*>;
                auto navigation=new NavigationCallback([](ICoreWebView2*,ICoreWebView2NavigationStartingEventArgs *args)->HRESULT{
                    LPWSTR uri=nullptr;args->get_Uri(&uri);if(!uri||std::wstring(uri)!=trustedUri)args->put_Cancel(TRUE);CoTaskMemFree(uri);return S_OK;
                });webview->add_NavigationStarting(navigation,&token);navigation->Release();
                wchar_t uri[32768];DWORD length=32768;UrlCreateFromPathW((sessionDirectory/L"installer.html").c_str(),uri,&length,0);trustedUri=uri;webview->Navigate(uri);return S_OK;
            });environment->CreateCoreWebView2Controller(windowHandle,ready);ready->Release();return S_OK;
        });
        wchar_t extra[2048]={};if(!testRegistryRoot.empty())GetEnvironmentVariableW(L"XIYUAN_UI_TEST_ARGUMENTS",extra,2048);
        auto options=new EnvironmentOptions(extra);
        require(SUCCEEDED(create(nullptr,(sessionDirectory/L"browser").c_str(),options,environment)),"无法初始化安装界面");options->Release();environment->Release();
        MSG message;while(GetMessageW(&message,nullptr,0,0)>0){TranslateMessage(&message);DispatchMessageW(&message);}
        cancelled=true;if(installThread.joinable())installThread.join();if(controller){controller->Close();controller->Release();}if(webview)webview->Release();
        FreeLibrary(loader);std::error_code ignored;fs::remove_all(sessionDirectory,ignored);
        if(!testRegistryRoot.empty())RegDeleteTreeW(HKEY_CURRENT_USER,testRegistryRoot.c_str());
        CoUninitialize();LocalFree(arguments);return 0;
    }catch(const std::exception &error){if(testRegistryRoot.empty())MessageBoxW(nullptr,wide(error.what()).c_str(),L"析元安装",MB_OK|MB_ICONERROR);return 1;}
}
