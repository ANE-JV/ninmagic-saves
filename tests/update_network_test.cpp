#define NM_UPDATE_NETWORK_TEST
#include "../source/update_network.cpp"
#include <iostream>
#include <stdexcept>

namespace {unsigned checks=0;void check(bool value){++checks;if(!value)throw std::runtime_error("update HTTPS policy check");}}
int main(){try{
    for(const std::string url:{"https://raw.githubusercontent.com/ANE-JV/ninmagic-saves/main/update.txt","https://example.test:443/versions/v0.1.0/NinMagic-Saves.nro"})check(nm::safeUrl(url));
    for(const std::string url:{"http://example.test/update.txt","https://u:p@example.test/update.txt","https:///update.txt","https://example.test/a#fragment","https://example.test/a b","https://example.test/a\nb","https://example.test/a\\b"})check(!nm::safeUrl(url));
    check(!nm::safeUrl(std::string(2049,'a')));
    check(nm::bodyLimit("sdmc:/config/NinMagicCleaner/app-update/manifest.pending")==0);
    check(nm::bodyLimit("sdmc:/config/NinMagicMods/app-update/download.pending")==0);
    check(nm::bodyLimit("sdmc:/config/NinMagicSaves/app-update/manifest.pending")==4096);
    check(nm::bodyLimit("sdmc:/config/NinMagicSaves/app-update/download.pending")==16ULL*1024*1024);
    for(const std::string path:{"sdmc:/config/NinMagicSaves/bindings.bin","sdmc:/config/NinMagicCheats/app-update/manifest.pending","sdmc:/switch/NinMagicSaves/NinMagic-Saves.nro","sdmc:/config/NinMagicSaves/app-update/../bindings.bin","sdmc:/Nintendo/Contents/any"})check(nm::bodyLimit(path)==0);
    FILE *file=std::tmpfile();check(file!=nullptr);nm::Download d{file,0,5,false};char bytes[]="abcdef";
    check(nm::receive(bytes,1,3,&d)==3&&d.written==3&&!d.overflow);
    check(nm::receive(bytes+3,1,2,&d)==2&&d.written==5&&!d.overflow);
    check(nm::receive(bytes,1,1,&d)==0&&d.written==5&&d.overflow);
    check(std::fflush(file)==0&&std::fseek(file,0,SEEK_END)==0&&std::ftell(file)==5);check(std::fclose(file)==0);
    file=std::tmpfile();check(file!=nullptr);d={file,0,5,false};check(nm::receive(bytes,SIZE_MAX,2,&d)==0&&d.written==0);check(std::fclose(file)==0);
    std::string error="stale";check(!nm::downloadHttpsFile("https://example.test/update.txt","sdmc:/config/NinMagicSaves/bindings.bin",error)&&error.find("temporales")!=error.npos);
    check(!nm::downloadHttpsFile("http://example.test/update.txt","sdmc:/config/NinMagicSaves/app-update/manifest.pending",error)&&error.find("HTTPS válida")!=error.npos);
    nm::updateNetworkExit();std::cout<<"Saves HTTPS: "<<checks<<" URL, path isolation and bounded receive checks PASS\n";return 0;
}catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}}
