#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "save_transfer.hpp"
#include "save_transfer_hash.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __SWITCH__
#include <switch.h>
#endif

namespace nmsaves {
namespace transfer_detail {
namespace {
constexpr size_t MaxEntries = 100000, MaxManifest = 16 * 1024 * 1024;
constexpr uint64_t MaxBytes = 64ULL * 1024 * 1024 * 1024;
struct Item { std::string path, hash; uint64_t size = 0; bool directory = false; };
struct Snapshot { SaveEntry save; std::vector<Item> items; uint64_t bytes = 0; uint32_t files = 0; std::string hash; };
struct Journal { RestorePlan plan; std::string instance, state; unsigned sequence = 0; };
bool equalSave(const SaveEntry& a, const SaveEntry& b) {
    return a.titleId==b.titleId && a.saveDataId==b.saveDataId && a.userLow==b.userLow && a.userHigh==b.userHigh &&
           a.kind==b.kind && a.index==b.index && a.rank==b.rank;
}
bool validSave(const SaveEntry& s) {
    return s.titleId && s.saveDataId && s.rank==0 &&
        ((s.kind==SaveKind::Account && (s.userLow||s.userHigh)) ||
         (s.kind==SaveKind::Device && !s.userLow && !s.userHigh));
}
std::string hashText(const std::string& text) { Sha256 h; h.add(reinterpret_cast<const uint8_t*>(text.data()),text.size()); return h.finish(); }
std::string hex(uint64_t n) { char b[17]; std::snprintf(b,sizeof(b),"%016llx",static_cast<unsigned long long>(n)); return b; }
bool digest(const std::string& s) { return s.size()==64 && s.find_first_not_of("0123456789abcdef")==std::string::npos; }
bool number(const std::string& s, uint64_t& n, unsigned base=10) {
    n=0; if(s.empty()||s.size()>20) return false;
    for(char c:s) { unsigned v=c>='0'&&c<='9'?unsigned(c-'0'):c>='a'&&c<='f'?unsigned(c-'a'+10):99;
        if(v>=base || n>(UINT64_MAX-v)/base) return false;
        n=n*base+v; }
    return true;
}
std::string encode(const std::string& s) { static constexpr char digits[]="0123456789abcdef"; std::string out; out.reserve(s.size()*2); for(unsigned char c:s) { out+=digits[c>>4]; out+=digits[c&15]; } return out; }
bool decode(const std::string& s, std::string& out) {
    out.clear(); if(s.size()%2||s.size()>4096) return false;
    for(size_t i=0;i<s.size();i+=2) { uint64_t b=0; if(!number(s.substr(i,2),b,16)||!b) return false; out+=static_cast<char>(b); }
    return true;
}
bool validRelative(const std::string& s) {
    if(s.empty()||s.size()>768||s.front()=='/'||s.back()=='/'||s.find_first_of("\\:\r\n")!=std::string::npos) return false;
    for(unsigned char c:s) if(c<32||c==127) return false;
    size_t from=0; while(from<s.size()) { auto end=s.find('/',from); auto part=s.substr(from,end==std::string::npos?end:end-from);
        if(part.empty()||part=="."||part=="..") return false;
        if(end==std::string::npos) break;
        from=end+1; }
    return true;
}
bool validId(const std::string& s) { return s.size()==25 && s[16]=='-' && s.substr(0,16).find_first_not_of("0123456789abcdef")==std::string::npos && s.substr(17).find_first_not_of("0123456789abcdef")==std::string::npos; }
bool pathStat(const std::string& path, struct stat& st) { return lstat(path.c_str(),&st)==0; }
bool directory(const std::string& path) { struct stat st{}; return pathStat(path,st)&&S_ISDIR(st.st_mode)&&!S_ISLNK(st.st_mode); }
std::string childPath(const std::string& root,const std::string& relative) { return relative.empty()?root:root+(root.empty()||root.back()!='/'?"/":"")+relative; }
bool safeDirectory(const std::string& path, bool create, std::string& error) {
    if(path.empty()||path.find('\\')!=std::string::npos) { error="Ruta de almacenamiento no válida."; return false; }
    size_t start=path.find(':'); std::string part;
    if(start!=std::string::npos) { if(path.find(':',start+1)!=std::string::npos||path.substr(0,start).find('/')!=std::string::npos) return false; part=path.substr(0,start+1); start++; }
    else start=0;
    if(start<path.size()&&path[start]=='/') { part+='/'; ++start; }
    while(start<path.size()) { auto end=path.find('/',start); const auto component=path.substr(start,end==std::string::npos?end:end-start);
        if(component.empty()||component=="."||component=="..") { error="Ruta de almacenamiento no válida."; return false; }
        if(!part.empty()&&part.back()!='/') part+='/';
        part+=component;
        struct stat st{};
        if(!pathStat(part,st)) { if(errno!=ENOENT||!create||mkdir(part.c_str(),0700)!=0) { error="No se puede abrir la carpeta de transferencias."; return false; } }
        else if(!S_ISDIR(st.st_mode)||S_ISLNK(st.st_mode)) { error="Se rechazó una ruta enlazada o no válida."; return false; }
        if(end==std::string::npos) break;
        start=end+1;
    }
    return directory(path);
}
bool gate(Backend& b, SaveEnvironment mode, std::string& error, const std::string& instance={}) {
    const auto env=b.environment();
    if(env.mode!=mode) { error="Entorno no confirmado. Exporta desde sysMMC con Atmosphere; importa desde emuMMC."; return false; }
    // Network state is informational. The user may export with Wi-Fi enabled
    // or an unknown connection state; this app still never opens the source writable.
    if(!env.gamesClosed) { error="Cierra todos los juegos antes de transferir partidas. "+env.processLabel; return false; }
    if(mode==SaveEnvironment::EmuMMC && (!digest(env.instanceId)||(!instance.empty()&&env.instanceId!=instance))) {
        error="No se pudo identificar esta emuMMC o no es la instancia de destino de la copia."; return false;
    }
    return true;
}
bool existsNow(Backend& b, const SaveEntry& chosen, std::string& error) {
    if(!validSave(chosen)) { error="La identidad o el tipo de partida no es válido."; return false; }
    std::vector<SaveEntry> saves; if(!b.enumerate(saves,error)) return false;
    size_t count=0; for(const auto& s:saves) if(equalSave(chosen,s)) ++count;
    if(count!=1) { error="La partida seleccionada ya no existe o es ambigua. Inicia el juego una vez con el usuario de destino."; return false; }
    return true;
}
class Mount {
    Backend& b_; bool mounted_=false;
public:
    std::string path;
    explicit Mount(Backend& b):b_(b){}
    bool open(const SaveEntry& s,bool writable,std::string& e) { mounted_=b_.mount(s,writable,path,e); return mounted_; }
    void close() { if(mounted_) b_.unmount(); mounted_=false; path.clear(); }
    ~Mount() { close(); }
};
bool check(Backend& b,const char* stage,const std::string& path,std::string& e) {
    if(b.checkpoint(stage,path)) return true;
    e="Operación interrumpida. Se conservan las copias de seguridad."; return false;
}
bool directoryNames(const std::string& path,std::vector<std::string>& names,std::string& error) {
    names.clear();
#ifdef __SWITCH__
    // Direct IPC distinguishes a real read failure from EOF. The POSIX libnx
    // devoptab reports EOF as ENOENT, which must not hide a partial enumeration.
    FsFileSystem* fs=nullptr; char translated[FS_MAX_PATH]{};
    if(fsdevTranslatePath(path.c_str(),&fs,translated)<0||!fs) { error="Ruta de archivos no disponible."; return false; }
    FsDir dir{}; Result rc=fsFsOpenDirectory(fs,translated,FsDirOpenMode_ReadDirs|FsDirOpenMode_ReadFiles,&dir);
    if(R_FAILED(rc)) { error="No se pudo abrir una carpeta de la partida."; return false; }
    std::unique_ptr<FsDirectoryEntry[]> entries(new(std::nothrow) FsDirectoryEntry[16]);
    if(!entries) { fsDirClose(&dir); error="Memoria insuficiente para enumerar la partida."; return false; }
    s64 count=0; bool ok=true;
    do {
        rc=fsDirRead(&dir,&count,16,entries.get());
        if(R_FAILED(rc)||count<0||count>16) { ok=false; break; }
        for(s64 i=0;i<count;++i) {
            const auto length=strnlen(entries[size_t(i)].name,sizeof(entries[size_t(i)].name));
            if(length==0||length==sizeof(entries[size_t(i)].name)) { ok=false; break; }
            names.emplace_back(entries[size_t(i)].name,length);
            if(names.size()>MaxEntries) { ok=false; break; }
        }
    } while(ok&&count>0);
    fsDirClose(&dir);
    if(!ok) error="Enumeración incompleta de una carpeta; no se continuará.";
    return ok;
#else
    DIR* dir=opendir(path.c_str()); if(!dir) { error="No se puede enumerar la carpeta."; return false; }
    errno=0; while(auto* de=readdir(dir)) { std::string n=de->d_name; if(n!="."&&n!="..") names.push_back(n); if(names.size()>MaxEntries) break; }
    const int e=errno; closedir(dir);
    if(e||names.size()>MaxEntries) { error="Enumeración incompleta de la carpeta."; return false; }
    return true;
#endif
}
bool scanItems(const std::string& root,const std::string& relative,std::vector<Item>& items,std::string& error,unsigned depth=0) {
    if(depth>64 || items.size()>MaxEntries) { error="La partida supera los límites de archivos o carpetas."; return false; }
    const auto path=childPath(root,relative);
    if(!directory(path)) { error="Carpeta no válida o enlace rechazado: "+relative; return false; }
    std::vector<std::string> names;
    if(!directoryNames(path,names,error)) return false;
    std::sort(names.begin(),names.end());
    for(const auto& name:names) {
        Item item; item.path=relative.empty()?name:relative+"/"+name;
        if(!validRelative(item.path)) { error="La partida contiene un nombre de archivo no admitido."; return false; }
        struct stat st{}; if(!pathStat(childPath(root,item.path),st)||S_ISLNK(st.st_mode)||(!S_ISDIR(st.st_mode)&&!S_ISREG(st.st_mode))) { error="Se rechazó un enlace o archivo especial de la partida."; return false; }
        item.directory=S_ISDIR(st.st_mode); if(!item.directory) { if(st.st_size<0||uint64_t(st.st_size)>MaxBytes) { error="Archivo de partida demasiado grande."; return false; } item.size=uint64_t(st.st_size); }
        items.push_back(item); if(items.size()>MaxEntries) { error="Demasiados archivos de partida."; return false; }
        if(item.directory&&!scanItems(root,item.path,items,error,depth+1)) return false;
    }
    return true;
}
bool hashFile(Backend& b,const std::string& path,std::string& result,uint64_t& size,std::string& error,
              SaveEnvironment mode=SaveEnvironment::Unknown,const std::string& instance={}) {
    struct stat st{}; if(!pathStat(path,st)||!S_ISREG(st.st_mode)||S_ISLNK(st.st_mode)||st.st_size<0||uint64_t(st.st_size)>MaxBytes) { error="Archivo no válido durante la verificación."; return false; }
    FILE* f=fopen(path.c_str(),"rb"); if(!f) { error="No se pudo leer un archivo de la partida."; return false; }
    Sha256 h; std::array<uint8_t,65536> buffer{}; size=0; size_t got; bool ok=true;
    while((got=fread(buffer.data(),1,buffer.size(),f))) {
        size+=got; if(size>MaxBytes||!check(b,"hash-chunk",path,error) ||
            (mode!=SaveEnvironment::Unknown && (size==got || size%(1024*1024)<got) && !gate(b,mode,error,instance))) { ok=false; break; }
        h.add(buffer.data(),got);
    }
    if(ferror(f)) ok=false;
    fclose(f);
    if(ok && size==uint64_t(st.st_size)) { result=h.finish(); return true; }
    if(error.empty()) error="Lectura incompleta de una partida.";
    return false;
}
std::string saveLine(const SaveEntry& s) {
    return hex(s.titleId)+"|"+hex(s.saveDataId)+"|"+hex(s.userLow)+"|"+hex(s.userHigh)+"|"+
        std::to_string(unsigned(s.kind))+"|"+std::to_string(s.index)+"|"+std::to_string(s.rank)+"|"+encode(s.gameName)+"|"+encode(s.userName);
}
std::vector<std::string> fields(const std::string& s) { std::vector<std::string> out; size_t start=0; while(true) { auto end=s.find('|',start); out.push_back(s.substr(start,end==std::string::npos?end:end-start)); if(end==std::string::npos) return out; start=end+1; } }
bool parseSave(const std::string& text,SaveEntry& s) {
    const auto f=fields(text); uint64_t t=0,i=0,r=0; s={};
    if(f.size()!=9||!number(f[0],s.titleId,16)||!number(f[1],s.saveDataId,16)||!number(f[2],s.userLow,16)||!number(f[3],s.userHigh,16)||
        !number(f[4],t)||!number(f[5],i)||i>65535||!number(f[6],r)||r>255||!decode(f[7],s.gameName)||!decode(f[8],s.userName)||s.gameName.size()>256||s.userName.size()>128) return false;
    s.kind=static_cast<SaveKind>(t); s.index=uint16_t(i); s.rank=uint8_t(r);
    return validSave(s)&&saveLine(s)==text;
}
std::string itemText(const Snapshot& s) {
    std::string text; for(const auto& item:s.items) text+=(item.directory?"D|":"F|"+std::to_string(item.size)+"|"+item.hash+"|")+encode(item.path)+"\n";
    return text;
}
std::string manifest(const Snapshot& s) { return "NMSAVE2\n"+saveLine(s.save)+"\n"+itemText(s)+"END|"+std::to_string(s.files)+"|"+std::to_string(s.bytes)+"\n"; }
bool readSmall(const std::string& path,std::string& out,size_t limit,std::string& error) {
    out.clear(); struct stat st{}; if(!pathStat(path,st)||!S_ISREG(st.st_mode)||S_ISLNK(st.st_mode)||st.st_size<0||uint64_t(st.st_size)>limit) { error="Falta un manifiesto válido o excede su límite."; return false; }
    FILE* f=fopen(path.c_str(),"rb"); if(!f) return false; std::array<char,4096> b{}; size_t n; bool ok=true;
    while((n=fread(b.data(),1,b.size(),f))) { if(out.size()+n>limit) { ok=false; break; } out.append(b.data(),n); }
    if(ferror(f)) ok=false;
    fclose(f); return ok;
}
bool parseManifest(const std::string& text,Snapshot& s) {
    s={}; std::istringstream in(text); std::string line;
    if(!std::getline(in,line)||line!="NMSAVE2"||!std::getline(in,line)||!parseSave(line,s.save)) return false;
    std::set<std::string> paths; bool end=false;
    while(std::getline(in,line)) {
        auto f=fields(line); if(f.empty()||end) return false;
        if(f[0]=="END") { uint64_t files=0,total=0; if(f.size()!=3||!number(f[1],files)||!number(f[2],total)||files!=s.files||total!=s.bytes) return false; end=true; continue; }
        Item item;
        if(f[0]=="D"&&f.size()==2) { item.directory=true; if(!decode(f[1],item.path)) return false; }
        else if(f[0]=="F"&&f.size()==4) { if(!number(f[1],item.size)||item.size>MaxBytes||!digest(f[2])||!decode(f[3],item.path)) return false; item.hash=f[2]; s.bytes+=item.size; ++s.files; if(s.bytes>MaxBytes) return false; }
        else return false;
        if(!validRelative(item.path)||!paths.insert(item.path).second||s.items.size()>=MaxEntries) return false;
        s.items.push_back(std::move(item));
    }
    if(!end||manifest(s)!=text) return false;
    s.hash=hashText(text); return true;
}
bool snapshot(Backend& b,const std::string& root,const SaveEntry& save,Snapshot& out,std::string& error,
              SaveEnvironment mode=SaveEnvironment::Unknown,const std::string& instance={}) {
    out={}; out.save=save;
    if(!scanItems(root,"",out.items,error)) return false;
    for(auto& item:out.items) if(!item.directory) {
        uint64_t size=0; if(!hashFile(b,childPath(root,item.path),item.hash,size,error,mode,instance)||size!=item.size) { if(error.empty()) error="La partida cambió durante la lectura."; return false; }
        out.bytes+=size; ++out.files; if(out.bytes>MaxBytes) { error="La partida supera el tamaño admitido."; return false; }
    }
    out.hash=hashText(manifest(out)); return true;
}
bool sameContents(const Snapshot& a,const Snapshot& b) { return a.files==b.files&&a.bytes==b.bytes&&itemText(a)==itemText(b); }
std::string fileFailure(const char* operation,const std::string& path,uint64_t offset=0) {
    // Capture before fclose/unmount/another FS call can replace thread-local
    // fsdevGetLastResult. Native POSIX errno alone loses most FS error details.
    const int savedErrno=errno;
#ifdef __SWITCH__
    const auto rc=fsdevGetLastResult();
    char code[24]; std::snprintf(code,sizeof(code),"0x%08lx",static_cast<unsigned long>(rc));
#endif
    std::string result=std::string(operation)+": "+path+" (byte "+std::to_string(offset)+", errno "+std::to_string(savedErrno);
#ifdef __SWITCH__
    result+=", FS "; result+=code;
#endif
    return result+"). Se conserva la copia de seguridad.";
}
bool closeWrittenFile(FILE*& f,const std::string& path,uint64_t offset,std::string& error) {
    bool ok=true;
    if(fflush(f)!=0) { if(error.empty()) error=fileFailure("No se pudo vaciar el archivo",path,offset); ok=false; }
    if(ok&&fsync(fileno(f))!=0) { if(error.empty()) error=fileFailure("No se pudo confirmar la escritura del archivo",path,offset); ok=false; }
    if(fclose(f)!=0) { if(error.empty()) error=fileFailure("No se pudo cerrar el archivo",path,offset); ok=false; }
    f=nullptr; return ok;
}
bool flushClose(FILE* f) { bool ok=fflush(f)==0; if(ok&&fsync(fileno(f))!=0) ok=false; if(fclose(f)!=0) ok=false; return ok; }
bool writeExclusive(Backend& b,const std::string& path,const std::string& data,std::string& error) {
    if(!check(b,"write-metadata",path,error)) return false;
    int fd=open(path.c_str(),O_WRONLY|O_CREAT|O_EXCL,0600); if(fd<0) { error="No se puede crear un registro exclusivo en la SD."; return false; }
    FILE* f=fdopen(fd,"wb"); if(!f) { close(fd); return false; }
    bool ok=fwrite(data.data(),1,data.size(),f)==data.size(); if(!flushClose(f)) ok=false;
    if(!ok) error="No se pudo guardar el registro completo en la SD.";
    return ok;
}
bool copyFile(Backend& b,const std::string& from,const std::string& to,std::string& error,SaveEnvironment mode,const std::string& instance) {
    struct stat st{}; if(!pathStat(from,st)||!S_ISREG(st.st_mode)||S_ISLNK(st.st_mode)) { error="Archivo de origen no válido."; return false; }
    FILE* in=fopen(from.c_str(),"rb"); if(!in) return false;
    int fd=open(to.c_str(),O_WRONLY|O_CREAT|O_EXCL,0600); if(fd<0) { fclose(in); error="No se pudo crear un archivo de destino; comprueba el espacio disponible."; return false; }
    FILE* out=fdopen(fd,"wb"); if(!out) { close(fd); fclose(in); return false; }
    std::array<uint8_t,65536> buffer{}; size_t n; uint64_t total=0; bool ok=true;
    while((n=fread(buffer.data(),1,buffer.size(),in))) {
        total+=n;
        if(total>MaxBytes||!check(b,"copy-chunk",to,error)||(total==n||total%(1024*1024)<n ? !gate(b,mode,error,instance):false)||fwrite(buffer.data(),1,n,out)!=n) { ok=false; break; }
    }
    if(ferror(in)||total!=uint64_t(st.st_size)) ok=false;
    fclose(in); if(!flushClose(out)) ok=false;
    if(!ok&&error.empty()) error="La copia no terminó. Se conserva la copia de seguridad.";
    return ok;
}
bool copyItems(Backend& b,const std::string& from,const std::string& to,const Snapshot& data,std::string& error,SaveEnvironment mode,const std::string& instance={}) {
    if(!directory(from)||!directory(to)) return false;
    for(const auto& item:data.items) {
        if(!check(b,"copy-entry",item.path,error)||!gate(b,mode,error,instance)) return false;
        const auto dest=childPath(to,item.path);
        if(item.directory) { if(mkdir(dest.c_str(),0700)!=0) { error="No se pudo crear una carpeta de la partida."; return false; } }
        else if(!copyFile(b,childPath(from,item.path),dest,error,mode,instance)) return false;
    }
    return true;
}
bool commitDestination(Backend& b,const std::string& instance,std::string& error) {
    return gate(b,SaveEnvironment::EmuMMC,error,instance)&&b.commitSave(error)&&check(b,"save-committed","",error);
}
bool copySaveFile(Backend& b,const std::string& from,const std::string& to,uint64_t expected,
                  uint64_t budget,const std::string& instance,std::string& error) {
    struct stat st{};
    if(!pathStat(from,st)||!S_ISREG(st.st_mode)||S_ISLNK(st.st_mode)||st.st_size<0||uint64_t(st.st_size)!=expected) { error="El archivo de la copia cambió antes de restaurarlo."; return false; }
    FILE* in=fopen(from.c_str(),"rb"); if(!in) { error=fileFailure("No se pudo abrir la copia",from); return false; }
    int fd=open(to.c_str(),O_WRONLY|O_CREAT|O_EXCL,0600);
    if(fd<0) { error=fileFailure("No se pudo crear el archivo de destino",to); fclose(in); return false; }
    FILE* out=fdopen(fd,"wb");
    if(!out) { error=fileFailure("No se pudo preparar el archivo de destino",to); close(fd); fclose(in); return false; }
    std::array<uint8_t,65536> buffer{}; uint64_t total=0,uncommitted=0; bool ok=true;
    do {
        while(total<expected&&uncommitted<budget) {
            const auto n=static_cast<size_t>(std::min<uint64_t>(buffer.size(),std::min(expected-total,budget-uncommitted)));
            const auto got=fread(buffer.data(),1,n,in);
            if(got!=n) { error=ferror(in)?fileFailure("No se pudo leer la copia",from,total):"La copia terminó antes de lo esperado."; ok=false; break; }
            if(!gate(b,SaveEnvironment::EmuMMC,error,instance)||!check(b,"copy-chunk",to,error)||!check(b,"save-write",std::to_string(n),error)) { ok=false; break; }
            if(fwrite(buffer.data(),1,n,out)!=n) { error=fileFailure("No se pudo escribir el archivo de destino",to,total); ok=false; break; }
            total+=n; uncommitted+=n;
        }
        if(ok&&total==expected) {
            const int extra=fgetc(in);
            if(extra!=EOF||ferror(in)) { error=ferror(in)?fileFailure("No se pudo terminar de leer la copia",from,total):"La copia cambió de tamaño durante la restauración."; ok=false; }
        }
        // No save commit with a writable file handle open. fsync only calls
        // fsFileFlush on Switch; the explicit filesystem commit frees journal.
        if(!closeWrittenFile(out,to,total,error)) ok=false;
        if(!ok||!commitDestination(b,instance,error)) { ok=false; break; }
        uncommitted=0;
        if(total==expected) break;
        if(!pathStat(to,st)||!S_ISREG(st.st_mode)||S_ISLNK(st.st_mode)||st.st_size<0||uint64_t(st.st_size)!=total) { error="El archivo de destino cambió entre tramos confirmados."; ok=false; break; }
        fd=open(to.c_str(),O_WRONLY); // Existing file: never create, truncate or append.
        if(fd<0) { error=fileFailure("No se pudo reabrir el archivo de destino",to,total); ok=false; break; }
        if(lseek(fd,static_cast<off_t>(total),SEEK_SET)!=static_cast<off_t>(total)) { error=fileFailure("No se pudo continuar en la posición confirmada",to,total); close(fd); ok=false; break; }
        out=fdopen(fd,"wb"); // fdopen does not truncate an already-open descriptor.
        if(!out) { error=fileFailure("No se pudo continuar el archivo de destino",to,total); close(fd); ok=false; break; }
    } while(ok);
    if(out) { std::string cleanup; closeWrittenFile(out,to,total,cleanup); }
    fclose(in); return ok;
}
bool copySaveItems(Backend& b,const std::string& from,const std::string& to,const Snapshot& data,
                   uint64_t budget,const std::string& instance,std::string& error) {
    if(!directory(from)||!directory(to)) { error="La copia o el destino no están disponibles."; return false; }
    for(const auto& item:data.items) {
        if(!check(b,"copy-entry",item.path,error)||!gate(b,SaveEnvironment::EmuMMC,error,instance)) return false;
        const auto dest=childPath(to,item.path);
        if(item.directory) {
            if(!check(b,"save-metadata",item.path,error)) return false;
            if(mkdir(dest.c_str(),0700)!=0) { error=fileFailure("No se pudo crear una carpeta de destino",dest); return false; }
            if(!commitDestination(b,instance,error)) return false;
        } else if(!copySaveFile(b,childPath(from,item.path),dest,item.size,budget,instance,error)) return false;
    }
    return true;
}
bool clearContents(Backend& b,const std::string& root,std::string& error,const std::string& instance) {
    std::vector<Item> items; if(!scanItems(root,"",items,error)) return false;
    for(auto it=items.rbegin();it!=items.rend();++it) {
        if(!gate(b,SaveEnvironment::EmuMMC,error,instance)||!check(b,"remove-entry",it->path,error)) return false;
        const auto path=childPath(root,it->path);
        if(!check(b,"save-metadata",it->path,error)) return false;
        if((it->directory?rmdir(path.c_str()):unlink(path.c_str()))!=0) { error=fileFailure("No se pudo sustituir un archivo del destino",path); return false; }
        if(!commitDestination(b,instance,error)) return false;
    }
    return true;
}
bool makeRoot(const std::string& root,std::string& error) {
    return safeDirectory(root,true,error)&&safeDirectory(root+"/exports",true,error)&&safeDirectory(root+"/backups",true,error)&&safeDirectory(root+"/transactions",true,error);
}
std::string makeId() {
    static uint32_t counter=0; uint64_t now=uint64_t(std::chrono::system_clock::now().time_since_epoch().count());
    char tail[10]; std::snprintf(tail,sizeof(tail),"-%08x",++counter); return hex(now)+tail;
}
bool newDirectory(const std::string& parent,std::string& id,std::string& error,const std::string& suffix={}) {
    for(unsigned i=0;i<64;++i) { id=makeId(); if(mkdir((parent+"/"+id+suffix).c_str(),0700)==0) return true; if(errno!=EEXIST) break; }
    error="No se pudo reservar una carpeta exclusiva en la SD."; return false;
}
bool readSnapshot(Backend& b,const std::string& parent,const std::string& id,Snapshot& s,std::string& error,bool verify=true) {
    if(!validId(id)||!safeDirectory(parent+"/"+id,false,error)) { error="Identificador de copia no válido."; return false; }
    const auto dir=parent+"/"+id; std::string text,complete;
    if(!readSmall(dir+"/manifest.txt",text,MaxManifest,error)||!parseManifest(text,s)||!readSmall(dir+"/complete.sha256",complete,65,error)||complete!=s.hash+"\n") { error="Copia incompleta o manifiesto dañado; no se usará."; return false; }
    if(verify) { Snapshot actual; if(!snapshot(b,dir+"/data",s.save,actual,error)||!sameContents(s,actual)) { error="La copia no supera la verificación SHA-256 de todos sus archivos."; return false; } }
    return true;
}
bool createSnapshot(Backend& b,const std::string& parent,const std::string& from,const SaveEntry& save,std::string& id,Snapshot& recorded,std::string& error,SaveEnvironment mode,const std::string& instance={}) {
    if(!newDirectory(parent,id,error,".partial")) return false;
    const auto stage=parent+"/"+id+".partial";
    if(mkdir((stage+"/data").c_str(),0700)!=0) { error="No se pudo crear la carpeta de datos."; return false; }
    Snapshot before,after,copied;
    if(!snapshot(b,from,save,before,error,mode,instance)||!copyItems(b,from,stage+"/data",before,error,mode,instance)||
        !snapshot(b,from,save,after,error,mode,instance)||!sameContents(before,after)||
        !snapshot(b,stage+"/data",save,copied,error)||!sameContents(before,copied)) { if(error.empty()) error="La partida cambió durante la exportación. La copia parcial no se puede importar."; return false; }
    const auto text=manifest(copied);
    if(text.size()>MaxManifest||!writeExclusive(b,stage+"/manifest.txt",text,error)||!writeExclusive(b,stage+"/complete.sha256",copied.hash+"\n",error)||!b.commitSd(error)||!gate(b,mode,error,instance)) return false;
    if(!check(b,"complete-snapshot",stage,error)||rename(stage.c_str(),(parent+"/"+id).c_str())!=0||!b.commitSd(error)) { if(error.empty()) error="No se pudo completar atómicamente la copia en la SD."; return false; }
    if(!readSnapshot(b,parent,id,recorded,error)) return false;
    return true;
}
ExportEntry exportEntry(const std::string& id,const Snapshot& s) { ExportEntry e; e.id=id; e.source=s.save; e.bytes=s.bytes; e.files=s.files; e.manifestSha256=s.hash; return e; }
bool listIds(const std::string& parent,std::vector<std::string>& out,std::string& error) {
    out.clear(); struct stat st{}; if(!pathStat(parent,st)&&errno==ENOENT) return true;
    if(!safeDirectory(parent,false,error)) return false;
    std::vector<std::string> names; if(!directoryNames(parent,names,error)) return false;
    for(const auto& name:names) if(validId(name)) out.push_back(name);
    std::sort(out.begin(),out.end()); return true;
}
std::string journalText(const Journal& j) {
    return "NMTRANSFER2\n"+j.plan.id+"\n"+std::to_string(j.sequence)+"\n"+j.state+"\n"+j.instance+"\n"+
        j.plan.exported.id+"\n"+j.plan.exported.manifestSha256+"\n"+saveLine(j.plan.exported.source)+"\n"+
        saveLine(j.plan.destination)+"\n"+j.plan.backupId+"\n"+j.plan.backupSha256+"\n"+std::to_string(j.plan.backupBytes)+"\n";
}
bool parseJournal(const std::string& text,Journal& j) {
    std::istringstream in(text); std::vector<std::string> lines; std::string line; while(std::getline(in,line)) lines.push_back(line);
    if(lines.size()!=13||lines[0]!="NMTRANSFER2") return false;
    const auto cut=text.rfind(lines[12]+"\n"); if(cut==std::string::npos||!digest(lines[12])||hashText(text.substr(0,cut))!=lines[12]) return false;
    uint64_t seq=0; j={}; j.plan.id=lines[1]; j.state=lines[3]; j.instance=lines[4]; j.plan.exported.id=lines[5]; j.plan.exported.manifestSha256=lines[6]; j.plan.backupId=lines[9]; j.plan.backupSha256=lines[10];
    if(!validId(j.plan.id)||!number(lines[2],seq)||seq>100000||!digest(j.instance)||!validId(j.plan.exported.id)||!digest(j.plan.exported.manifestSha256)||
       !parseSave(lines[7],j.plan.exported.source)||!parseSave(lines[8],j.plan.destination)||!validId(j.plan.backupId)||!digest(j.plan.backupSha256)||
       !number(lines[11],j.plan.backupBytes)||!compatibleTransferTarget(j.plan.exported.source,j.plan.destination)) return false;
    if(j.state!="prepared"&&j.state!="applying"&&j.state!="applied"&&j.state!="rolling-back"&&j.state!="restored"&&j.state!="recovery-required") return false;
    j.sequence=unsigned(seq); j.plan.backupVerified=true;
    return journalText(j)+hashText(journalText(j))+"\n"==text;
}
std::string recordName(unsigned seq) { char out[20]; std::snprintf(out,sizeof(out),"%08u.record",seq); return out; }
bool appendJournal(Backend& b,const std::string& root,Journal& j,const std::string& state,std::string& error) {
    Journal next=j; next.state=state; if(!j.state.empty()) ++next.sequence;
    const auto dir=root+"/transactions/"+j.plan.id; const auto path=dir+"/"+recordName(next.sequence);
    const auto text=journalText(next); const auto temp=path+".partial-"+makeId();
    struct stat st{};
    if(pathStat(path,st)||errno!=ENOENT) { error="El diario ya cambió; vuelve a consultar la recuperación."; return false; }
    if(!check(b,"journal",state,error)||!writeExclusive(b,temp,text+hashText(text)+"\n",error)||!b.commitSd(error)||rename(temp.c_str(),path.c_str())!=0) { if(error.empty()) error="No se pudo guardar el diario de recuperación."; return false; }
    // Once renamed, do not reuse this sequence, even if the storage flush fails.
    j=next; return b.commitSd(error);
}
bool readJournal(const std::string& root,const std::string& id,Journal& j,std::string& error) {
    if(!validId(id)||!safeDirectory(root+"/transactions/"+id,false,error)) return false;
    const auto dir=root+"/transactions/"+id; bool found=false;
    std::vector<std::string> names,records; if(!directoryNames(dir,names,error)) return false;
    for(const auto& name:names) if(name.size()>=7&&name.substr(name.size()-7)==".record") records.push_back(name);
    std::sort(records.begin(),records.end());
    for(unsigned seq=0;seq<records.size();++seq) {
        if(records[seq]!=recordName(seq)) { error="El diario tiene registros fuera de secuencia. No se modificará ninguna partida."; return false; }
        const auto path=dir+"/"+records[seq];
        std::string text; Journal next;
        if(!readSmall(path,text,8192,error)||!parseJournal(text,next)||next.plan.id!=id||next.sequence!=seq||
            (found&&(next.instance!=j.instance||next.plan.backupId!=j.plan.backupId||next.plan.backupSha256!=j.plan.backupSha256||
              !equalSave(next.plan.destination,j.plan.destination)||!equalSave(next.plan.exported.source,j.plan.exported.source)||next.plan.exported.id!=j.plan.exported.id||next.plan.exported.manifestSha256!=j.plan.exported.manifestSha256))) {
            error="Diario de recuperación dañado. No se modificará ninguna partida."; return false;
        }
        bool transition=false;
        if(!found) transition=next.state=="prepared";
        else if(j.state=="prepared") transition=next.state=="applying";
        else if(j.state=="applying") transition=next.state=="applied"||next.state=="rolling-back"||next.state=="recovery-required";
        else if(j.state=="applied") transition=next.state=="rolling-back";
        else if(j.state=="rolling-back"||j.state=="recovery-required") transition=next.state=="rolling-back"||next.state=="restored"||next.state=="recovery-required";
        if(!transition) { error="Transición de recuperación no válida. No se modificará ninguna partida."; return false; }
        j=next; found=true;
    }
    if(!found) error="No hay un diario completo para esa operación.";
    return found;
}
bool incompletePreparation(const std::string& root,const std::string& id) {
    if(!validId(id)) return false;
    const auto path=root+"/transactions/"+id;
    std::string error; if(!safeDirectory(path,false,error)) return false;
    std::vector<std::string> names; if(!directoryNames(path,names,error)) return false;
    bool hasRecord=false;
    for(const auto& name:names) if(name.size()>=7&&name.substr(name.size()-7)==".record") { hasRecord=true; break; }
    // Before the first complete record, no destination mutation is authorized.
    // Keep all incomplete preparation artifacts, but do not block other saves.
    return !hasRecord;
}
bool pendingForSave(const std::string& root,const SaveEntry& target,std::string& error) {
    std::vector<std::string> ids; if(!listIds(root+"/transactions",ids,error)) return true;
    for(const auto& id:ids) {
        if(incompletePreparation(root,id)) continue;
        Journal j; std::string e; if(!readJournal(root,id,j,e)) { error=e; return true; }
        if(equalSave(target,j.plan.destination) && (j.state=="applying"||j.state=="rolling-back"||j.state=="recovery-required")) { error="Esta partida tiene una importación pendiente de recuperar. Usa Recuperar antes de volver a importar."; return true; }
    }
    return false;
}
TransferResult failure(const std::string& error,const std::string& id={},bool recovery=false) { TransferResult r; r.message=error.empty()?"No se pudo completar la operación; no continúes hasta revisar el resultado.":error; r.id=id; r.recoveryRequired=recovery; return r; }
bool replaceFrom(Backend& b,const std::string& dest,const std::string& source,const Snapshot& data,const SaveEntry& target,
                 uint64_t budget,const std::string& instance,std::string& error) {
    if(!budget) { error="No se ha comprobado un presupuesto de escritura para la partida."; return false; }
    if(!clearContents(b,dest,error,instance)||!copySaveItems(b,source,dest,data,budget,instance,error)) return false;
    Snapshot actual; if(!snapshot(b,dest,target,actual,error,SaveEnvironment::EmuMMC,instance)||!sameContents(data,actual)) { if(error.empty()) error="La partida restaurada no coincide con la copia."; return false; }
    if(!gate(b,SaveEnvironment::EmuMMC,error,instance)||!b.commitSave(error)) return false;
    Snapshot committed;
    if(!snapshot(b,dest,target,committed,error,SaveEnvironment::EmuMMC,instance)||!sameContents(data,committed)) { if(error.empty()) error="La lectura posterior al commit no coincide con la copia."; return false; }
    return true;
}
bool restoreBackup(Backend& b,const std::string& root,Journal& journal,const std::string& destination,std::string& error) {
    Snapshot backup;
    if(!readSnapshot(b,root+"/backups",journal.plan.backupId,backup,error)||backup.hash!=journal.plan.backupSha256||!equalSave(backup.save,journal.plan.destination)) { error="La copia de seguridad no se pudo verificar. No se intentará restaurarla."; return false; }
    // Caller always supplies a fresh mount. Closing a failed transaction may
    // already have discarded all uncommitted changes; don't rewrite a matching
    // (possibly empty) backup just to report that recovery succeeded.
    Snapshot current;
    if(!snapshot(b,destination,journal.plan.destination,current,error,SaveEnvironment::EmuMMC,journal.instance)) return false;
    if(sameContents(current,backup)) {
        return appendJournal(b,root,journal,"rolling-back",error)&&appendJournal(b,root,journal,"restored",error);
    }
    uint64_t capacity=0,budget=0;
    if(!b.saveCapacity(capacity,error)||backup.bytes>capacity||!b.saveWriteBudget(budget,error)) {
        if(error.empty()) error="La partida de destino ya no tiene capacidad comprobada para recuperar la copia.";
        return false;
    }
    if(!appendJournal(b,root,journal,"rolling-back",error)) return false;
    if(!replaceFrom(b,destination,root+"/backups/"+journal.plan.backupId+"/data",backup,journal.plan.destination,budget,journal.instance,error)) return false;
    return appendJournal(b,root,journal,"restored",error);
}
} // namespace

bool validatedSaveCapacity(const SaveEntry& mounted,const SaveCapacityMetadata& metadata,
                           uint64_t& bytes,std::string& error) {
    bytes=0; error.clear();
    if(!validSave(mounted)||metadata.systemSaveDataId||
       metadata.titleId!=mounted.titleId||metadata.userLow!=mounted.userLow||metadata.userHigh!=mounted.userHigh||
       metadata.kind!=mounted.kind||metadata.index!=mounted.index||metadata.rank!=mounted.rank) {
        error="Los datos de capacidad no corresponden al juego, usuario y tipo de la partida de destino.";
        return false;
    }
    if(metadata.dataSize<=0||metadata.journalSize<0) {
        error="La capacidad útil de la partida de destino no es válida. No se modificará la partida.";
        return false;
    }
    // No addition: the journal and the container overhead cannot hold game
    // files. The response is re-read before applying, never cached in a plan.
    bytes=static_cast<uint64_t>(metadata.dataSize);
    return true;
}
bool validatedSaveWriteBudget(const SaveEntry& mounted,const SaveCapacityMetadata& metadata,
                              uint64_t& bytes,std::string& error) {
    bytes=0; uint64_t capacity=0;
    if(!validatedSaveCapacity(mounted,metadata,capacity,error)) return false;
    const uint64_t journal=static_cast<uint64_t>(metadata.journalSize);
    // Conservative policy, not a claim about currently free journal blocks.
    // Leave space for metadata/copy-on-write and never write more than 1 MiB
    // between closed-file commits. Small/unknown journals are not guessed.
    const uint64_t reserve=std::max<uint64_t>(64*1024,std::min<uint64_t>(1024*1024,journal/4));
    if(journal<=reserve) { error="El journal de esta partida es nulo o demasiado pequeño para el método de escritura comprobado. No se modificará."; return false; }
    const uint64_t budget=(std::min<uint64_t>(1024*1024,(journal-reserve)/2)/(16*1024))*(16*1024);
    if(budget<64*1024) { error="El journal de esta partida no permite el presupuesto mínimo de escritura. No se modificará."; return false; }
    bytes=budget; return true;
}

std::string destinationConfigFingerprint(uint64_t device,const std::string& text,std::string& profilePath) {
    profilePath.clear(); if(!device||text.size()>8192||text.find('\0')!=std::string::npos) return {};
    auto trim=[](const std::string& s) { auto a=s.find_first_not_of(" \t\r"); if(a==std::string::npos) return std::string{}; auto z=s.find_last_not_of(" \t\r"); return s.substr(a,z-a+1); };
    std::istringstream in(text); std::string line,section; std::map<std::string,std::string> values;
    while(std::getline(in,line)) {
        line=trim(line); if(line.empty()||line[0]==';'||line[0]=='#') continue;
        if(line.front()=='['&&line.back()==']') { section=line.substr(1,line.size()-2); continue; }
        if(section!="emummc") continue;
        const auto p=line.find('='); if(p==std::string::npos) return {};
        auto key=trim(line.substr(0,p)),value=trim(line.substr(p+1));
        if(key=="enabled"||key=="sector"||key=="path"||key=="id") { if(!values.emplace(key,value).second) return {}; }
    }
    if(values["enabled"]!="1"||!validRelative(values["path"])) return {};
    auto numeric=[](std::string v,uint64_t& n) { unsigned base=10; if(v.rfind("0x",0)==0||v.rfind("0X",0)==0) { v.erase(0,2); base=16; } std::transform(v.begin(),v.end(),v.begin(),[](char c){return c>='A'&&c<='F'?char(c-'A'+'a'):c;}); return number(v,n,base); };
    // Hekate's emummc_load_cfg defaults both fields to zero before parsing.
    uint64_t sector=0,id=0;
    if((values.count("sector")&&!numeric(values["sector"],sector))||(values.count("id")&&!numeric(values["id"],id))) return {};
    profilePath=values["path"];
    return hashText("NMSAVES-DESTINATION-1|"+hex(device)+"|"+std::to_string(sector)+"|"+std::to_string(id)+"|"+profilePath);
}

Engine::Engine(Backend& backend,std::string root):backend_(backend),root_(std::move(root)) {}
bool Engine::listSaves(std::vector<SaveEntry>& saves,std::string& error) {
    saves.clear(); error.clear(); const auto env=backend_.environment();
    if(env.mode==SaveEnvironment::Unknown) { error="Entorno activo desconocido; no se enumerarán partidas."; return false; }
    if(!gate(backend_,env.mode,error)) return false;
    return backend_.enumerate(saves,error);
}
bool Engine::listExports(std::vector<ExportEntry>& out,std::string& error) {
    out.clear(); error.clear(); std::vector<std::string> ids;
    if(!listIds(root_+"/exports",ids,error)) return false;
    unsigned invalid=0; for(const auto& id:ids) { Snapshot s; std::string e; if(readSnapshot(backend_,root_+"/exports",id,s,e)) out.push_back(exportEntry(id,s)); else ++invalid; }
    if(invalid) error=std::to_string(invalid)+" copia(s) incompleta(s) o dañada(s) excluida(s).";
    return true;
}
TransferResult Engine::exportSave(const SaveEntry& source) {
    std::string e,id; if(!gate(backend_,SaveEnvironment::SysMMC,e)||!existsNow(backend_,source,e)||!makeRoot(root_,e)) return failure(e);
    Mount mount(backend_); if(!mount.open(source,false,e)) return failure(e);
    Snapshot saved;
    if(!createSnapshot(backend_,root_+"/exports",mount.path,source,id,saved,e,SaveEnvironment::SysMMC)) return failure(e,id);
    TransferResult result; result.ok=true; result.id=id; result.message="Partida exportada y verificada. El original se abrió en solo lectura. Reinicia en emuMMC para importarla."; return result;
}
TransferResult Engine::prepareRestore(const ExportEntry& exported,const SaveEntry& target,RestorePlan& plan) {
    plan={}; std::string e; if(!gate(backend_,SaveEnvironment::EmuMMC,e)||!compatibleTransferTarget(exported.source,target)||!existsNow(backend_,target,e)||!makeRoot(root_,e)) return failure(e.empty()?"El juego, tipo o índice de la partida no coincide con el destino.":e);
    if(pendingForSave(root_,target,e)) return failure(e);
    Snapshot source; if(!readSnapshot(backend_,root_+"/exports",exported.id,source,e)||source.hash!=exported.manifestSha256||!equalSave(source.save,exported.source)) return failure(e.empty()?"La exportación cambió desde que fue seleccionada.":e);
    const auto env=backend_.environment();
    Mount mount(backend_); if(!mount.open(target,false,e)) return failure(e);
    uint64_t capacity=0,budget=0;
    if(!backend_.saveCapacity(capacity,e)||source.bytes>capacity) return failure(e.empty()?"Los datos exportados superan la capacidad de la partida de destino. No se creará ni ampliará una partida automáticamente.":e);
    if(!backend_.saveWriteBudget(budget,e)) return failure(e);
    std::string backupId; Snapshot backup;
    if(!createSnapshot(backend_,root_+"/backups",mount.path,target,backupId,backup,e,SaveEnvironment::EmuMMC,env.instanceId)) return failure(e);
    Journal j; j.instance=env.instanceId; j.plan.exported=exportEntry(exported.id,source); j.plan.destination=target; j.plan.backupId=backupId; j.plan.backupSha256=backup.hash; j.plan.backupBytes=backup.bytes; j.plan.backupVerified=true;
    if(!newDirectory(root_+"/transactions",j.plan.id,e)||!appendJournal(backend_,root_,j,"prepared",e)) return failure(e);
    plan=j.plan; TransferResult result; result.ok=true; result.id=plan.id; result.message="Copia del destino verificada. Revisa el juego y el usuario de destino y confirma para reemplazar su partida."; return result;
}
TransferResult Engine::applyRestore(const RestorePlan& plan) {
    std::string e; Journal j;
    if(!readJournal(root_,plan.id,j,e)) return failure(e);
    if(j.state!="prepared"||!plan.backupVerified||!equalSave(plan.destination,j.plan.destination)||!equalSave(plan.exported.source,j.plan.exported.source)||
       plan.exported.id!=j.plan.exported.id||plan.exported.manifestSha256!=j.plan.exported.manifestSha256||plan.backupId!=j.plan.backupId||plan.backupSha256!=j.plan.backupSha256) return failure("La confirmación no corresponde a una preparación pendiente y verificada.");
    if(!gate(backend_,SaveEnvironment::EmuMMC,e,j.instance)||!existsNow(backend_,j.plan.destination,e)||pendingForSave(root_,j.plan.destination,e)) return failure(e);
    Snapshot source,backup;
    if(!readSnapshot(backend_,root_+"/exports",j.plan.exported.id,source,e)||source.hash!=j.plan.exported.manifestSha256||!equalSave(source.save,j.plan.exported.source)||
       !readSnapshot(backend_,root_+"/backups",j.plan.backupId,backup,e)||backup.hash!=j.plan.backupSha256||!equalSave(backup.save,j.plan.destination)) return failure(e.empty()?"La exportación o la copia previa cambió; no se importará.":e);
    Mount mount(backend_); if(!mount.open(j.plan.destination,true,e)) return failure(e);
    uint64_t capacity=0,budget=0;
    if(!backend_.saveCapacity(capacity,e)||source.bytes>capacity) return failure(e.empty()?"La capacidad del destino ya no admite la partida exportada.":e);
    if(!backend_.saveWriteBudget(budget,e)) return failure(e);
    Snapshot current; if(!snapshot(backend_,mount.path,j.plan.destination,current,e,SaveEnvironment::EmuMMC,j.instance)||!sameContents(current,backup)) return failure(e.empty()?"La partida de destino cambió después de preparar la copia. Vuelve a preparar la importación.":e);
    if(!appendJournal(backend_,root_,j,"applying",e)) return failure(e,j.plan.id);
    if(replaceFrom(backend_,mount.path,root_+"/exports/"+j.plan.exported.id+"/data",source,j.plan.destination,budget,j.instance,e)) {
        if(!appendJournal(backend_,root_,j,"applied",e)) return failure("Se escribió la partida, pero no se pudo cerrar el diario. Usa Recuperar antes de jugar. "+e,j.plan.id,true);
        TransferResult r; r.ok=true; r.id=j.plan.id; r.message="Partida importada y confirmada en emuMMC. La copia anterior se conserva para Recuperar. Algunos juegos vinculan datos internos a una cuenta; compruébala al iniciar."; return r;
    }
    const auto cause=e; std::string rollbackError;
    // Never attempt rollback inside the failed/full journal transaction.
    // Unmount does not commit. Revalidate the same save and environment before
    // opening a fresh transaction; already committed chunks remain recoverable.
    mount.close();
    if(gate(backend_,SaveEnvironment::EmuMMC,rollbackError,j.instance)&&existsNow(backend_,j.plan.destination,rollbackError)&&
       mount.open(j.plan.destination,true,rollbackError)&&restoreBackup(backend_,root_,j,mount.path,rollbackError))
        return failure("La importación falló y se restauró la partida anterior. "+cause,j.plan.id);
    std::string journalError; appendJournal(backend_,root_,j,"recovery-required",journalError);
    return failure("No se pudo completar la importación ni recuperar automáticamente. No abras el juego; usa Recuperar. "+cause+" "+rollbackError,j.plan.id,true);
}
bool Engine::listRecoveries(std::vector<RecoveryEntry>& out,std::string& error) {
    out.clear(); error.clear(); std::vector<std::string> ids; if(!listIds(root_+"/transactions",ids,error)) return false;
    for(const auto& id:ids) { if(incompletePreparation(root_,id)) continue; Journal j; if(!readJournal(root_,id,j,error)) return false;
        if(j.state=="prepared"||j.state=="restored") continue;
        RecoveryEntry r; r.id=id; r.state=j.state; r.backupId=j.plan.backupId; r.destination=j.plan.destination; r.needsRecovery=j.state!="applied"; out.push_back(r);
    }
    return true;
}
TransferResult Engine::recover(const RecoveryEntry& recovery) {
    std::string e; Journal j;
    if(!readJournal(root_,recovery.id,j,e)) return failure(e);
    if(!equalSave(recovery.destination,j.plan.destination)||recovery.backupId!=j.plan.backupId||recovery.state!=j.state||j.state=="prepared"||j.state=="restored") return failure("La recuperación elegida ya no corresponde a una importación recuperable. Vuelve a consultar la lista.");
    if(!gate(backend_,SaveEnvironment::EmuMMC,e,j.instance)||!existsNow(backend_,j.plan.destination,e)) return failure(e,recovery.id,true);
    Snapshot backup; if(!readSnapshot(backend_,root_+"/backups",j.plan.backupId,backup,e)||backup.hash!=j.plan.backupSha256||!equalSave(backup.save,j.plan.destination)) return failure("Copia de seguridad no válida. No se modificó el destino. "+e,recovery.id,true);
    Mount mount(backend_); if(!mount.open(j.plan.destination,true,e)) return failure(e,recovery.id,true);
    if(!restoreBackup(backend_,root_,j,mount.path,e)) return failure("La recuperación no terminó. Conserva la SD y no abras el juego. "+e,recovery.id,true);
    TransferResult r; r.ok=true; r.id=recovery.id; r.message="Restaurada y confirmada la copia anterior a la importación. La copia en SD se conserva."; return r;
}
} // namespace transfer_detail

bool compatibleTransferTarget(const SaveEntry& a,const SaveEntry& b) {
    return transfer_detail::validSave(a)&&transfer_detail::validSave(b)&&a.titleId==b.titleId&&a.kind==b.kind&&a.index==b.index&&a.rank==b.rank;
}
std::string transferTitleId(uint64_t id) { auto s=transfer_detail::hex(id); std::transform(s.begin(),s.end(),s.begin(),[](char c){return c>='a'&&c<='f'?char(c-'a'+'A'):c;}); return s; }

// Native service bindings. Only public FS save APIs, no raw storage or key APIs.
namespace {
constexpr const char* TransferRoot="sdmc:/config/NinMagicSaves/transfer";
#ifdef __SWITCH__
std::string resultCode(Result rc) { char b[24]; std::snprintf(b,sizeof(b),"0x%08lx",static_cast<unsigned long>(rc)); return b; }
std::string instanceFingerprint(uint64_t device) {
    // A local anti-mix-up binding, NOT evidence that the config was booted.
    // Runtime sysMMC/emuMMC evidence comes exclusively from SPL 65007.
    std::string text,error;
    if(!device||!transfer_detail::readSmall("sdmc:/emuMMC/emummc.ini",text,8192,error)) return {};
    std::string profilePath;
    auto fingerprint=transfer_detail::destinationConfigFingerprint(device,text,profilePath);
    if(fingerprint.empty()||!transfer_detail::safeDirectory("sdmc:/"+profilePath,false,error)) return {};
    return fingerprint;
}
class NativeBackend final : public transfer_detail::Backend {
    bool spl_=false,nifm_=false,pm_=false,mounted_=false,writable_=false;
    SaveEntry mountedSave_;
public:
    ~NativeBackend() override { unmount(); if(pm_) pmdmntExit(); if(nifm_) nifmExit(); if(spl_) splExit(); }
    TransferEnvironment environment() override {
        TransferEnvironment env;
        env.modeLabel="Entorno sin comprobar"; env.networkLabel="Red sin comprobar"; env.processLabel="Juego abierto sin comprobar";
        if(!spl_) spl_=R_SUCCEEDED(splInitialize());
        if(spl_) {
            u64 active=UINT64_MAX; const auto rc=splGetConfig(static_cast<SplConfigItem>(65007),&active);
            if(R_SUCCEEDED(rc)&&active<=1) {
                env.mode=active?SaveEnvironment::EmuMMC:SaveEnvironment::SysMMC;
                env.modeLabel=active?"Atmosphere en emuMMC":"Atmosphere en sysMMC";
                if(active) { u64 device=0; if(R_SUCCEEDED(splGetConfig(SplConfigItem_DeviceId,&device))) env.instanceId=instanceFingerprint(device); }
            } else env.detail="No se pudo comprobar el entorno activo mediante Atmosphere: "+resultCode(rc);
        }
        if(!nifm_) nifm_=R_SUCCEEDED(nifmInitialize(NifmServiceType_User));
        if(nifm_) {
            bool wireless=true; const auto wirelessRc=nifmIsWirelessCommunicationEnabled(&wireless);
            NifmInternetConnectionType type{}; NifmInternetConnectionStatus status{}; u32 strength=0;
            const auto connectionRc=nifmGetInternetConnectionStatus(&type,&strength,&status);
            // libnx nifm.h documents exactly 0xD46ED as neither connected nor
            // connecting. Other errors are UNKNOWN, never evidence of offline.
            env.offlineVerified=R_SUCCEEDED(wirelessRc)&&!wireless&&connectionRc==0xD46ED;
            if(env.offlineVerified) env.networkLabel="Wi-Fi desactivado; sin conexión ni conexión en curso";
            else if(R_SUCCEEDED(connectionRc)) env.networkLabel="Hay conexión de red o un intento de conexión";
            else if(R_SUCCEEDED(wirelessRc)&&wireless) env.networkLabel="Desactiva Wi-Fi y desconecta el cable/adaptador de red";
            else env.networkLabel="Estado de red no verificable: "+resultCode(connectionRc);
        }
        if(!pm_) pm_=R_SUCCEEDED(pmdmntInitialize());
        if(pm_) {
            constexpr Result ProcessNotFound=15U|(1U<<9);
            u64 application=0,self=0,again=0;
            const auto rc=pmdmntGetApplicationProcessId(&application);
            if(rc==ProcessNotFound) { env.gamesClosed=true; env.processLabel="Ningún juego abierto"; }
            else if(R_SUCCEEDED(rc)&&application&&R_SUCCEEDED(svcGetProcessId(&self,CUR_PROCESS_HANDLE))&&self==application&&
                    R_SUCCEEDED(pmdmntGetApplicationProcessId(&again))&&again==application) {
                env.gamesClosed=true; env.processLabel="Solo NinMagic Saves en modo aplicación";
            } else if(R_SUCCEEDED(rc)&&application) env.processLabel="Cierra el juego antes de transferir sus partidas";
            else env.processLabel="No se pudo comprobar el proceso activo: "+resultCode(rc);
        }
        if(env.mode==SaveEnvironment::EmuMMC&&env.instanceId.empty()) env.detail="No se pudo vincular la copia a esta consola y al perfil emuMMC. No se restaurarán partidas.";
        return env;
    }
    bool enumerate(std::vector<SaveEntry>& saves,std::string& error) override {
        saves.clear(); error.clear(); FsSaveDataInfoReader reader{};
        auto rc=fsOpenSaveDataInfoReader(&reader,FsSaveDataSpaceId_User);
        if(R_FAILED(rc)) { error="No se pudo enumerar la memoria de partidas activa: "+resultCode(rc); return false; }
        std::array<FsSaveDataInfo,32> infos{}; s64 count=0; bool ok=true;
        do {
            rc=fsSaveDataInfoReaderRead(&reader,infos.data(),infos.size(),&count);
            if(R_FAILED(rc)||count<0||count>static_cast<s64>(infos.size())) { error="Enumeración de partidas incompleta: "+resultCode(rc); ok=false; break; }
            for(s64 i=0;i<count;++i) {
                const auto& info=infos[size_t(i)];
                if(info.save_data_space_id!=FsSaveDataSpaceId_User || info.save_data_rank!=FsSaveDataRank_Primary ||
                    (info.save_data_type!=FsSaveDataType_Account&&info.save_data_type!=FsSaveDataType_Device)||info.system_save_data_id) continue;
                SaveEntry save; save.titleId=info.application_id; save.saveDataId=info.save_data_id; save.userLow=info.uid.uid[0]; save.userHigh=info.uid.uid[1];
                save.kind=static_cast<SaveKind>(info.save_data_type); save.index=info.save_data_index; save.rank=info.save_data_rank;
                if(!transfer_detail::validSave(save)) continue;
                save.gameName=transferTitleId(save.titleId);
                save.userName=save.kind==SaveKind::Device?"Compartida por la consola":"Usuario "+transferTitleId(save.userLow)+transferTitleId(save.userHigh);
                saves.push_back(std::move(save));
                if(saves.size()>16384) { error="Demasiadas partidas para una enumeración segura."; ok=false; break; }
            }
        } while(ok&&count>0);
        fsSaveDataInfoReaderClose(&reader); if(!ok) { saves.clear(); return false; }
        const bool nsReady=R_SUCCEEDED(nsInitialize());
        const bool accountReady=R_SUCCEEDED(accountInitialize(AccountServiceType_Administrator));
        std::unique_ptr<NsApplicationControlData> control(new(std::nothrow) NsApplicationControlData{});
        std::map<uint64_t,std::string> names;
        for(auto& save:saves) {
            auto name=names.find(save.titleId);
            if(name!=names.end()) save.gameName=name->second;
            else if(nsReady&&control) {
                u64 actual=0; std::memset(control.get(),0,sizeof(*control));
                if(R_SUCCEEDED(nsGetApplicationControlData(NsApplicationControlSource_Storage,save.titleId,control.get(),sizeof(*control),&actual))&&actual>=sizeof(NacpStruct)) {
                    NacpLanguageEntry* language=nullptr;
                    if(R_SUCCEEDED(nacpGetLanguageEntry(&control->nacp,&language))&&language&&language->name[0]) save.gameName.assign(language->name,strnlen(language->name,sizeof(language->name)));
                }
                if(save.gameName.size()>256) save.gameName.resize(256);
                names.emplace(save.titleId,save.gameName);
            }
            if(accountReady&&save.kind==SaveKind::Account) {
                AccountUid uid{{save.userLow,save.userHigh}}; AccountProfile profile{};
                if(R_SUCCEEDED(accountGetProfile(&profile,uid))) {
                    AccountProfileBase base{};
                    if(R_SUCCEEDED(accountProfileGet(&profile,nullptr,&base))&&base.nickname[0]) save.userName.assign(base.nickname,strnlen(base.nickname,sizeof(base.nickname)));
                    accountProfileClose(&profile);
                }
            }
        }
        if(accountReady) accountExit();
        if(nsReady) nsExit();
        std::sort(saves.begin(),saves.end(),[](const SaveEntry& a,const SaveEntry& b){ if(a.gameName!=b.gameName) return a.gameName<b.gameName; if(a.userName!=b.userName) return a.userName<b.userName; return a.saveDataId<b.saveDataId; });
        return true;
    }
    bool mount(const SaveEntry& save,bool writable,std::string& path,std::string& error) override {
        if(mounted_) { error="Ya hay una partida abierta para transferencia."; return false; }
        // Re-query runtime immediately before opening a writable FS handle.
        if(writable&&!transfer_detail::gate(*this,SaveEnvironment::EmuMMC,error)) return false;
        FsSaveDataAttribute attr{}; attr.application_id=save.titleId; attr.uid.uid[0]=save.userLow; attr.uid.uid[1]=save.userHigh;
        attr.save_data_type=uint8_t(save.kind); attr.save_data_rank=save.rank; attr.save_data_index=save.index;
        FsFileSystem fs{};
        const auto rc=writable?fsOpenSaveDataFileSystem(&fs,FsSaveDataSpaceId_User,&attr):fsOpenReadOnlySaveDataFileSystem(&fs,FsSaveDataSpaceId_User,&attr);
        if(R_FAILED(rc)) { error="No se pudo abrir la partida "+std::string(writable?"de destino":"en solo lectura")+": "+resultCode(rc)+". Inicia el juego una vez si aún no tiene partida."; return false; }
        if(fsdevMountDevice("nmsvsave",fs)<0) { error="No se pudo montar la partida. No se creará ninguna partida nueva."; return false; }
        mounted_=true; writable_=writable; mountedSave_=save; path="nmsvsave:/"; return true;
    }
    void unmount() override { if(mounted_) fsdevUnmountDevice("nmsvsave"); mounted_=false; writable_=false; mountedSave_={}; }
    bool readCapacityMetadata(transfer_detail::SaveCapacityMetadata& metadata,std::string& error) {
        metadata={}; error.clear(); auto* fs=mounted_?fsdevGetDeviceFileSystem("nmsvsave"):nullptr;
        if(!fs) { error="No hay una partida montada para comprobar su capacidad."; return false; }
        // GetTotalSpace on a read-only save FS is unsupported (FS 6371,
        // 0x0031C602). Query the existing save's extra data through fsp-srv
        // instead; this neither opens a writable handle nor resizes the save.
        // data_size is the usable area, not FsSaveDataInfo.size (container).
        FsSaveDataExtraData extra{};
        const auto rc=fsReadSaveDataFileSystemExtraDataBySaveDataSpaceId(
            &extra,sizeof(extra),FsSaveDataSpaceId_User,mountedSave_.saveDataId);
        if(R_FAILED(rc)) { error="No se pudo leer la capacidad real de la partida de destino: "+resultCode(rc); return false; }
        metadata.titleId=extra.attr.application_id;
        metadata.userLow=extra.attr.uid.uid[0]; metadata.userHigh=extra.attr.uid.uid[1];
        metadata.systemSaveDataId=extra.attr.system_save_data_id;
        metadata.kind=static_cast<SaveKind>(extra.attr.save_data_type);
        metadata.rank=extra.attr.save_data_rank; metadata.index=extra.attr.save_data_index;
        metadata.dataSize=extra.data_size; metadata.journalSize=extra.journal_size;
        return true;
    }
    bool saveCapacity(uint64_t& bytes,std::string& error) override {
        bytes=0; transfer_detail::SaveCapacityMetadata metadata;
        return readCapacityMetadata(metadata,error)&&transfer_detail::validatedSaveCapacity(mountedSave_,metadata,bytes,error);
    }
    bool saveWriteBudget(uint64_t& bytes,std::string& error) override {
        bytes=0; transfer_detail::SaveCapacityMetadata metadata;
        return readCapacityMetadata(metadata,error)&&transfer_detail::validatedSaveWriteBudget(mountedSave_,metadata,bytes,error);
    }
    bool commitSave(std::string& error) override {
        if(!mounted_||!writable_) { error="No hay un destino abierto para confirmar."; return false; }
        auto* fs=fsdevGetDeviceFileSystem("nmsvsave");
        if(!fs) { error="El destino dejó de estar montado."; return false; }
        const auto rc=fsFsCommit(fs); if(R_FAILED(rc)) error="No se pudo confirmar la partida en emuMMC: "+resultCode(rc); return R_SUCCEEDED(rc);
    }
    bool commitSd(std::string& error) override { const auto rc=fsdevCommitDevice("sdmc"); if(R_FAILED(rc)) error="No se pudo confirmar el diario o la copia en SD: "+resultCode(rc); return R_SUCCEEDED(rc); }
};
#else
class NativeBackend final : public transfer_detail::Backend {
public:
    TransferEnvironment environment() override { TransferEnvironment e; e.modeLabel="PC: sin entorno Switch"; e.detail="La transferencia real se ejecuta únicamente en Nintendo Switch con Atmosphere."; return e; }
    bool enumerate(std::vector<SaveEntry>& saves,std::string& error) override { saves.clear(); error="No hay un servicio de partidas de Nintendo Switch en el PC."; return false; }
    bool mount(const SaveEntry&,bool,std::string&,std::string& error) override { error="Servicio FS no disponible."; return false; }
    void unmount() override {}
    bool saveCapacity(uint64_t&,std::string& error) override { error="Servicio FS no disponible."; return false; }
    bool saveWriteBudget(uint64_t&,std::string& error) override { error="Servicio FS no disponible."; return false; }
    bool commitSave(std::string& error) override { error="Servicio FS no disponible."; return false; }
    bool commitSd(std::string& error) override { error="SD de Switch no disponible."; return false; }
};
#endif
}
TransferEnvironment inspectTransferEnvironment() { NativeBackend b; return b.environment(); }
bool listTransferSaves(std::vector<SaveEntry>& saves,std::string& error) {
    NativeBackend b; transfer_detail::Engine e(b,TransferRoot); return e.listSaves(saves,error);
}
bool listTransferExports(std::vector<ExportEntry>& out,std::string& error) { NativeBackend b; transfer_detail::Engine e(b,TransferRoot); return e.listExports(out,error); }
TransferResult exportTransferSave(const SaveEntry& source) { NativeBackend b; transfer_detail::Engine e(b,TransferRoot); return e.exportSave(source); }
TransferResult prepareTransferRestore(const ExportEntry& source,const SaveEntry& target,RestorePlan& plan) { NativeBackend b; transfer_detail::Engine e(b,TransferRoot); return e.prepareRestore(source,target,plan); }
TransferResult applyTransferRestore(const RestorePlan& plan) { NativeBackend b; transfer_detail::Engine e(b,TransferRoot); return e.applyRestore(plan); }
bool listTransferRecoveries(std::vector<RecoveryEntry>& out,std::string& error) { NativeBackend b; transfer_detail::Engine e(b,TransferRoot); return e.listRecoveries(out,error); }
TransferResult recoverTransferRestore(const RecoveryEntry& recovery) { NativeBackend b; transfer_detail::Engine e(b,TransferRoot); return e.recover(recovery); }
} // namespace nmsaves
