#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "update_network.hpp"
#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __SWITCH__
#include <switch.h>
#include <curl/curl.h>
#endif

namespace nm {
namespace {
constexpr uint64_t MaxNro=16ULL*1024*1024;
constexpr const char *Root="sdmc:/config/NinMagicSaves/app-update";
bool safeUrl(const std::string &url){
    if(url.size()<9||url.size()>2048||url.rfind("https://",0)!=0)return false;
    const auto end=url.find('/',8);const auto host=url.substr(8,end==std::string::npos?end:end-8);
    return !host.empty()&&host.find('@')==std::string::npos&&url.find('#')==std::string::npos&&std::none_of(url.begin(),url.end(),[](unsigned char c){return c<=32||c==127||c=='\\';});
}
uint64_t bodyLimit(const std::string &path){
    if(path==std::string(Root)+"/manifest.pending")return 4096;
    if(path==std::string(Root)+"/download.pending")return MaxNro;
    return 0;
}
#if defined(__SWITCH__) || defined(NM_UPDATE_NETWORK_TEST)
struct Download{FILE *file=nullptr;uint64_t written=0,limit=0;bool overflow=false;};
size_t receive(void *ptr,size_t size,size_t count,void *user){
    auto &d=*static_cast<Download*>(user);
    if(size&&count>SIZE_MAX/size)return 0;
    const size_t bytes=size*count;
    if(d.written>d.limit||bytes>d.limit-d.written){d.overflow=true;return 0;}
    const size_t written=fwrite(ptr,1,bytes,d.file);d.written+=written;return written;
}
#endif
#ifdef __SWITCH__
bool socketsReady=false,nifmReady=false,curlReady=false;
std::string networkHint;
bool networkInit(std::string &error){
    // The updater must never establish a connection in the original NAND,
    // even if a caller accidentally exposes its screen there.
    u64 mode=0;
    Result modeResult=splInitialize();
    if(R_SUCCEEDED(modeResult)){
        modeResult=splGetConfig(static_cast<SplConfigItem>(65007),&mode);
        splExit();
    }
    if(R_FAILED(modeResult)||mode!=1){error="Las actualizaciones por Internet solo están disponibles en emuMMC comprobada.";return false;}
    if(!socketsReady){const Result rc=socketInitializeDefault();if(R_FAILED(rc)){char detail[32];std::snprintf(detail,sizeof(detail),"0x%08lX",static_cast<unsigned long>(rc));error="No se puede abrir BSD para HTTPS ("+std::string(detail)+").";return false;}socketsReady=true;}
    Result rc=0;if(!nifmReady){rc=nifmInitialize(NifmServiceType_User);nifmReady=R_SUCCEEDED(rc);}
    NifmInternetConnectionType type{};u32 strength=0;NifmInternetConnectionStatus status{};
    if(nifmReady)rc=nifmGetInternetConnectionStatus(&type,&strength,&status);
    char hint[96];std::snprintf(hint,sizeof(hint),"NIFM rc=0x%08lX estado=%d",static_cast<unsigned long>(rc),nifmReady&&R_SUCCEEDED(rc)?int(status):-1);networkHint=hint;
    // Nintendo connectivity tests are only diagnostic: verified HTTPS decides
    // whether the publication is reachable.
    if(!curlReady){if(curl_global_init(CURL_GLOBAL_DEFAULT)!=CURLE_OK){error="No se puede iniciar HTTPS.";return false;}curlReady=true;}
    return true;
}
bool validDestination(const std::string &path,std::string &error){
    for(const char *parent:{"sdmc:/config","sdmc:/config/NinMagicSaves",Root}){struct stat st{};if(lstat(parent,&st)!=0||!S_ISDIR(st.st_mode)||S_ISLNK(st.st_mode)){error="La carpeta de actualizaciones no es un directorio normal.";return false;}}
    struct stat st{};if(lstat(path.c_str(),&st)==0){if(!S_ISREG(st.st_mode)||S_ISLNK(st.st_mode)){error="El destino de la descarga no es un archivo regular.";return false;}}
    else if(errno!=ENOENT){error="No se puede revisar el archivo temporal de actualización.";return false;}
    return true;
}
#endif
}
bool downloadHttpsFile(const std::string &url,const std::string &path,std::string &error){
    error.clear();const uint64_t limit=bodyLimit(path);
    if(!safeUrl(url)){error="La actualización necesita una URL HTTPS válida.";return false;}
    if(!limit){error="La descarga solo admite los archivos temporales privados del actualizador.";return false;}
#ifdef __SWITCH__
    if(!validDestination(path,error)||!networkInit(error))return false;
    CURL *curl=curl_easy_init();if(!curl){error="No se puede iniciar la consulta HTTPS.";return false;}
    int flags=O_WRONLY|O_CREAT|O_TRUNC;
#ifdef O_NOFOLLOW
    flags|=O_NOFOLLOW;
#endif
    const int fd=open(path.c_str(),flags,0600);FILE *file=fd<0?nullptr:fdopen(fd,"wb");
    if(!file){if(fd>=0)close(fd);curl_easy_cleanup(curl);error="No se puede guardar la descarga en la microSD.";return false;}
    Download d{file,0,limit,false};char detail[CURL_ERROR_SIZE]{};
    const auto set=[&](CURLoption key,auto value){return curl_easy_setopt(curl,key,value)==CURLE_OK;};
    const bool configured=set(CURLOPT_URL,url.c_str())&&set(CURLOPT_USERAGENT,"NinMagic-Saves")&&
        set(CURLOPT_FOLLOWLOCATION,1L)&&set(CURLOPT_MAXREDIRS,5L)&&
        set(CURLOPT_PROTOCOLS,long(CURLPROTO_HTTPS))&&set(CURLOPT_REDIR_PROTOCOLS,long(CURLPROTO_HTTPS))&&
        set(CURLOPT_SSL_VERIFYPEER,1L)&&set(CURLOPT_SSL_VERIFYHOST,2L)&&
        set(CURLOPT_CONNECTTIMEOUT,15L)&&set(CURLOPT_TIMEOUT,limit==4096?40L:180L)&&
        set(CURLOPT_LOW_SPEED_LIMIT,512L)&&set(CURLOPT_LOW_SPEED_TIME,20L)&&
        set(CURLOPT_NOSIGNAL,1L)&&set(CURLOPT_FAILONERROR,1L)&&
        set(CURLOPT_MAXFILESIZE_LARGE,curl_off_t(limit))&&set(CURLOPT_WRITEFUNCTION,receive)&&
        set(CURLOPT_WRITEDATA,&d)&&set(CURLOPT_ERRORBUFFER,detail);
    const CURLcode result=configured?curl_easy_perform(curl):CURLE_FAILED_INIT;
    long status=0;const bool response=curl_easy_getinfo(curl,CURLINFO_RESPONSE_CODE,&status)==CURLE_OK;
    curl_easy_cleanup(curl);
    bool ok=configured&&response&&result==CURLE_OK&&status==200&&d.written>0&&!d.overflow;
    if(fflush(file)!=0)ok=false;
    if(ok&&fsync(fileno(file))!=0)ok=false;
    if(fclose(file)!=0)ok=false;
    if(!ok){
        unlink(path.c_str());
        if(!configured)error="No se pudo configurar la conexión HTTPS verificada.";
        else if(d.overflow)error="La descarga supera el tamaño permitido para la actualización.";
        else if(result==CURLE_OK)error="El servidor no devolvió un archivo válido (HTTP "+std::to_string(status)+").";
        else error="No se pudo consultar la actualización por HTTPS: "+std::string(detail[0]?detail:curl_easy_strerror(result));
        if(!status&&!networkHint.empty())error+=" ("+networkHint+").";
    }
    return ok;
#else
    error="La descarga HTTPS solo está disponible en la consola.";return false;
#endif
}
void updateNetworkExit(){
#ifdef __SWITCH__
    if(curlReady){curl_global_cleanup();curlReady=false;}
    if(nifmReady){nifmExit();nifmReady=false;}
    if(socketsReady){socketExit();socketsReady=false;}
    networkHint.clear();
#endif
}
}
