#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "save_transfer.hpp"
#include "save_transfer_hash.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <sstream>
#include <string>
#include <unistd.h>

using namespace nmsaves;
namespace fs=std::filesystem;
static unsigned checks=0;
static void require(bool value,const std::string& label) { ++checks; if(!value) { std::cerr<<"FAIL: "<<label<<'\n'; std::exit(1); } }
static void put(const std::string& path,const std::string& value) { fs::create_directories(fs::path(path).parent_path()); std::ofstream out(path,std::ios::binary); out<<value; require(bool(out),"fixture write "+path); }
static std::string get(const std::string& path) { std::ifstream in(path,std::ios::binary); std::ostringstream out; out<<in.rdbuf(); return out.str(); }
static std::string sha(const std::string& s) { transfer_detail::Sha256 h; h.add(reinterpret_cast<const uint8_t*>(s.data()),s.size()); return h.finish(); }
static transfer_detail::SaveCapacityMetadata capacityMetadata(const SaveEntry& save,int64_t capacity) {
    transfer_detail::SaveCapacityMetadata metadata;
    metadata.titleId=save.titleId; metadata.userLow=save.userLow; metadata.userHigh=save.userHigh;
    metadata.kind=save.kind; metadata.index=save.index; metadata.rank=save.rank;
    metadata.dataSize=capacity; metadata.journalSize=16*1024*1024;
    return metadata;
}
static std::map<std::string,std::string> contents(const std::string& root) {
    std::map<std::string,std::string> result;
    for(const auto& entry:fs::recursive_directory_iterator(root)) result[fs::relative(entry.path(),root).generic_string()]=entry.is_directory()?"<directory>":get(entry.path().generic_string());
    return result;
}
struct Fake final : transfer_detail::Backend {
    TransferEnvironment env;
    SaveEntry source,target,mountedSave;
    std::string sourcePath,targetPath,mountedPath;
    std::function<bool(const char*,const std::string&)> fault;
    unsigned readMounts=0,writeMounts=0,saveCommits=0,sdCommits=0,envChecks=0;
    unsigned failCommit=0,failSd=0;
    uint64_t capacity=64*1024*1024;
    bool capacityKnown=true;
    unsigned readonlyCapacityQueries=0,writableCapacityQueries=0;
    std::function<void(transfer_detail::SaveCapacityMetadata&)> changeCapacityMetadata;
    bool failAllCommits=false,missingTarget=false,duplicateTarget=false,failEnumeration=false,mounted=false,writable=false;
    Fake() {
        source.titleId=0x0100000012345000ULL; source.saveDataId=101; source.userLow=11; source.userHigh=12; source.gameName="Juego de prueba"; source.userName="Usuario original";
        target=source; target.saveDataId=202; target.userLow=21; target.userHigh=22; target.userName="Otro usuario";
        env.mode=SaveEnvironment::SysMMC; env.offlineVerified=true; env.gamesClosed=true; env.instanceId=std::string(64,'a');
    }
    TransferEnvironment environment() override { ++envChecks; return env; }
    bool enumerate(std::vector<SaveEntry>& saves,std::string& error) override {
        saves.clear(); if(failEnumeration) { error="fixture enumeration failure"; return false; }
        if(env.mode==SaveEnvironment::SysMMC) saves.push_back(source);
        if(env.mode==SaveEnvironment::EmuMMC&&!missingTarget) { saves.push_back(target); if(duplicateTarget) saves.push_back(target); }
        return true;
    }
    bool mount(const SaveEntry& entry,bool write,std::string& path,std::string& error) override {
        if(mounted) { error="already mounted"; return false; }
        if(entry.saveDataId==source.saveDataId) { require(!write,"source never opened writable"); path=sourcePath; }
        else if(entry.saveDataId==target.saveDataId) path=targetPath;
        else return false;
        mountedPath=path; mountedSave=entry; mounted=true; writable=write; if(write) ++writeMounts; else ++readMounts; return true;
    }
    void unmount() override { mounted=false; writable=false; mountedPath.clear(); mountedSave={}; }
    bool saveCapacity(uint64_t& bytes,std::string& error) override {
        bytes=0;
        require(mounted&&mountedPath==targetPath,"capacity queries identify the mounted destination");
        if(writable) ++writableCapacityQueries; else ++readonlyCapacityQueries;
        if(!capacityKnown) { error="metadata capacity query failed"; return false; }
        auto metadata=capacityMetadata(mountedSave,static_cast<int64_t>(capacity));
        if(changeCapacityMetadata) changeCapacityMetadata(metadata);
        return transfer_detail::validatedSaveCapacity(mountedSave,metadata,bytes,error);
    }
    bool commitSave(std::string& error) override {
        require(mounted&&writable&&mountedPath==targetPath,"commits only mounted destination");
        ++saveCommits; if(failAllCommits||saveCommits==failCommit) { error="injected save commit failure"; return false; } return true;
    }
    bool commitSd(std::string& error) override { ++sdCommits; if(sdCommits==failSd) { error="injected SD flush failure"; return false; } return true; }
    bool checkpoint(const char* stage,const std::string& path) override { return !fault||fault(stage,path); }
};
struct Fixture {
    std::string root,store;
    Fake b;
    transfer_detail::Engine engine;
    std::map<std::string,std::string> originalSource,originalTarget;
    ExportEntry exported;
    explicit Fixture(const std::string& base,const std::string& name):root(base+"/"+name),store(root+"/store"),engine(b,store) {
        b.sourcePath=root+"/source"; b.targetPath=root+"/destination";
        put(b.sourcePath+"/progress.dat",std::string(150000,'Q'));
        put(b.sourcePath+"/slot/a.bin",std::string("\0\1\2\xff",4));
        put(b.sourcePath+"/slot/empty",""); fs::create_directories(b.sourcePath+"/empty-dir");
        put(b.targetPath+"/old.dat","old destination"); put(b.targetPath+"/sub/keep.dat","old nested destination"); fs::create_directories(b.targetPath+"/empty-old");
        originalSource=contents(b.sourcePath); originalTarget=contents(b.targetPath);
    }
    void exportGood() {
        auto result=engine.exportSave(b.source); require(result.ok,"valid export: "+result.message);
        std::string error; std::vector<ExportEntry> exports; require(engine.listExports(exports,error)&&exports.size()==1,"one verified export: "+error);
        exported=exports.front(); require(exported.files==3&&exported.bytes==150004,"manifest actual totals");
        require(contents(b.sourcePath)==originalSource,"export never changes source"); require(b.writeMounts==0&&b.saveCommits==0,"no destination write during export");
        b.env.mode=SaveEnvironment::EmuMMC;
    }
    RestorePlan prepareGood() { RestorePlan plan; auto r=engine.prepareRestore(exported,b.target,plan); require(r.ok&&plan.backupVerified,"backup before confirmation: "+r.message); require(contents(b.targetPath)==originalTarget,"preparation leaves destination untouched"); require(b.writeMounts==0,"backup uses readonly mount"); return plan; }
    std::vector<RecoveryEntry> recoveries() { std::vector<RecoveryEntry> list; std::string error; require(engine.listRecoveries(list,error),"recovery list readable: "+error); return list; }
};
int main() {
    char scratch[]="/tmp/nmsaves-transfer-test-XXXXXX";
    const char* created=mkdtemp(scratch); require(created!=nullptr,"isolated test directory");
    const std::string base=created;
    require(sha("")=="e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855","SHA256 empty vector");
    require(sha("abc")=="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad","SHA256 standard vector");
    {
        Fake f;
        auto metadata=capacityMetadata(f.target,64*1024*1024);
        metadata.journalSize=std::numeric_limits<int64_t>::max();
        uint64_t bytes=999; std::string error="previous error";
        require(transfer_detail::validatedSaveCapacity(f.target,metadata,bytes,error)&&bytes==64*1024*1024&&error.empty(),
                "capacity uses verified data area only, never adds even a huge journal");
        auto shared=f.target; shared.kind=SaveKind::Device; shared.userLow=shared.userHigh=0;
        require(transfer_detail::validatedSaveCapacity(shared,capacityMetadata(shared,1024),bytes,error)&&bytes==1024,
                "device save capacity supports its zero account without mapping to an account save");
        for(unsigned bad=0;bad<12;++bad) {
            auto changed=metadata; auto expected=f.target; bytes=999; error.clear();
            switch(bad) {
                case 0: ++changed.titleId; break;
                case 1: ++changed.userLow; break;
                case 2: ++changed.userHigh; break;
                case 3: changed.kind=SaveKind::Device; break;
                case 4: ++changed.index; break;
                case 5: ++changed.rank; break;
                case 6: changed.systemSaveDataId=1; break;
                case 7: changed.dataSize=-1; break;
                case 8: changed.dataSize=0; break;
                case 9: changed.journalSize=-1; break;
                case 10: expected.saveDataId=0; break;
                case 11: expected.titleId=0; break;
            }
            require(!transfer_detail::validatedSaveCapacity(expected,changed,bytes,error)&&bytes==0&&!error.empty(),
                    "wrong identity or invalid metadata fails closed without stale capacity "+std::to_string(bad));
        }
    }
    {
        std::string path;
        const auto file=transfer_detail::destinationConfigFingerprint(123,"[emummc]\nenabled=1\npath=custom/profile\n",path);
        require(file.size()==64&&path=="custom/profile","custom file profile with Hekate default sector/id accepted");
        require(file==transfer_detail::destinationConfigFingerprint(123,"[emummc]\nenabled=1\npath=custom/profile\nsector=0x0\nid=0\n",path),"equivalent defaults have same binding");
        require(file!=transfer_detail::destinationConfigFingerprint(124,"[emummc]\nenabled=1\npath=custom/profile\n",path),"another console has different opaque binding");
        require(file!=transfer_detail::destinationConfigFingerprint(123,"[emummc]\nenabled=1\npath=custom/profile\nsector=0x1234\n",path),"raw sector distinguishes profile");
        for(const auto& bad:std::vector<std::string>{"[emummc]\nenabled=1\npath=../bad\n","[emummc]\nenabled=1\npath=sdmc:/bad\n","[emummc]\nenabled=1\npath=good\nid=nope\n","[emummc]\nenabled=0\npath=good\n","[emummc]\nenabled=1\npath=good\npath=other\n","[other]\nenabled=1\npath=good\n"}) require(transfer_detail::destinationConfigFingerprint(123,bad,path).empty(),"invalid or ambiguous config binding rejected");
        require(transfer_detail::destinationConfigFingerprint(0,"[emummc]\nenabled=1\npath=good\n",path).empty(),"unknown console identity cannot bind destination");
    }
    {
        Fixture f(base,"roundtrip"); f.exportGood(); auto plan=f.prepareGood();
        require(plan.destination.userLow!=plan.exported.source.userLow,"explicit different-user mapping retained");
        require(fs::exists(f.store+"/backups/"+plan.backupId+"/complete.sha256"),"backup durable before confirmation");
        auto r=f.engine.applyRestore(plan); require(r.ok,"apply valid transfer: "+r.message);
        require(f.b.readonlyCapacityQueries==1&&f.b.writableCapacityQueries==1,
                "real capacity is checked through metadata during readonly preparation and reread before writes");
        require(contents(f.b.targetPath)==f.originalSource,"destination exactly replaced, stale files removed and empty directories retained");
        require(contents(f.b.sourcePath)==f.originalSource,"original unchanged after restore"); require(f.b.saveCommits==1,"one save commit on successful apply");
        require(!f.engine.applyRestore(plan).ok,"confirmed plan cannot apply twice");
        auto recoveries=f.recoveries(); require(recoveries.size()==1&&!recoveries.front().needsRecovery,"applied backup available for explicit revert");
        r=f.engine.recover(recoveries.front()); require(r.ok,"explicit revert succeeds"); require(contents(f.b.targetPath)==f.originalTarget,"rollback restores destination exactly");
        require(fs::exists(f.store+"/backups/"+plan.backupId+"/data/old.dat"),"backup not deleted by revert");
        require(!f.engine.recover(recoveries.front()).ok,"stale recovery confirmation rejected");
    }
    {
        Fixture f(base,"device"); f.b.source.kind=f.b.target.kind=SaveKind::Device; f.b.source.userLow=f.b.source.userHigh=f.b.target.userLow=f.b.target.userHigh=0;
        f.exportGood(); auto plan=f.prepareGood(); require(f.engine.applyRestore(plan).ok,"device saves map by exact title/type/index, no account");
    }
    for(unsigned mode=0;mode<4;++mode) {
        Fixture f(base,"export-gate-"+std::to_string(mode));
        if(mode==0) f.b.env.mode=SaveEnvironment::Unknown;
        if(mode==1) f.b.env.mode=SaveEnvironment::EmuMMC;
        if(mode==2) f.b.env.gamesClosed=false;
        if(mode==3) f.b.failEnumeration=true;
        require(!f.engine.exportSave(f.b.source).ok,"unsafe export gate rejected "+std::to_string(mode)); require(f.b.readMounts==0&&f.b.writeMounts==0,"no FS mount before export gate");
        if(mode!=1) { std::vector<SaveEntry> saves; std::string error; require(!f.engine.listSaves(saves,error)&&saves.empty(),"unknown runtime/game open/enumeration failure still blocks save list"); }
    }
    for(const auto& network:std::vector<std::string>{"Wi-Fi enabled","Network connected","Network state unknown"}) {
        Fixture f(base,"network-warning-"+std::to_string(checks)); f.b.env.offlineVerified=false; f.b.env.networkLabel=network;
        std::vector<SaveEntry> saves; std::string error;
        require(f.engine.listSaves(saves,error)&&saves.size()==1&&saves.front().saveDataId==f.b.source.saveDataId,"sysMMC save list permits network warning: "+network);
        f.exportGood();
        require(!f.b.env.offlineVerified&&f.b.env.networkLabel==network,"export preserves network diagnostic without changing connectivity");
    }
    {
        Fixture f(base,"network-changes"); unsigned chunks=0;
        f.b.fault=[&](const char* stage,const std::string&){ if(std::string(stage)=="copy-chunk"&&++chunks==1) f.b.env.offlineVerified=false; return true; };
        require(f.engine.exportSave(f.b.source).ok,"Wi-Fi/network change during copy is informational and does not interrupt export");
        std::vector<ExportEntry> list; std::string error; require(f.engine.listExports(list,error)&&list.size()==1,"completed export remains verified after network status change");
        require(contents(f.b.sourcePath)==f.originalSource&&f.b.writeMounts==0,"network warning never permits source writes");
    }
    {
        Fixture f(base,"interrupted-export");
        f.b.fault=[](const char* stage,const std::string&){ return std::string(stage)!="copy-chunk"; };
        require(!f.engine.exportSave(f.b.source).ok,"actual copy interruption still stops export");
        std::vector<ExportEntry> list; std::string error; require(f.engine.listExports(list,error)&&list.empty(),"interrupted copy never appears complete"); require(contents(f.b.sourcePath)==f.originalSource,"source untouched after interrupted export");
    }
    for(unsigned changed=0;changed<2;++changed) {
        Fixture f(base,"runtime-changes-"+std::to_string(changed)); unsigned chunks=0;
        f.b.fault=[&](const char* stage,const std::string&){ if(std::string(stage)=="copy-chunk"&&++chunks==1) { if(changed==0) f.b.env.mode=SaveEnvironment::Unknown; else f.b.env.gamesClosed=false; } return true; };
        require(!f.engine.exportSave(f.b.source).ok,"runtime or game status change still interrupts export");
        std::vector<ExportEntry> list; std::string error; require(f.engine.listExports(list,error)&&list.empty(),"runtime check interruption cannot produce an importable export");
        require(contents(f.b.sourcePath)==f.originalSource,"runtime check failure leaves source unchanged");
    }
    {
        Fixture f(base,"unique-users"); auto one=f.engine.exportSave(f.b.source); require(one.ok,"first source save");
        f.b.source.userLow=99; f.b.source.saveDataId=303; auto two=f.engine.exportSave(f.b.source); require(two.ok&&one.id!=two.id,"same title second account gets unique export");
        std::vector<ExportEntry> list; std::string error; require(f.engine.listExports(list,error)&&list.size()==2,"source accounts never merged");
    }
    for(unsigned kind=0;kind<5;++kind) {
        Fixture f(base,"mapping-"+std::to_string(kind)); f.exportGood(); auto wrong=f.b.target;
        if(kind==0) ++wrong.titleId;
        if(kind==1) { wrong.kind=SaveKind::Device; wrong.userLow=wrong.userHigh=0; }
        if(kind==2) ++wrong.index;
        if(kind==3) ++wrong.userLow;
        if(kind==4) ++wrong.saveDataId;
        RestorePlan plan; require(!f.engine.prepareRestore(f.exported,wrong,plan).ok,"wrong title/type/index/user/save rejected "+std::to_string(kind)); require(f.b.writeMounts==0,"mapping failure before writing");
    }
    for(unsigned kind=0;kind<5;++kind) {
        Fixture f(base,"restore-gate-"+std::to_string(kind)); f.exportGood();
        if(kind==0) f.b.env.mode=SaveEnvironment::SysMMC;
        if(kind==1) f.b.env.gamesClosed=false;
        if(kind==2) f.b.env.instanceId.clear();
        if(kind==3) f.b.missingTarget=true;
        if(kind==4) f.b.duplicateTarget=true;
        RestorePlan plan; require(!f.engine.prepareRestore(f.exported,f.b.target,plan).ok,"unsafe restore gate rejected "+std::to_string(kind)); require(f.b.writeMounts==0,"no destination write on bad gate");
    }
    for(unsigned corruption=0;corruption<4;++corruption) {
        Fixture f(base,"corrupt-"+std::to_string(corruption)); f.exportGood(); const auto package=f.store+"/exports/"+f.exported.id;
        if(corruption==0) put(package+"/data/progress.dat","corrupted");
        if(corruption==1) put(package+"/manifest.txt",get(package+"/manifest.txt")+"bad\n");
        if(corruption==2) put(package+"/complete.sha256",std::string(64,'0')+"\n");
        if(corruption==3) put(package+"/data/unlisted.dat","not in manifest");
        std::vector<ExportEntry> list; std::string error; require(f.engine.listExports(list,error)&&list.empty()&&!error.empty(),"corrupted export rejected "+std::to_string(corruption));
        RestorePlan plan; require(!f.engine.prepareRestore(f.exported,f.b.target,plan).ok,"corrupt copy cannot prepare import");
    }
    {
        Fixture f(base,"path-traversal"); f.exportGood(); const auto package=f.store+"/exports/"+f.exported.id;
        auto text=get(package+"/manifest.txt"); const auto p=text.find("656d7074792d646972"); require(p!=std::string::npos,"fixture manifest directory"); text.replace(p,18,"2e2e2f6576696c");
        put(package+"/manifest.txt",text); put(package+"/complete.sha256",sha(text)+"\n");
        RestorePlan plan; require(!f.engine.prepareRestore(f.exported,f.b.target,plan).ok,"path traversal rejected even with recomputed manifest checksum");
    }
    {
        Fixture f(base,"symlink"); put(f.root+"/outside","outside sentinel");
        require(symlink((f.root+"/outside").c_str(),(f.b.sourcePath+"/escape").c_str())==0,"fixture symlink created");
        require(!f.engine.exportSave(f.b.source).ok,"source symlink rejected"); require(get(f.root+"/outside")=="outside sentinel","no link target changed");
    }
    {
        Fixture f(base,"stale-destination"); f.exportGood(); auto plan=f.prepareGood(); put(f.b.targetPath+"/old.dat","new progress after backup");
        auto r=f.engine.applyRestore(plan); require(!r.ok&&!r.recoveryRequired,"stale destination blocks apply"); require(get(f.b.targetPath+"/old.dat")=="new progress after backup","new destination progress untouched"); require(f.b.saveCommits==0,"no commit on stale destination");
    }
    for(unsigned tamper=0;tamper<4;++tamper) {
        Fixture f(base,"plan-"+std::to_string(tamper)); f.exportGood(); auto plan=f.prepareGood();
        if(tamper==0) ++plan.destination.userLow;
        if(tamper==1) ++plan.destination.titleId;
        if(tamper==2) plan.backupVerified=false;
        if(tamper==3) plan.backupSha256=std::string(64,'0');
        require(!f.engine.applyRestore(plan).ok,"tampered confirmed plan rejected "+std::to_string(tamper)); require(contents(f.b.targetPath)==f.originalTarget,"plan failure leaves target intact");
    }
    {
        Fixture f(base,"different-instance"); f.exportGood(); auto plan=f.prepareGood(); f.b.env.instanceId=std::string(64,'b');
        require(!f.engine.applyRestore(plan).ok,"same users and title on other emuMMC cannot apply prepared restore"); require(f.b.writeMounts==0,"other instance rejected before writable mount");
        f.b.env.instanceId=std::string(64,'a'); require(f.engine.applyRestore(plan).ok,"original emuMMC accepts plan"); auto entries=f.recoveries();
        f.b.env.instanceId=std::string(64,'b'); require(!f.engine.recover(entries.front()).ok,"backup recovery bound to same destination instance");
    }
    {
        Fixture f(base,"backup-corrupt"); f.exportGood(); auto plan=f.prepareGood(); put(f.store+"/backups/"+plan.backupId+"/data/old.dat","damage");
        require(!f.engine.applyRestore(plan).ok,"damaged mandatory backup blocks before mutation"); require(f.b.writeMounts==0,"bad backup no writable mount");
    }
    for(unsigned kind=0;kind<4;++kind) {
        Fixture f(base,"capacity-"+std::to_string(kind)); f.exportGood();
        if(kind==0) f.b.capacity=32;
        else if(kind==1) f.b.capacityKnown=false;
        else if(kind==2) f.b.changeCapacityMetadata=[](transfer_detail::SaveCapacityMetadata& m){ ++m.userHigh; };
        else f.b.changeCapacityMetadata=[](transfer_detail::SaveCapacityMetadata& m){ m.dataSize=-1; };
        RestorePlan plan; require(!f.engine.prepareRestore(f.exported,f.b.target,plan).ok,"insufficient, unknown or mismatched target capacity rejected");
        require(f.b.writeMounts==0&&f.b.saveCommits==0&&contents(f.b.targetPath)==f.originalTarget,"capacity failure does not change destination");
        require(!plan.backupVerified&&plan.id.empty()&&fs::is_empty(f.store+"/backups"),"failed capacity check cannot produce a confirmed plan or partial destination backup");
        require(contents(f.b.sourcePath)==f.originalSource,"capacity failure preserves source export data");
    }
    for(unsigned kind=0;kind<4;++kind) {
        Fixture f(base,"capacity-after-confirm-"+std::to_string(kind)); f.exportGood(); const auto plan=f.prepareGood();
        const auto originalBackup=contents(f.store+"/backups/"+plan.backupId);
        if(kind==0) f.b.capacity=32;
        else if(kind==1) f.b.capacityKnown=false;
        else if(kind==2) f.b.changeCapacityMetadata=[](transfer_detail::SaveCapacityMetadata& m){ ++m.titleId; };
        else f.b.changeCapacityMetadata=[](transfer_detail::SaveCapacityMetadata& m){ m.dataSize=-1; };
        auto result=f.engine.applyRestore(plan);
        require(!result.ok&&!result.recoveryRequired,"capacity/identity changes after review reject application before mutation");
        require(f.b.saveCommits==0&&contents(f.b.targetPath)==f.originalTarget,"second capacity check leaves destination contents untouched");
        require(contents(f.store+"/backups/"+plan.backupId)==originalBackup,"second capacity failure retains verified destination backup unchanged");
        require(contents(f.b.sourcePath)==f.originalSource,"second capacity failure never writes source");
        require(f.b.readonlyCapacityQueries==1&&f.b.writableCapacityQueries==1,"capacity after confirmation is queried again instead of reused");
        f.b.capacity=64*1024*1024; f.b.capacityKnown=true; f.b.changeCapacityMetadata={};
        require(f.engine.applyRestore(plan).ok,"same export and prepared backup can retry once verified capacity is available");
    }
    for(unsigned fault=0;fault<3;++fault) {
        Fixture f(base,"rollback-"+std::to_string(fault)); f.exportGood(); auto plan=f.prepareGood(); bool fired=false;
        f.b.fault=[&](const char* stage,const std::string& path){ bool match=(fault==0&&std::string(stage)=="remove-entry")||(fault==1&&std::string(stage)=="copy-chunk"&&path.rfind(f.b.targetPath+"/",0)==0); if(match&&!fired) { fired=true; return false; } return true; };
        if(fault==2) f.b.failCommit=1;
        auto r=f.engine.applyRestore(plan); require(!r.ok&&!r.recoveryRequired,"write/delete/commit failure safely rolled back "+std::to_string(fault)+": "+r.message);
        require(contents(f.b.targetPath)==f.originalTarget,"failed apply restores original target contents"); require(contents(f.b.sourcePath)==f.originalSource,"failure never changes source");
    }
    {
        Fixture f(base,"rollback-failure"); f.exportGood(); auto plan=f.prepareGood();
        f.b.fault=[&](const char* stage,const std::string& path){ return !(std::string(stage)=="copy-chunk"&&path.rfind(f.b.targetPath+"/",0)==0); };
        auto r=f.engine.applyRestore(plan); require(!r.ok&&r.recoveryRequired,"failed rollback leaves explicit recovery required");
        RestorePlan another; require(!f.engine.prepareRestore(f.exported,f.b.target,another).ok,"pending recovery prevents another import");
        f.b.fault={}; transfer_detail::Engine reopened(f.b,f.store); std::vector<RecoveryEntry> pending; std::string error;
        require(reopened.listRecoveries(pending,error)&&pending.size()==1&&pending[0].needsRecovery,"durable journal survives engine restart");
        require(reopened.recover(pending[0]).ok,"explicit recovery after restart succeeds"); require(contents(f.b.targetPath)==f.originalTarget,"restart recovery restores original bytes");
        require(fs::exists(f.store+"/exports/"+f.exported.id+"/data/progress.dat"),"source export remains available");
    }
    {
        Fixture f(base,"commit-failure"); f.exportGood(); auto plan=f.prepareGood(); f.b.failAllCommits=true;
        auto r=f.engine.applyRestore(plan); require(!r.ok&&r.recoveryRequired,"persistent save commit failure never reports success");
        f.b.failAllCommits=false; auto pending=f.recoveries(); require(f.engine.recover(pending.front()).ok,"commit retry recovery works");
    }
    {
        Fixture f(base,"orphan-preparation"); f.exportGood(); bool fired=false;
        f.b.fault=[&](const char* stage,const std::string& value){ if(std::string(stage)=="journal"&&value=="prepared"&&!fired) { fired=true; return false; } return true; };
        RestorePlan plan; require(!f.engine.prepareRestore(f.exported,f.b.target,plan).ok,"preparation interruption before first durable record");
        require(f.recoveries().empty(),"orphan preparation does not create false recovery"); f.b.fault={}; plan=f.prepareGood(); require(f.engine.applyRestore(plan).ok,"orphan preparation never blocks later valid import");
    }
    {
        Fixture f(base,"journal-partial"); f.exportGood(); auto plan=f.prepareGood();
        // Simulate a process that died with an incomplete future journal record.
        put(f.store+"/transactions/"+plan.id+"/00000001.record.partial-abandoned","half a record");
        require(f.engine.applyRestore(plan).ok,"abandoned unique temp cannot strand next journal sequence");
    }
    {
        Fixture f(base,"journal-post-rename-flush"); f.exportGood(); auto plan=f.prepareGood(); f.b.failSd=f.b.sdCommits+2;
        auto result=f.engine.applyRestore(plan); require(!result.ok,"SD failure after journal rename not reported as successful apply");
        require(contents(f.b.targetPath)==f.originalTarget,"no target mutation when applying record not durably flushed");
        f.b.failSd=0; auto pending=f.recoveries(); require(pending.size()==1&&pending.front().needsRecovery,"conservative recovery record after uncertain SD flush");
        require(f.engine.recover(pending.front()).ok,"uncertain journal flush recoverable on retry");
    }
    {
        Fixture f(base,"journal-applied-failure"); f.exportGood(); auto plan=f.prepareGood();
        f.b.fault=[](const char* stage,const std::string& value){ return !(std::string(stage)=="journal"&&value=="applied"); };
        auto r=f.engine.applyRestore(plan); require(!r.ok&&r.recoveryRequired,"applied data without final journal requires recovery");
        require(contents(f.b.targetPath)==f.originalSource,"fixture data really committed before final metadata failure");
        f.b.fault={}; auto pending=f.recoveries(); require(f.engine.recover(pending.front()).ok,"recover after final journal failure"); require(contents(f.b.targetPath)==f.originalTarget,"pre-import backup survives late journal failure");
    }
    {
        Fixture f(base,"journal-corrupt"); f.exportGood(); auto plan=f.prepareGood(); put(f.store+"/transactions/"+plan.id+"/00000000.record","corrupt");
        require(!f.engine.applyRestore(plan).ok,"corrupt journal never permits a restore"); require(contents(f.b.targetPath)==f.originalTarget,"corrupt journal destination untouched");
    }
    {
        Fixture f(base,"journal-gap"); f.exportGood(); auto plan=f.prepareGood(); require(f.engine.applyRestore(plan).ok,"gap fixture applied");
        const auto journal=f.store+"/transactions/"+plan.id;
        require(unlink((journal+"/00000001.record").c_str())==0,"remove middle journal record to model damaged directory entry");
        std::vector<RecoveryEntry> entries; std::string error;
        require(!f.engine.listRecoveries(entries,error),"journal 0+2 is rejected, not interpreted as prepared");
        require(!f.engine.applyRestore(plan).ok,"gapped journal cannot reopen old confirmation");
        require(contents(f.b.targetPath)==f.originalSource,"gap failure does not overwrite already imported data");
    }
    {
        Fixture f(base,"journal-invalid-transition"); f.exportGood(); auto plan=f.prepareGood(); require(f.engine.applyRestore(plan).ok,"transition fixture applied");
        const auto path=f.store+"/transactions/"+plan.id+"/00000001.record";
        auto record=get(path); const auto pos=record.find("\napplying\n"); require(pos!=std::string::npos,"journal transition marker");
        record.replace(pos,10,"\nrestored\n"); record.resize(record.size()-65); record+=sha(record)+"\n"; put(path,record);
        std::vector<RecoveryEntry> entries; std::string error; require(!f.engine.listRecoveries(entries,error),"invalid state transition rejected even with recomputed checksum");
    }
    std::cout<<"save_transfer: "<<checks<<" checks passed. Fixtures retained in "<<base<<"\n";
}
