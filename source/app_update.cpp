#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "app_update.hpp"
#include "update_network.hpp"
#ifdef __SWITCH__
#include <switch.h>
#endif
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <sstream>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>
#ifdef NM_APP_UPDATE_TEST
#include <functional>
#endif

namespace nm {
namespace {
constexpr uint64_t MaxNro=16ULL*1024*1024;
constexpr const char *Target="sdmc:/switch/NinMagicSaves/NinMagic-Saves.nro";
constexpr const char *CurrentVersion="0.2.2";
#ifdef NM_APP_UPDATE_TEST
std::function<bool(const std::string&,const std::string&)> failRename;
std::function<bool()> testCommit;
#endif
std::string physical(const std::string &path){
#ifdef NM_APP_UPDATE_TEST
    const char *base=std::getenv("NM_APP_UPDATE_DEVICE");if(base&&path.rfind("sdmc:/",0)==0)return std::string(base)+"/"+path.substr(6);
#endif
    return path;
}
std::string config(){return physical("sdmc:/config/NinMagicSaves");}
std::string root(){return config()+"/app-update";}
std::string target(){return physical(Target);}
std::string sibling(const char *name){return physical(std::string("sdmc:/switch/NinMagicSaves/")+name);}
bool statPath(const std::string &path,struct stat &st){return lstat(path.c_str(),&st)==0;}
bool regular(const std::string &path,uint64_t maximum){struct stat st{};return statPath(path,st)&&S_ISREG(st.st_mode)&&!S_ISLNK(st.st_mode)&&st.st_size>=0&&uint64_t(st.st_size)<=maximum;}
bool removeFile(const std::string &path){struct stat st{};if(!statPath(path,st))return errno==ENOENT;return S_ISREG(st.st_mode)&&!S_ISLNK(st.st_mode)&&unlink(path.c_str())==0;}
bool directory(const std::string &path){struct stat st{};if(statPath(path,st))return S_ISDIR(st.st_mode)&&!S_ISLNK(st.st_mode);if(errno!=ENOENT)return false;
#ifdef _WIN32
    return mkdir(path.c_str())==0;
#else
    return mkdir(path.c_str(),0777)==0;
#endif
}
bool ensureRoot(std::string &error){if(!directory(physical("sdmc:/config"))||!directory(config())||!directory(root())){error="No se puede abrir la carpeta privada de actualizaciones.";return false;}return true;}
bool renameFile(const std::string &from,const std::string &to){
#ifdef NM_APP_UPDATE_TEST
    if(failRename&&failRename(from,to)){errno=EIO;return false;}
#endif
    return rename(from.c_str(),to.c_str())==0;
}
bool commitDevice(){
#ifdef NM_APP_UPDATE_TEST
    if(testCommit)return testCommit();
#endif
#ifdef __SWITCH__
    return R_SUCCEEDED(fsdevCommitDevice("sdmc"));
#else
    return true;
#endif
}
bool closeWritten(FILE *f){bool ok=fflush(f)==0;if(ok)ok=fsync(fileno(f))==0;if(fclose(f)!=0)ok=false;return ok;}
bool writeFile(const std::string &path,const std::string &text,bool exclusive=false){struct stat st{};if(statPath(path,st)&&(!S_ISREG(st.st_mode)||S_ISLNK(st.st_mode)||exclusive))return false;int fd=open(path.c_str(),O_WRONLY|O_CREAT|(exclusive?O_EXCL:O_TRUNC),0600);if(fd<0)return false;FILE *f=fdopen(fd,"wb");if(!f){close(fd);return false;}bool ok=fwrite(text.data(),1,text.size(),f)==text.size();return closeWritten(f)&&ok;}
bool readSmall(const std::string &path,std::string &data,size_t maximum){data.clear();if(!regular(path,maximum))return false;FILE *f=fopen(path.c_str(),"rb");if(!f)return false;std::array<char,4096> b{};size_t n;while((n=fread(b.data(),1,b.size(),f))){if(data.size()+n>maximum){fclose(f);return false;}data.append(b.data(),n);}bool ok=!ferror(f);fclose(f);return ok;}
bool copyFile(const std::string &from,const std::string &to,bool exclusive,std::string &error){if(!regular(from,MaxNro)){error="El NRO de origen no es un archivo regular válido.";return false;}struct stat st{};if(statPath(to,st)&&(!S_ISREG(st.st_mode)||S_ISLNK(st.st_mode)||exclusive)){error="El destino de la actualización ya existe o no es válido.";return false;}FILE *in=fopen(from.c_str(),"rb");if(!in){error="No se pudo abrir el NRO.";return false;}int fd=open(to.c_str(),O_WRONLY|O_CREAT|(exclusive?O_EXCL:O_TRUNC),0600);if(fd<0){fclose(in);error="No se puede guardar el NRO; comprueba el espacio libre.";return false;}FILE *out=fdopen(fd,"wb");if(!out){close(fd);fclose(in);return false;}std::array<uint8_t,65536> bytes{};size_t n;uint64_t total=0;bool ok=true;while((n=fread(bytes.data(),1,bytes.size(),in))){total+=n;if(total>MaxNro||fwrite(bytes.data(),1,n,out)!=n){ok=false;break;}}if(ferror(in))ok=false;fclose(in);if(!closeWritten(out))ok=false;if(!ok)error="No se pudo guardar el NRO completo. El original se conserva.";return ok;}

class Sha256 {
    uint32_t h[8]={0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    std::array<uint8_t,64> pending{};size_t used=0;uint64_t length=0;
    static uint32_t rotate(uint32_t x,unsigned n){return (x>>n)|(x<<(32-n));}
    void block(const uint8_t *p){
        static constexpr uint32_t k[64]={0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
        uint32_t w[64];for(unsigned i=0;i<16;++i)w[i]=(uint32_t(p[i*4])<<24)|(uint32_t(p[i*4+1])<<16)|(uint32_t(p[i*4+2])<<8)|p[i*4+3];for(unsigned i=16;i<64;++i){uint32_t a=w[i-15],b=w[i-2];w[i]=(rotate(a,7)^rotate(a,18)^(a>>3))+w[i-16]+(rotate(b,17)^rotate(b,19)^(b>>10))+w[i-7];}
        uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],z=h[7];for(unsigned i=0;i<64;++i){uint32_t t=z+(rotate(e,6)^rotate(e,11)^rotate(e,25))+((e&f)^(~e&g))+k[i]+w[i];uint32_t t2=(rotate(a,2)^rotate(a,13)^rotate(a,22))+((a&b)^(a&c)^(b&c));z=g;g=f;f=e;e=d+t;d=c;c=b;b=a;a=t+t2;}h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d;h[4]+=e;h[5]+=f;h[6]+=g;h[7]+=z;
    }
public:
    void add(const uint8_t *p,size_t n){length+=n;while(n){size_t take=std::min(n,64-used);std::memcpy(pending.data()+used,p,take);used+=take;p+=take;n-=take;if(used==64){block(pending.data());used=0;}}}
    std::string finish(){uint64_t bits=length*8;pending[used++]=0x80;if(used>56){std::fill(pending.begin()+used,pending.end(),0);block(pending.data());used=0;}std::fill(pending.begin()+used,pending.begin()+56,0);for(unsigned i=0;i<8;++i)pending[63-i]=uint8_t(bits>>(i*8));block(pending.data());char out[65]{};for(unsigned i=0;i<8;++i)std::snprintf(out+i*8,9,"%08x",h[i]);return out;}
};
std::string shaText(const std::string &data){Sha256 hash;hash.add(reinterpret_cast<const uint8_t*>(data.data()),data.size());return hash.finish();}
bool shaFile(const std::string &path,std::string &digest,uint64_t &size){digest.clear();size=0;if(!regular(path,MaxNro))return false;FILE *f=fopen(path.c_str(),"rb");if(!f)return false;Sha256 hash;std::array<uint8_t,65536> bytes{};size_t n;while((n=fread(bytes.data(),1,bytes.size(),f))){size+=n;if(size>MaxNro){fclose(f);return false;}hash.add(bytes.data(),n);}bool ok=!ferror(f);fclose(f);if(ok)digest=hash.finish();return ok&&size>0;}
bool hexHash(const std::string &hash){return hash.size()==64&&hash.find_first_not_of("0123456789abcdef")==std::string::npos;}
bool httpsUrl(const std::string &url){if(url.size()>2048||url.rfind("https://",0)!=0)return false;auto end=url.find('/',8);std::string host=url.substr(8,end==std::string::npos?std::string::npos:end-8);return !host.empty()&&host.find('@')==std::string::npos&&url.find('#')==std::string::npos&&std::none_of(url.begin(),url.end(),[](unsigned char c){return c<=32||c==127||c=='\\';});}
bool number(const std::string &value,uint64_t &result){result=0;if(value.empty()||value.size()>20)return false;for(char c:value){if(c<'0'||c>'9'||result>(UINT64_MAX-unsigned(c-'0'))/10)return false;result=result*10+unsigned(c-'0');}return true;}
bool version(const std::string &text,std::array<unsigned,3> &parts){std::istringstream input(text);std::string part;unsigned count=0;while(std::getline(input,part,'.')){uint64_t n=0;if(count==3||part.size()>5||!number(part,n)||n>65535||(part.size()>1&&part[0]=='0'))return false;parts[count++]=unsigned(n);}return count==3&&!text.empty()&&text.back()!='.';}
bool newer(const std::string &a,const std::string &b){std::array<unsigned,3>x{},y{};return version(a,x)&&version(b,y)&&x>y;}
bool parseFields(const std::string &data,const char *magic,const std::vector<std::string> &keys,std::map<std::string,std::string>&fields){fields.clear();if(data.empty()||data.size()>4096||data.find('\0')!=std::string::npos)return false;std::istringstream input(data);std::string line;if(!std::getline(input,line))return false;if(!line.empty()&&line.back()=='\r')line.pop_back();if(line!=magic)return false;while(std::getline(input,line)){if(!line.empty()&&line.back()=='\r')line.pop_back();auto eq=line.find('=');if(eq==std::string::npos)return false;auto key=line.substr(0,eq);if(std::find(keys.begin(),keys.end(),key)==keys.end()||!fields.emplace(key,line.substr(eq+1)).second)return false;}return fields.size()==keys.size();}
bool validInfo(const AppUpdateInfo &info){std::array<unsigned,3> parts{};return version(info.version,parts)&&httpsUrl(info.url)&&hexHash(info.sha256)&&info.size>=0x80+56+0x4000&&info.size<=MaxNro;}
std::string manifestText(const AppUpdateInfo &info){return "NINMAGIC-SAVES-UPDATE-1\nversion="+info.version+"\nurl="+info.url+"\nsha256="+info.sha256+"\nsize="+std::to_string(info.size)+"\n";}
bool parseManifest(const std::string &data,AppUpdateInfo &info){std::map<std::string,std::string> f;if(!parseFields(data,"NINMAGIC-SAVES-UPDATE-1",{"version","url","sha256","size"},f))return false;AppUpdateInfo next;next.version=f["version"];next.url=f["url"];next.sha256=f["sha256"];for(char &c:next.sha256)if(c>='A'&&c<='F')c=char(c-'A'+'a');if(!number(f["size"],next.size)||!validInfo(next))return false;next.configured=true;next.available=newer(next.version,CurrentVersion);info=std::move(next);return true;}
uint32_t u32(const uint8_t *p){return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);}
uint64_t u64(const uint8_t *p){uint64_t n=0;for(unsigned i=0;i<8;++i)n|=uint64_t(p[i])<<(i*8);return n;}
bool readAt(FILE *f,uint64_t offset,void *out,size_t size){return offset<=MaxNro&&fseek(f,long(offset),SEEK_SET)==0&&fread(out,1,size,f)==size;}
bool bound(uint64_t offset,uint64_t size,uint64_t maximum){return offset<=maximum&&size<=maximum-offset;}
std::string stringAt(const uint8_t *p,size_t size){const void *end=std::memchr(p,0,size);return end?std::string(reinterpret_cast<const char*>(p),static_cast<const uint8_t*>(end)-p):std::string(1,'\1');}
bool inspectNro(const std::string &path,const std::string &expected,std::string &found,std::string &error){
    found.clear();struct stat st{};if(!regular(path,MaxNro)||!statPath(path,st)){error="El ejecutable no es un NRO regular de hasta 16 MiB.";return false;}uint64_t total=uint64_t(st.st_size);FILE *f=fopen(path.c_str(),"rb");if(!f){error="No se puede abrir el NRO para comprobarlo.";return false;}std::array<uint8_t,0x80> header{};std::array<uint8_t,56> asset{};bool ok=readAt(f,0,header.data(),header.size())&&std::memcmp(header.data()+0x10,"NRO0",4)==0;uint64_t code=ok?u32(header.data()+0x18):0;ok=ok&&code>=0x80&&bound(code,56,total);uint64_t end=0;
    for(unsigned i=0;ok&&i<3;++i){uint64_t offset=u32(header.data()+0x20+i*8),size=u32(header.data()+0x24+i*8);ok=size&&offset>=end&&bound(offset,size,code)&&(i||offset==0);end=offset+size;}ok=ok&&readAt(f,code,asset.data(),asset.size())&&std::memcmp(asset.data(),"ASET",4)==0&&u32(asset.data()+4)==0;uint64_t nacp=0;end=56;
    for(unsigned i=0;ok&&i<3;++i){uint64_t offset=u64(asset.data()+8+i*16),size=u64(asset.data()+16+i*16);ok=size&&offset>=end&&bound(offset,size,total-code);end=offset+size;if(i==1){ok=ok&&size==0x4000;nacp=code+offset;}}
    std::array<uint8_t,0x4000> control{};ok=ok&&readAt(f,nacp,control.data(),control.size());fclose(f);
    if(ok){for(unsigned i=0;i<16;++i){auto title=stringAt(control.data()+i*0x300,0x200),author=stringAt(control.data()+i*0x300+0x200,0x100);if((i==0&&title!="NinMagic Saves")||(!title.empty()&&(title!="NinMagic Saves"||author!="NinMagic"))||(title.empty()&&!author.empty())){ok=false;break;}}found=stringAt(control.data()+0x3060,16);std::array<unsigned,3> parts{};ok=ok&&version(found,parts)&&(expected.empty()||found==expected);}
    if(!ok){error="El NRO, sus recursos o el nombre/versión internos no corresponden a NinMagic Saves.";return false;}return true;
}
bool matchesFile(const std::string &path,const std::string &hash,uint64_t size){std::string actual;uint64_t bytes=0;return shaFile(path,actual,bytes)&&actual==hash&&bytes==size;}
bool validTarget(const std::string &path,std::string &error){if(path!=Target){error="La actualización solo admite sdmc:/switch/NinMagicSaves/NinMagic-Saves.nro.";return false;}for(const char *parent:{"sdmc:/switch","sdmc:/switch/NinMagicSaves"}){struct stat st{};if(!statPath(physical(parent),st)||!S_ISDIR(st.st_mode)||S_ISLNK(st.st_mode)){error="La carpeta de la aplicación no es un directorio normal de la microSD.";return false;}}return true;}
struct Prepared {AppUpdateInfo info;std::string oldHash,oldVersion;uint64_t oldSize=0;};
std::string preparedText(const Prepared &p){return manifestText(p.info)+"old_sha256="+p.oldHash+"\nold_size="+std::to_string(p.oldSize)+"\nold_version="+p.oldVersion+"\n";}
bool parsePrepared(const std::string &data,Prepared &p){std::map<std::string,std::string> f;if(!parseFields(data,"NINMAGIC-SAVES-UPDATE-1",{"version","url","sha256","size","old_sha256","old_size","old_version"},f))return false;p.info.version=f["version"];p.info.url=f["url"];p.info.sha256=f["sha256"];p.oldHash=f["old_sha256"];p.oldVersion=f["old_version"];std::array<unsigned,3> parts{};return number(f["size"],p.info.size)&&number(f["old_size"],p.oldSize)&&p.oldSize<=MaxNro&&p.oldSize>0&&hexHash(p.oldHash)&&version(p.oldVersion,parts)&&validInfo(p.info)&&newer(p.info.version,p.oldVersion);}
bool sameInfo(const AppUpdateInfo &a,const AppUpdateInfo &b){return a.version==b.version&&a.url==b.url&&a.sha256==b.sha256&&a.size==b.size;}
struct Journal {Prepared prepared;std::string token,state;};
bool tokenValid(const std::string &s){return s.size()==8&&s.find_first_not_of("0123456789")==std::string::npos;}
std::string backup(const std::string &token){return root()+"/backup-"+token+".nro";}
std::string journalText(const Journal &j){std::string data="NINMAGIC-SAVES-INSTALL-1\ntoken="+j.token+"\nstate="+j.state+"\n"+preparedText(j.prepared);return data+"checksum="+shaText(data)+"\n";}
bool parseJournal(std::string data,Journal &j){auto at=data.rfind("checksum=");if(at==std::string::npos||data.size()-at!=74||data.back()!='\n'||shaText(data.substr(0,at))!=data.substr(at+9,64))return false;data.resize(at);const std::string prefix="NINMAGIC-SAVES-INSTALL-1\ntoken=";if(data.rfind(prefix,0)!=0)return false;auto end=data.find('\n',prefix.size());if(end==std::string::npos)return false;j.token=data.substr(prefix.size(),end-prefix.size());auto next=data.find('\n',end+1);if(next==std::string::npos||data.compare(end+1,6,"state="))return false;j.state=data.substr(end+7,next-end-7);if(!tokenValid(j.token)||(j.state!="ready"&&j.state!="committed"&&j.state!="restored"))return false;return parsePrepared(data.substr(next+1),j.prepared);}
bool saveJournal(const Journal &j){const auto path=root()+"/install.txt",pending=path+".pending",previous=path+".previous";if(!writeFile(pending,journalText(j))||!removeFile(previous))return false;bool had=regular(path,4096);if(had&&!renameFile(path,previous))return false;if(!renameFile(pending,path)){if(had)renameFile(previous,path);return false;}return true;}
bool readJournal(Journal &j,bool &exists,std::string &error){exists=false;bool found=false;for(const char *name:{"install.txt","install.txt.pending","install.txt.previous"}){const auto path=root()+"/"+name;struct stat st{};if(!statPath(path,st)){if(errno==ENOENT)continue;error="No se pudo revisar el registro de actualización.";return false;}exists=true;if(!S_ISREG(st.st_mode)||S_ISLNK(st.st_mode)){error="El registro de actualización contiene una ruta no admitida.";return false;}std::string data;Journal candidate;if(readSmall(path,data,4096)&&parseJournal(data,candidate)&&(!found||candidate.token>j.token)){j=std::move(candidate);found=true;}}if(exists&&!found){error="El registro de actualización está dañado. Se conserva el NRO y su copia anterior.";return false;}return true;}
bool restoreOld(const Journal &j,std::string &error,bool allowDamagedCurrent=false){
    const auto &p=j.prepared;if(!matchesFile(backup(j.token),p.oldHash,p.oldSize)){error="La copia anterior no supera su SHA-256; no se sustituirá el ejecutable.";return false;}std::string versionFound;if(!inspectNro(backup(j.token),p.oldVersion,versionFound,error))return false;const auto staging=sibling("NinMagic-Saves-recovery.pending");if(!removeFile(staging)||!copyFile(backup(j.token),staging,true,error)||!matchesFile(staging,p.oldHash,p.oldSize))return false;
    struct stat st{};if(statPath(target(),st)){if(!allowDamagedCurrent&&!matchesFile(target(),p.oldHash,p.oldSize)&&!matchesFile(target(),p.info.sha256,p.info.size)){error="El NRO actual cambió fuera de esta actualización; no se sobrescribe.";return false;}if(!removeFile(target())){error="No se pudo retirar el NRO para recuperar la versión anterior.";return false;}}else if(errno!=ENOENT)return false;
    if(!renameFile(staging,target())||!matchesFile(target(),p.oldHash,p.oldSize)||!commitDevice()){error="No se pudo confirmar la recuperación del NRO. Conserva la copia backup-"+j.token+".nro de config/NinMagicSaves/app-update.";return false;}return true;
}
bool cleanupResolved(const Journal &j,std::string &error){
    for(const auto &item:{std::pair<std::string,bool>(sibling("NinMagic-Saves-update-recovery.nro"),true),{sibling("NinMagic-Saves-update-pending.nro"),false},{root()+"/prepared.nro",false}}){
        struct stat st{};if(!statPath(item.first,st)){if(errno==ENOENT)continue;error="No se pudo revisar un NRO temporal de actualización.";return false;}
        const auto &hash=item.second?j.prepared.oldHash:j.prepared.info.sha256;const auto bytes=item.second?j.prepared.oldSize:j.prepared.info.size;
        if(!matchesFile(item.first,hash,bytes)||!removeFile(item.first)){error="Se conserva un NRO temporal que no puede verificarse o retirarse.";return false;}
    }
    struct stat st{};const auto metadata=root()+"/prepared.txt";
    if(statPath(metadata,st)){
        std::string data;Prepared p;
        if(!readSmall(metadata,data,4096)||!parsePrepared(data,p)||preparedText(p)!=preparedText(j.prepared)||!removeFile(metadata)){error="Se conserva una preparación de actualización distinta o dañada.";return false;}
    }else if(errno!=ENOENT){error="No se pudo revisar la preparación de actualización.";return false;}
    return true;
}
bool absent(const std::string &path){struct stat st{};return !statPath(path,st)&&errno==ENOENT;}
bool inspectInstalled(std::string &found,std::string &hash,uint64_t &size,std::string &error){
    found.clear();hash.clear();size=0;std::string confirmed;
    if(!validTarget(Target,error)||!inspectNro(target(),"",found,error)||!shaFile(target(),hash,size)||!inspectNro(target(),found,confirmed,error)||!matchesFile(target(),hash,size)){
        if(error.empty())error="No se pudo comprobar de forma estable el NRO instalado en la microSD.";
        found.clear();hash.clear();size=0;return false;
    }
    return true;
}
void installedInfo(AppUpdateInfo &info){
    info.runningVersion=CurrentVersion;std::string error;
    info.sdVerified=inspectInstalled(info.sdVersion,info.sdSha256,info.sdSize,error);
}
bool noInstallTemporaries(std::string &error){
    for(const auto &path:{root()+"/install.txt.pending",sibling("NinMagic-Saves-update-pending.nro"),sibling("NinMagic-Saves-update-recovery.nro"),sibling("NinMagic-Saves-recovery.pending")}){
        if(!absent(path)){error="Hay una actualización preparada, pendiente o un archivo de recuperación. No se archivará su registro.";return false;}
    }
    return true;
}
bool noArchiveTemporaries(std::string &error){
    if(!noInstallTemporaries(error))return false;
    for(const auto &path:{root()+"/prepared.nro",root()+"/prepared.txt",root()+"/download.pending"}){
        if(!absent(path)){error="Hay una actualización preparada, pendiente o un archivo de recuperación. No se archivará su registro.";return false;}
    }
    return true;
}
bool regularPreparationCache(std::string &error){
    // A cancelled download review is not an interrupted NRO replacement.
    // Keep bounded regular cache files for a later re-prepare; never treat
    // installation staging, a journal, a directory or a link as harmless cache.
    for(const auto &entry:{std::pair<const char*,uint64_t>("prepared.nro",MaxNro),{"prepared.txt",4096},{"download.pending",MaxNro}}){
        const auto path=root()+"/"+entry.first;
        if(!absent(path)&&!regular(path,entry.second)){error="La caché de preparación no es un archivo regular válido. Se conserva sin cambios.";return false;}
    }
    return true;
}
bool existingUpdateDirectories(std::string &error){
    for(const auto &path:{physical("sdmc:/config"),config(),root()}){
        struct stat st{};if(!statPath(path,st)){if(errno==ENOENT)return false;error="No se puede revisar la carpeta de actualización.";return false;}
        if(!S_ISDIR(st.st_mode)||S_ISLNK(st.st_mode)){error="La carpeta de actualización no es un directorio normal.";return false;}
    }
    return true;
}
struct ArchiveRecord {std::string name,data;Journal journal;bool parsed=false;};
struct ArchiveSnapshot {std::vector<ArchiveRecord> records;Journal journal;CompletedUpdateInfo info;};
bool verifiedBackup(const Journal &j,std::string &error){
    std::string found;
    if(!matchesFile(backup(j.token),j.prepared.oldHash,j.prepared.oldSize)||!inspectNro(backup(j.token),j.prepared.oldVersion,found,error)){
        if(error.empty())error="La copia anterior no coincide con el SHA-256 del registro.";
        return false;
    }
    return true;
}
bool captureArchive(const std::string &recordsRoot,ArchiveSnapshot &snapshot,std::string &error,const Journal *resolved=nullptr){
    snapshot={};error.clear();snapshot.info.state=CompletedUpdateState::Blocked;
    if(!inspectInstalled(snapshot.info.currentVersion,snapshot.info.currentSha256,snapshot.info.currentSize,error))return false;
    if(!existingUpdateDirectories(error)){if(error.empty())snapshot.info.state=CompletedUpdateState::None;return false;}
    bool selected=false;
    for(const char *name:{"install.txt","install.txt.previous"}){
        const auto path=recordsRoot+"/"+name;struct stat st{};
        if(!statPath(path,st)){if(errno==ENOENT)continue;error="No se puede revisar un registro de actualización.";return false;}
        ArchiveRecord record;record.name=name;
        if(!readSmall(path,record.data,4096)){error="Un registro no es un TXT regular y acotado.";return false;}
        record.parsed=parseJournal(record.data,record.journal);
        if(!resolved&&!record.parsed){error="Hay una copia del registro dañada; no se archivará automáticamente.";return false;}
        if(record.parsed&&(!selected||record.journal.token>snapshot.journal.token)){snapshot.journal=record.journal;selected=true;}
        snapshot.records.push_back(std::move(record));
    }
    if(snapshot.records.empty()){
        if(!noInstallTemporaries(error)||!regularPreparationCache(error))return false;
        snapshot.info.state=CompletedUpdateState::None;return false;
    }
    if(!noArchiveTemporaries(error))return false;
    if(!selected){error="No hay un registro válido para revisar.";return false;}
    if(resolved){
        if(snapshot.records.front().name!="install.txt"||snapshot.records.front().data!=journalText(*resolved)){error="El registro resuelto cambió antes de archivarlo.";return false;}
        snapshot.journal=*resolved;
        const auto &p=resolved->prepared;
        if((resolved->state!="committed"&&resolved->state!="restored")||!matchesFile(target(),resolved->state=="committed"?p.info.sha256:p.oldHash,resolved->state=="committed"?p.info.size:p.oldSize)){error="La resolución del ejecutable no se pudo confirmar.";return false;}
    }else for(const auto &record:snapshot.records){
        if(record.journal.state=="ready"){error="El registro todavía describe una operación pendiente; no se archivará.";return false;}
        if(record.journal.token==snapshot.journal.token&&preparedText(record.journal.prepared)!=preparedText(snapshot.journal.prepared)){error="Las copias del registro describen datos diferentes.";return false;}
    }
    if(!verifiedBackup(snapshot.journal,error))return false;
    for(const auto &record:snapshot.records)if(record.parsed&&record.journal.token!=snapshot.journal.token&&!verifiedBackup(record.journal,error))return false;
    auto &info=snapshot.info;info.token=snapshot.journal.token;info.journalState=snapshot.journal.state;info.oldVersion=snapshot.journal.prepared.oldVersion;info.newVersion=snapshot.journal.prepared.info.version;
    std::string binding="NINMAGIC-SAVES-ARCHIVE-REVIEW-1\n"+info.token+"\n"+info.journalState+"\n"+info.currentVersion+"\n"+info.currentSha256+"\n"+std::to_string(info.currentSize)+"\n";
    for(const auto &record:snapshot.records)binding+=record.name+"\n"+shaText(record.data)+"\n";
    info.reviewSha256=shaText(binding);info.state=CompletedUpdateState::CanArchive;
    info.detail="Registro finalizado "+info.oldVersion+" → "+info.newVersion+". NRO en microSD: "+info.currentVersion+". Archivar conserva el ejecutable, las descargas y las copias anteriores.";
    return true;
}
std::string archiveDirectory(const CompletedUpdateInfo &info){return root()+"/history/"+info.token+"-"+info.reviewSha256;}
std::string archiveReceipt(const ArchiveSnapshot &snapshot){
    std::string data="NINMAGIC-SAVES-UPDATE-ARCHIVE-1\nreview="+snapshot.info.reviewSha256+"\n";
    for(const auto &record:snapshot.records)data+=record.name+"="+shaText(record.data)+"\n";
    return data+"checksum="+shaText(data)+"\n";
}
bool sameReview(const CompletedUpdateInfo &a,const CompletedUpdateInfo &b){
    return a.state==CompletedUpdateState::CanArchive&&b.state==a.state&&a.token==b.token&&a.journalState==b.journalState&&a.oldVersion==b.oldVersion&&a.newVersion==b.newVersion&&a.currentVersion==b.currentVersion&&a.currentSha256==b.currentSha256&&a.currentSize==b.currentSize&&a.reviewSha256==b.reviewSha256;
}
bool stableArchive(const ArchiveSnapshot &snapshot,const std::string &destination,const std::vector<bool> &moved,std::string &error,bool sealed=false){
    if(!validTarget(Target,error)||!existingUpdateDirectories(error)||!noArchiveTemporaries(error)||!matchesFile(target(),snapshot.info.currentSha256,snapshot.info.currentSize)||!verifiedBackup(snapshot.journal,error)){
        if(error.empty())error="El NRO, su copia anterior o los temporales cambiaron después de la revisión.";
        return false;
    }
    for(const auto &path:{root()+"/history",destination}){struct stat st{};if(statPath(path,st)){if(!S_ISDIR(st.st_mode)||S_ISLNK(st.st_mode)){error="La carpeta de historial cambió o no es un directorio normal.";return false;}}else if(errno!=ENOENT){error="No se pudo revisar el historial de actualizaciones.";return false;}}
    if(sealed){std::string receipt;if(!readSmall(destination+"/receipt.txt",receipt,4096)||receipt!=archiveReceipt(snapshot)){error="El recibo del historial cambió antes de completar el archivo.";return false;}}
    for(size_t i=0;i<snapshot.records.size();++i){
        const auto &record=snapshot.records[i];std::string data;
        if(!readSmall((moved[i]?destination:root())+"/"+record.name,data,4096)||data!=record.data||(moved[i]&&!absent(root()+"/"+record.name))){error="Una copia del registro cambió después de la revisión.";return false;}
        if(record.parsed&&record.journal.token!=snapshot.journal.token&&!verifiedBackup(record.journal,error))return false;
    }
    // A previously absent active copy must remain absent too.
    for(const char *name:{"install.txt","install.txt.previous"})if(std::none_of(snapshot.records.begin(),snapshot.records.end(),[&](const auto &r){return r.name==name;})&&!absent(root()+"/"+name)){error="Apareció otro registro después de la revisión.";return false;}
    return true;
}
bool archiveSnapshot(const ArchiveSnapshot &snapshot,std::string &error){
    const auto destination=archiveDirectory(snapshot.info),receipt=archiveReceipt(snapshot);std::vector<bool> moved(snapshot.records.size(),false);
    if(!stableArchive(snapshot,destination,moved,error))return false;
    if(!directory(root()+"/history")||!directory(destination)){error="No se pudo abrir una carpeta normal para conservar el historial.";return false;}
    const auto receiptPath=destination+"/receipt.txt";std::string stored;
    if(!absent(receiptPath)){
        if(!readSmall(receiptPath,stored,4096)||stored!=receipt){error="La carpeta de historial ya contiene otro registro; no se sobrescribirá.";return false;}
    }else if(!writeFile(receiptPath,receipt,true)){error="No se pudo guardar el recibo exclusivo del historial.";return false;}
    if(!readSmall(receiptPath,stored,4096)||stored!=receipt||!commitDevice()){error="La microSD no confirmó el recibo del archivo; los registros siguen activos.";return false;}
    bool ok=true;
    // Keep the authoritative primary record until all auxiliary copies moved.
    for(size_t n=snapshot.records.size();n>0;--n){
        const auto i=n-1;const auto &record=snapshot.records[i];
        if(!stableArchive(snapshot,destination,moved,error,true)||!absent(destination+"/"+record.name)||!renameFile(root()+"/"+record.name,destination+"/"+record.name)){ok=false;break;}
        moved[i]=true;std::string data;if(!readSmall(destination+"/"+record.name,data,4096)||data!=record.data){ok=false;break;}
    }
    if(ok)ok=stableArchive(snapshot,destination,moved,error,true)&&commitDevice()&&stableArchive(snapshot,destination,moved,error,true);
    if(ok)return true;
    bool restored=true;
    for(size_t i=0;i<moved.size();++i)if(moved[i]){
        std::string data;const auto &record=snapshot.records[i];
        bool safe=existingUpdateDirectories(data);struct stat st{};for(const auto &path:{root()+"/history",destination})if(!statPath(path,st)||!S_ISDIR(st.st_mode)||S_ISLNK(st.st_mode))safe=false;
        if(!safe||!absent(root()+"/"+record.name)||!readSmall(destination+"/"+record.name,data,4096)||data!=record.data||!renameFile(destination+"/"+record.name,root()+"/"+record.name))restored=false;
    }
    if(!commitDevice())restored=false;
    error=restored?"No se pudo archivar el registro; sus copias activas se han conservado.":"No se pudo completar el archivo ni toda su devolución. Se conservan los TXT en app-update y en history; no se tocó el NRO ni su backup.";
    return false;
}
bool archiveResolved(const Journal &journal,std::string &error){ArchiveSnapshot snapshot;if(!captureArchive(root(),snapshot,error,&journal))return false;return archiveSnapshot(snapshot,error);}
}

bool recoverAppUpdate(const std::string &path,std::string &error){
    error.clear();if(!validTarget(path,error)||!ensureRoot(error))return false;Journal journal;bool exists=false;if(!readJournal(journal,exists,error)||!exists)return error.empty();const auto &p=journal.prepared;
    if(matchesFile(target(),p.info.sha256,p.info.size)){std::string found;if(!inspectNro(target(),p.info.version,found,error))return false;journal.state="committed";}
    else if(matchesFile(target(),p.oldHash,p.oldSize))journal.state="restored";
    else{struct stat st{};if(statPath(target(),st)){error="El NRO actual no coincide con la actualización interrumpida; se conserva sin cambios.";return false;}if(errno!=ENOENT||!restoreOld(journal,error))return false;journal.state="restored";}
    if(!saveJournal(journal)||!commitDevice()){error="Se recuperó el NRO, pero no se pudo confirmar su registro.";return false;}if(!cleanupResolved(journal,error))return false;if(!commitDevice()){error="El NRO se conserva, pero la microSD no confirmó la limpieza de la actualización.";return false;}return archiveResolved(journal,error);
}
bool canArchiveCompletedUpdate(const std::string &path,CompletedUpdateInfo &info,std::string &error){
    info={};error.clear();if(!validTarget(path,error)){info.state=CompletedUpdateState::Blocked;info.detail=error;return false;}
    ArchiveSnapshot snapshot;const bool ok=captureArchive(root(),snapshot,error);info=std::move(snapshot.info);
    if(!ok)info.detail=error.empty()?"No hay un registro activo de actualización que archivar.":error;
    return ok;
}
bool archiveCompletedUpdate(const std::string &path,const CompletedUpdateInfo &review,std::string &error){
    error.clear();if(!validTarget(path,error)||review.state!=CompletedUpdateState::CanArchive||!tokenValid(review.token)||!hexHash(review.reviewSha256)){if(error.empty())error="Primero revisa el registro finalizado que quieres archivar.";return false;}
    ArchiveSnapshot snapshot;
    if(captureArchive(root(),snapshot,error)){
        if(!sameReview(review,snapshot.info)){error="El registro o el NRO cambiaron desde la revisión. Vuelve a revisarlos antes de confirmar.";return false;}
        return archiveSnapshot(snapshot,error);
    }
    // A repeated confirmation is harmless only if the exact reviewed records
    // already exist in their verified history location and no active copy exists.
    if(snapshot.info.state==CompletedUpdateState::None){
        const auto destination=archiveDirectory(review);struct stat st{};
        if(statPath(root()+"/history",st)&&S_ISDIR(st.st_mode)&&!S_ISLNK(st.st_mode)&&statPath(destination,st)&&S_ISDIR(st.st_mode)&&!S_ISLNK(st.st_mode)&&captureArchive(destination,snapshot,error)&&sameReview(review,snapshot.info)){
            std::string receipt;
            if(absent(root()+"/install.txt")&&absent(root()+"/install.txt.previous")&&readSmall(destination+"/receipt.txt",receipt,4096)&&receipt==archiveReceipt(snapshot))return true;
        }
    }
    if(error.empty())error="El registro revisado cambió o ya no puede confirmarse; no se tocó el NRO.";
    return false;
}
bool checkAppUpdate(AppUpdateInfo &info,std::string &error){
    info={};installedInfo(info);const auto installed=info;error.clear();if(!ensureRoot(error))return false;struct stat st{};std::string url=OfficialUpdateManifest;if(statPath(config()+"/update-url.txt",st)){if(!readSmall(config()+"/update-url.txt",url,2049)){error="No se puede leer la URL de actualización.";return false;}while(!url.empty()&&(url.back()=='\n'||url.back()=='\r'))url.pop_back();if(!httpsUrl(url)){error="update-url.txt debe contener una sola URL HTTPS válida.";return false;}}else if(errno!=ENOENT){error="No se puede revisar update-url.txt.";return false;}info.configured=true;const auto file=root()+"/manifest.pending";if(!downloadHttpsFile(url,file,error))return false;std::string data;if(!readSmall(file,data,4096)||!parseManifest(data,info)){removeFile(file);error="El manifiesto de actualización no es válido.";return false;}removeFile(file);info.runningVersion=installed.runningVersion;info.sdVersion=installed.sdVersion;info.sdSha256=installed.sdSha256;info.sdSize=installed.sdSize;info.sdVerified=installed.sdVerified;info.detail=info.available?"Hay una versión nueva. La descarga se comprobará antes de pedir instalarla.":"La versión publicada no es posterior a la versión en ejecución ("+std::string(CurrentVersion)+").";return true;
}
bool prepareAppUpdate(AppUpdateInfo &info,const std::string &path,std::string &error){
    error.clear();info.prepared=false;info.installed=false;installedInfo(info);if(!validTarget(path,error)||!validInfo(info)||!info.available){if(error.empty())error="No hay una actualización posterior válida para preparar.";return false;}if(info.sdVerified&&info.sdVersion==info.version&&info.sdSha256==info.sha256&&info.sdSize==info.size){error="La versión publicada ya está en la microSD. Cierra y vuelve a abrir NinMagic para ejecutarla; no se reinstalará ni se modificará el registro.";return false;}if(!recoverAppUpdate(path,error))return false;Prepared prepared;prepared.info=info;std::string found;if(!inspectNro(target(),"",found,error)||!shaFile(target(),prepared.oldHash,prepared.oldSize))return false;prepared.oldVersion=found;if(!newer(info.version,found)){error="La versión descargable no es posterior al NRO instalado.";return false;}const auto pending=root()+"/download.pending";if(!downloadHttpsFile(info.url,pending,error))return false;if(!matchesFile(pending,info.sha256,info.size)||!inspectNro(pending,info.version,found,error)){removeFile(pending);if(error.empty())error="El tamaño o SHA-256 del NRO no coincide con su manifiesto.";return false;}if(!removeFile(root()+"/prepared.nro")||!renameFile(pending,root()+"/prepared.nro")||!writeFile(root()+"/prepared.txt",preparedText(prepared))||!commitDevice()){error="No se pudo guardar la actualización verificada; el NRO actual se conserva.";return false;}info.prepared=true;info.detail="Descarga verificada. Confirma para reemplazar la aplicación; después tendrás que cerrarla y abrirla.";return true;
}
bool installPreparedAppUpdate(AppUpdateInfo &info,const std::string &path,std::string &error){
    error.clear();info.installed=false;if(!validTarget(path,error)||!ensureRoot(error)||!info.prepared||!validInfo(info)){if(error.empty())error="Primero descarga y verifica la actualización.";return false;}Prepared prepared;std::string data,found;if(!readSmall(root()+"/prepared.txt",data,4096)||!parsePrepared(data,prepared)||!sameInfo(info,prepared.info)){error="La actualización preparada cambió o no es válida.";return false;}if(!matchesFile(target(),prepared.oldHash,prepared.oldSize)){error="El NRO instalado cambió desde la descarga. Comprueba la actualización de nuevo.";return false;}if(!matchesFile(root()+"/prepared.nro",info.sha256,info.size)||!inspectNro(root()+"/prepared.nro",info.version,found,error)){if(error.empty())error="La descarga guardada no supera su SHA-256.";return false;}Journal journal;journal.prepared=prepared;journal.state="ready";
    for(unsigned serial=1;serial<100000000;++serial){char token[9];std::snprintf(token,sizeof(token),"%08u",serial);struct stat st{};if(statPath(backup(token),st)||statPath(root()+"/backup-"+token+".txt",st))continue;journal.token=token;break;}if(journal.token.empty()){error="No queda un identificador libre para la copia anterior.";return false;}
    if(!copyFile(target(),backup(journal.token),true,error)||!matchesFile(backup(journal.token),prepared.oldHash,prepared.oldSize)||!writeFile(root()+"/backup-"+journal.token+".txt",preparedText(prepared),true)){if(error.empty())error="No se pudo confirmar la copia anterior; el NRO actual se conserva.";return false;}
    const auto staged=sibling("NinMagic-Saves-update-pending.nro"),rollback=sibling("NinMagic-Saves-update-recovery.nro");struct stat st{};if(statPath(rollback,st)){error="Existe un NRO de recuperación pendiente. Se conserva antes de actualizar.";return false;}if(!removeFile(staged)||!copyFile(root()+"/prepared.nro",staged,true,error)||!matchesFile(staged,info.sha256,info.size)||!saveJournal(journal)||!commitDevice()){if(error.empty())error="No se pudo preparar el reemplazo; se conserva el NRO actual.";return false;}
    if(!matchesFile(target(),prepared.oldHash,prepared.oldSize)||!renameFile(target(),rollback)){error="No se pudo conservar el NRO original antes del reemplazo.";return false;}
    bool replaced=renameFile(staged,target());bool verified=replaced&&matchesFile(target(),info.sha256,info.size)&&inspectNro(target(),info.version,found,error)&&commitDevice();if(verified){journal.state="committed";verified=saveJournal(journal)&&commitDevice();}
    if(!verified){std::string reason=error.empty()?"No se pudo confirmar el reemplazo del NRO.":error;std::string recovery;if(!restoreOld(journal,recovery,replaced)){error=reason+" "+recovery;return false;}journal.state="restored";if(saveJournal(journal)&&commitDevice()&&cleanupResolved(journal,recovery)&&commitDevice())archiveResolved(journal,recovery);error=reason+" Se ha recuperado la versión anterior.";return false;}
    if(!cleanupResolved(journal,error)||!commitDevice()||!archiveResolved(journal,error)){if(error.empty())error="La aplicación se actualizó, pero su registro sigue pendiente de archivo. Cierra y vuelve a abrir NinMagic.";return false;}info.prepared=false;info.installed=true;info.detail="Aplicación actualizada a "+info.version+". Ciérrala y vuelve a abrirla. Copia anterior: config/NinMagicSaves/app-update/backup-"+journal.token+".nro.";return true;
}
}
