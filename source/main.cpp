#include <switch.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include "ui.hpp"
#include "save_transfer.hpp"
#include "reboot_workflow.hpp"
#include "update_network.hpp"

namespace {
using namespace nmsaves;
enum class JobKind { Inspect, Sources, Exports, Targets, Export, PrepareImport, Import, Recoveries, Recover, ReviewReboot, PrepareAccess, Reboot, CheckUpdate, PrepareUpdate, InstallUpdate, ArchiveRecord };
struct Job {
    JobKind kind=JobKind::Inspect;
    TransferEnvironment environment;RuntimeEnvironment runtime;
    std::vector<SaveEntry> saves,selectedSaves;
    std::vector<ExportEntry> exports;std::vector<RecoveryEntry> recoveries;
    ExportEntry selectedExport;SaveEntry selectedTarget;RestorePlan importPlan;RecoveryEntry selectedRecovery;
    RebootWorkflow workflow;RebootPlan rebootPlan;RebootTarget rebootTarget=RebootTarget::CfwSysMMC;
    bool needsAccess=false,networkNoticeAcknowledged=false;
    nm::AppUpdateInfo update;nm::CompletedUpdateInfo updateRecord;
    std::string error,message,recoveryError;
    bool ok=false,active=false;std::atomic<bool> done{false};std::atomic<uint64_t> progress{0};Thread thread{};
} job;
bool sameEnvironment(const TransferEnvironment&e,const RuntimeEnvironment&r){
    return r.atmosphere&&((e.mode==SaveEnvironment::EmuMMC&&r.mode==RuntimeMode::EmuMMC)||(e.mode==SaveEnvironment::SysMMC&&r.mode==RuntimeMode::SysMMC));
}
bool requireEnvironment(SaveEnvironment wanted){
    job.environment=inspectTransferEnvironment();job.runtime=inspectRuntimeEnvironment();
    if(!sameEnvironment(job.environment,job.runtime)||job.environment.mode!=wanted){job.error="El entorno no coincide con la operación. Vuelve a comprobarlo antes de continuar.";return false;}
    return true;
}
bool requireRebootEnvironment() {
    const auto wanted=job.rebootTarget==RebootTarget::CfwSysMMC?SaveEnvironment::EmuMMC:SaveEnvironment::SysMMC;
    if(!requireEnvironment(wanted))return false;
    if(!job.environment.gamesClosed){job.error="Cierra el juego antes de preparar o confirmar un reinicio.";return false;}
    return true;
}
void setTransferResult(const TransferResult&r){job.ok=r.ok;job.message=r.message;if(!r.ok)job.error=r.message;}
void reviewReboot(){
    job.needsAccess=false;job.workflow=inspectRebootWorkflow();job.ok=prepareRebootPlan(job.rebootTarget,job.rebootPlan,job.error);
    const bool targetMissing=std::none_of(job.workflow.entries.begin(),job.workflow.entries.end(),[](const BootEntry&e){return e.target==job.rebootTarget;});
    if(!job.ok&&targetMissing&&job.workflow.configurationValid&&job.workflow.canPrepareAccess&&
       !job.workflow.ownedConfigurationPresent&&job.workflow.runtime.gamesClosed){
        job.needsAccess=true;job.ok=true;job.error.clear();
    }
}
void work(void*){
    job.error.clear();job.message.clear();job.ok=false;
    switch(job.kind){
        case JobKind::Inspect:
            job.environment=inspectTransferEnvironment();job.runtime=inspectRuntimeEnvironment();job.recoveryError.clear();
            if(sameEnvironment(job.environment,job.runtime)&&job.environment.mode==SaveEnvironment::EmuMMC)nm::recoverAppUpdate(AppPath,job.recoveryError);
            job.ok=true;break;
        case JobKind::Sources:
            job.saves.clear();if(requireEnvironment(SaveEnvironment::SysMMC))job.ok=listTransferSaves(job.saves,job.error);break;
        case JobKind::Exports:
            job.exports.clear();if(requireEnvironment(SaveEnvironment::EmuMMC))job.ok=listTransferExports(job.exports,job.error);break;
        case JobKind::Targets:
            job.saves.clear();if(requireEnvironment(SaveEnvironment::EmuMMC)){job.ok=listTransferSaves(job.saves,job.error);if(job.ok)job.saves.erase(std::remove_if(job.saves.begin(),job.saves.end(),[](const SaveEntry&s){return !compatibleTransferTarget(job.selectedExport.source,s);}),job.saves.end());}
                        break;
        case JobKind::Export:{
            if(!requireEnvironment(SaveEnvironment::SysMMC))break;
            unsigned copied=0;for(const auto&s:job.selectedSaves){auto r=exportTransferSave(s);if(!r.ok){job.error="Copiadas "+std::to_string(copied)+" de "+std::to_string(job.selectedSaves.size())+". "+r.message;break;}job.progress.store(++copied,std::memory_order_relaxed);}
            job.ok=copied==job.selectedSaves.size()&&!job.selectedSaves.empty();
            if(job.ok)job.message="Se copiaron "+std::to_string(copied)+" partidas a la microSD. El origen se conserva. Vuelve a emuMMC desde el menú principal y abre NinMagic Saves para importarlas.";
            break;
        }
        case JobKind::PrepareImport:job.importPlan={};if(requireEnvironment(SaveEnvironment::EmuMMC))setTransferResult(prepareTransferRestore(job.selectedExport,job.selectedTarget,job.importPlan));break;
        case JobKind::Import:if(requireEnvironment(SaveEnvironment::EmuMMC))setTransferResult(applyTransferRestore(job.importPlan));break;
        case JobKind::Recoveries:job.recoveries.clear();if(requireEnvironment(SaveEnvironment::EmuMMC))job.ok=listTransferRecoveries(job.recoveries,job.error);break;
        case JobKind::Recover:if(requireEnvironment(SaveEnvironment::EmuMMC))setTransferResult(recoverTransferRestore(job.selectedRecovery));break;
        case JobKind::ReviewReboot:if(requireRebootEnvironment())reviewReboot();break;
        case JobKind::PrepareAccess:if(requireRebootEnvironment()&&prepareRebootAccess(true,job.networkNoticeAcknowledged,job.error))reviewReboot();break;
        case JobKind::Reboot:if(!requireRebootEnvironment())break;job.ok=requestReboot(job.rebootPlan,true,job.networkNoticeAcknowledged,job.error);job.message="Se solicitó el reinicio. Abre NinMagic Saves manualmente desde hbmenu al arrancar.";break;
        case JobKind::CheckUpdate:{if(!requireEnvironment(SaveEnvironment::EmuMMC))break;job.ok=nm::checkAppUpdate(job.update,job.error);std::string ignored;nm::canArchiveCompletedUpdate(AppPath,job.updateRecord,ignored);break;}
        case JobKind::PrepareUpdate:if(requireEnvironment(SaveEnvironment::EmuMMC))job.ok=nm::prepareAppUpdate(job.update,AppPath,job.error);break;
        case JobKind::InstallUpdate:{
            if(!requireEnvironment(SaveEnvironment::EmuMMC))break;
            job.ok=nm::installPreparedAppUpdate(job.update,AppPath,job.error);std::string ignored;nm::canArchiveCompletedUpdate(AppPath,job.updateRecord,ignored);
            if(!job.updateRecord.currentVersion.empty()&&!job.updateRecord.currentSha256.empty()){job.update.sdVersion=job.updateRecord.currentVersion;job.update.sdSha256=job.updateRecord.currentSha256;job.update.sdSize=job.updateRecord.currentSize;job.update.sdVerified=true;}
                        break;
        }
        case JobKind::ArchiveRecord:if(requireEnvironment(SaveEnvironment::EmuMMC))job.ok=nm::archiveCompletedUpdate(AppPath,job.updateRecord,job.error);break;
    }job.done.store(true,std::memory_order_release);
}
bool start(JobKind kind,View&v,const std::string&message){
    if(job.active)return false;
    job.kind=kind;job.update=v.update;job.updateRecord=v.updateRecord;job.error.clear();job.progress.store(0);job.done.store(false);
    Result rc=threadCreate(&job.thread,work,nullptr,nullptr,512*1024,0x2C,-2);
    if(R_SUCCEEDED(rc)){rc=threadStart(&job.thread);if(R_FAILED(rc))threadClose(&job.thread);}
    if(R_FAILED(rc)){char error[100];std::snprintf(error,sizeof(error),"No se pudo iniciar la operación (0x%08lX).",static_cast<unsigned long>(rc));v.message=error;v.success=false;v.screen=Screen::Result;return false;}
    job.active=true;v.message=message;v.screen=Screen::Busy;v.progressKnown=kind==JobKind::Export;v.progressTotal=job.selectedSaves.size();v.progressCurrent=0;appletSetAutoSleepDisabled(true);return true;
}
void applyEnvironment(View&v){
    v.environment=UiEnvironment::Unknown;if(sameEnvironment(job.environment,job.runtime))v.environment=job.environment.mode==SaveEnvironment::EmuMMC?UiEnvironment::EmuMmc:UiEnvironment::SysMmc;
    v.offline=job.environment.offlineVerified&&job.runtime.offline;v.atmosphere=job.runtime.atmosphere?"Atmosphère confirmado":"Atmosphère no confirmado";v.environmentDetail=job.environment.detail;
    if(v.environment==UiEnvironment::Unknown)v.environmentDetail="No se pudo confirmar de forma coherente el entorno. No se abrirán partidas ni se iniciará la red.";
    if(v.environment==UiEnvironment::SysMmc)v.environmentDetail="Antes de exportar, activa modo avión en el sistema original y desconecta Ethernet. La detección de red es informativa; no impide continuar.";
}
std::string profile(const SaveEntry&s){return s.kind==SaveKind::Device?"Datos compartidos de la consola":s.userName.empty()?"Perfil sin nombre disponible":s.userName;}
std::string saveSubtitle(const SaveEntry&s){return s.kind==SaveKind::Device?"Datos compartidos de la consola":"Partida de usuario / "+profile(s);}
std::string game(const SaveEntry&s){return s.gameName.empty()?"Juego "+transferTitleId(s.titleId):s.gameName;}
std::string accountIdentity(const SaveEntry&s){return s.kind==SaveKind::Device?"Tipo: dispositivo; sin perfil individual":"Partida de usuario | UID: "+transferTitleId(s.userLow)+transferTitleId(s.userHigh);}
UiRow saveRow(const SaveEntry&s){return {game(s),saveSubtitle(s),"Title ID: "+transferTitleId(s.titleId)+". "+(s.kind==SaveKind::Device?accountIdentity(s):"Perfil: "+profile(s)+". "+accountIdentity(s))+". Save ID: "+transferTitleId(s.saveDataId),false};}
void moveSelection(View&v,u64 buttons){
    const size_t count=v.screen==Screen::Home?homeActions(v.environment).size():v.rows.size();if(!count){v.selected=0;return;}
    if(buttons&(HidNpadButton_Down|HidNpadButton_StickLDown))v.selected=(v.selected+1)%count;
    if(buttons&(HidNpadButton_Up|HidNpadButton_StickLUp))v.selected=(v.selected+count-1)%count;
}
void showReboot(View&v){
    v.rebootToOriginal=job.rebootTarget==RebootTarget::CfwSysMMC;v.automaticReboot=job.rebootPlan.automatic;v.rebootDetails.clear();
    if(job.needsAccess){v.screen=Screen::ConfigureReboot;v.rebootDetails.push_back("Crea accesos separados para sysMMC CFW y emuMMC; no cambia el arranque automático.");v.rebootDetails.push_back("El aviso de red se muestra antes de confirmar. La aplicación no exige detectar modo avión para continuar.");}
    else {v.screen=Screen::RebootConfirm;v.transferDetail=job.rebootPlan.detail;v.rebootDetails.push_back("Entrada: "+job.rebootPlan.entry.name+". "+job.rebootPlan.entry.sourcePath);v.rebootDetails.push_back(job.rebootPlan.automatic?"A solicitará el reinicio hacia la entrada revisada.":"Reinicio automático no disponible: sal y entra manualmente en Hekate; elige la entrada indicada.");v.rebootDetails.push_back("Después del arranque, abre hbmenu y NinMagic Saves manualmente.");}
}
void showImportReview(View&v,const RestorePlan&p){
    v.sourceGame=game(p.exported.source);v.sourceProfile=profile(p.exported.source);v.sourceTitleId=transferTitleId(p.exported.source.titleId);v.destinationProfile=profile(p.destination);v.destinationTitleId=transferTitleId(p.destination.titleId);v.backupVerified=p.backupVerified;v.backupPath=p.backupId;v.sourceAccountId=accountIdentity(p.exported.source);v.destinationAccountId=accountIdentity(p.destination);v.screen=Screen::ImportConfirm;
}
} // namespace
int main(int argc,char**argv){
    (void)argc;(void)argv;using namespace nmsaves;
    View view;view.update.runningVersion=AppVersion;const auto type=appletGetAppletType();view.appletMode=type==AppletType_LibraryApplet||type==AppletType_SystemApplet;
    char fw[40];const auto hos=hosversionGet();std::snprintf(fw,sizeof(fw),"%u.%u.%u",unsigned((hos>>16)&255),unsigned((hos>>8)&255),unsigned(hos&255));view.firmware=fw;
    bool romMounted=R_SUCCEEDED(romfsInit());
    if(!romMounted||!uiInit("romfs:")){
        if(romMounted)romfsExit();
        consoleInit(nullptr);std::printf("NinMagic Saves %s beta\nNo se pudieron cargar los recursos.\nNo se han abierto partidas. Pulsa + para salir.\n",AppVersion);
        PadState fallback;padConfigureInput(1,HidNpadStyleSet_NpadStandard);padInitializeDefault(&fallback);while(appletMainLoop()){padUpdate(&fallback);if(padGetButtonsDown(&fallback)&HidNpadButton_Plus)break;consoleUpdate(nullptr);}consoleExit(nullptr);return 1;
    }
    Framebuffer framebuffer;const Result fbResult=framebufferCreate(&framebuffer,nwindowGetDefault(),1280,720,PIXEL_FORMAT_RGBA_8888,2);if(R_FAILED(fbResult)){uiExit();romfsExit();return 1;}framebufferMakeLinear(&framebuffer);
    PadState pad;padConfigureInput(1,HidNpadStyleSet_NpadStandard);padInitializeDefault(&pad);
    std::vector<SaveEntry> sources,targets;std::vector<ExportEntry> exports;std::vector<RecoveryEntry> recoveries;
    Screen resultReturn=Screen::Home;const u64 firstTick=armGetSystemTick();bool quit=false;
    start(JobKind::Inspect,view,"Comprobando el entorno activo y la conexión...");
    while(!quit&&appletMainLoop()){
        view.seconds=double(armGetSystemTick()-firstTick)/double(armGetSystemTickFreq());padUpdate(&pad);const u64 down=padGetButtonsDown(&pad);bool completed=false;
        if(job.active)view.progressCurrent=job.progress.load(std::memory_order_relaxed);
        if(job.active&&job.done.load(std::memory_order_acquire)){
            threadWaitForExit(&job.thread);threadClose(&job.thread);job.active=false;appletSetAutoSleepDisabled(false);completed=true;applyEnvironment(view);
            if(job.kind==JobKind::InstallUpdate){view.update=job.update;view.updateRecord=job.updateRecord;}
            if(!job.ok&&job.kind!=JobKind::CheckUpdate){view.message=job.error.empty()?"No se pudo completar la operación. No hay un resultado confirmado.":job.error;view.success=false;view.screen=Screen::Result;}
            else switch(job.kind){
                case JobKind::Inspect:view.recoveryError=job.recoveryError;view.screen=Screen::Home;view.selected=0;break;
                case JobKind::Sources:
                    sources=std::move(job.saves);view.rows.clear();for(const auto&s:sources)view.rows.push_back(saveRow(s));view.selected=0;view.transferDetail="No hay partidas de usuario ni datos compartidos disponibles en este sistema original.";view.screen=Screen::Sources;break;
                case JobKind::Exports:
                    exports=std::move(job.exports);view.rows.clear();for(const auto&e:exports){auto row=saveRow(e.source);row.subtitle=saveSubtitle(e.source)+" / "+std::to_string(e.files)+" archivos";row.detail+=". Copia: "+e.id;view.rows.push_back(std::move(row));}view.selected=0;view.transferDetail=!job.error.empty()?job.error:exports.empty()?"Todavía no hay copias exportadas. Usa Traer partidas originales y exporta desde sysMMC CFW.":"";view.screen=Screen::Imports;break;
                case JobKind::Targets:
                    targets=std::move(job.saves);view.rows.clear();for(const auto&s:targets)view.rows.push_back(saveRow(s));view.selected=0;view.transferDetail=job.selectedExport.source.kind==SaveKind::Device?"No hay datos compartidos de destino para el mismo Title ID y tipo. Inicia ese juego en emuMMC, crea su guardado, ciérralo y vuelve aquí.":"No hay una partida de usuario de destino para el mismo Title ID y tipo. Inicia ese juego en emuMMC con la cuenta elegida, crea una partida, ciérralo y vuelve aquí.";view.screen=Screen::Targets;break;
                case JobKind::PrepareImport:
                    if(job.importPlan.backupVerified)showImportReview(view,job.importPlan);else {view.message="No se pudo verificar el backup del destino. La sustitución no está disponible.";view.success=false;view.screen=Screen::Result;}
                        break;
                case JobKind::Recoveries:
                    recoveries=std::move(job.recoveries);view.rows.clear();for(const auto&r:recoveries){auto row=saveRow(r.destination);row.subtitle=saveSubtitle(r.destination)+" / "+r.state;row.detail="Copia previa: "+r.backupId+". Se restaurará el estado anterior a esta importación; se perderán cambios posteriores.";view.rows.push_back(std::move(row));}view.selected=0;view.transferDetail="No hay copias previas de importaciones realizadas por NinMagic Saves.";view.screen=Screen::Backups;break;
                case JobKind::ReviewReboot:case JobKind::PrepareAccess:showReboot(view);break;
                case JobKind::CheckUpdate:view.update=std::move(job.update);view.updateRecord=std::move(job.updateRecord);view.updateError=job.error;view.queryComplete=true;view.screen=Screen::Update;break;
                case JobKind::PrepareUpdate:view.update=std::move(job.update);view.screen=Screen::ConfirmUpdate;break;
                case JobKind::InstallUpdate:view.update=std::move(job.update);view.updateRecord=std::move(job.updateRecord);view.success=job.ok;view.message=job.ok?view.update.detail:job.error;view.screen=Screen::Result;break;
                case JobKind::ArchiveRecord:view.success=true;view.message="Registro archivado. Se conservan el NRO y sus copias anteriores.";view.recoveryError.clear();view.updateRecord={};view.screen=Screen::Result;break;
                case JobKind::Export:case JobKind::Import:case JobKind::Recover:case JobKind::Reboot:view.success=true;view.message=job.message;view.screen=Screen::Result;resultReturn=Screen::Home;break;
            }
        }
        // Do not process the completion frame. A key pressed on the old screen
        // must not confirm an overwrite or reboot review that has just appeared.
        if(!job.active&&!completed){
            if(down&HidNpadButton_Plus)quit=true;
            else {
                if(view.screen==Screen::Home||view.screen==Screen::Sources||view.screen==Screen::Imports||view.screen==Screen::Targets||view.screen==Screen::Backups)moveSelection(view,down);
                switch(view.screen){
                    case Screen::Home:
                        if(down&HidNpadButton_X){resultReturn=Screen::Home;start(JobKind::Inspect,view,"Comprobando el entorno activo y la conexión...");}
                        else if((down&HidNpadButton_R)&&canUpdateUi(view.environment)){resultReturn=Screen::Update;start(JobKind::CheckUpdate,view,"Consultando el canal HTTPS de NinMagic Saves...");}
                        else if(down&HidNpadButton_Minus)view.screen=Screen::Help;
                        else if(down&HidNpadButton_A){const auto actions=homeActions(view.environment);if(view.selected>=actions.size())break;resultReturn=Screen::Home;
                            switch(actions[view.selected]){
                                case HomeAction::Refresh:start(JobKind::Inspect,view,"Comprobando el entorno activo...");break;
                                case HomeAction::BringOriginals:case HomeAction::ReturnEmuMmc:job.rebootTarget=actions[view.selected]==HomeAction::BringOriginals?RebootTarget::CfwSysMMC:RebootTarget::EmuMMC;job.networkNoticeAcknowledged=false;start(JobKind::ReviewReboot,view,"Revisando el acceso de Hekate sin reiniciar...");break;
                                case HomeAction::Export:start(JobKind::Sources,view,"Revisando las partidas del sistema original...");break;
                                case HomeAction::Import:start(JobKind::Exports,view,"Leyendo las copias guardadas en la microSD...");break;
                                case HomeAction::Backups:start(JobKind::Recoveries,view,"Revisando copias previas a una importación...");break;
                            }
                        }break;
                    case Screen::Sources:
                        if(down&HidNpadButton_B){view.screen=Screen::Home;view.selected=0;}
                        else if((down&HidNpadButton_A)&&view.selected<view.rows.size())view.rows[view.selected].checked=!view.rows[view.selected].checked;
                        else if((down&HidNpadButton_Y)&&std::any_of(view.rows.begin(),view.rows.end(),[](const UiRow&r){return r.checked;}))view.screen=Screen::ExportConfirm;
                        break;
                    case Screen::ExportConfirm:
                        if(down&HidNpadButton_B)view.screen=Screen::Sources;
                        else if((down&HidNpadButton_A)&&canExportUi(view.environment)){job.selectedSaves.clear();for(size_t i=0;i<sources.size()&&i<view.rows.size();++i)if(view.rows[i].checked)job.selectedSaves.push_back(sources[i]);resultReturn=Screen::Home;start(JobKind::Export,view,"Copiando las partidas originales a la microSD...");}
                        break;
                    case Screen::Imports:
                        if(down&HidNpadButton_B){view.screen=Screen::Home;view.selected=0;}
                        else if((down&HidNpadButton_A)&&view.selected<exports.size()){job.selectedExport=exports[view.selected];resultReturn=Screen::Home;start(JobKind::Targets,view,"Buscando partidas de destino del mismo juego...");}
                        break;
                    case Screen::Targets:
                        if(down&HidNpadButton_B){resultReturn=Screen::Home;start(JobKind::Exports,view,"Leyendo las copias de la microSD...");}
                        else if((down&HidNpadButton_A)&&view.selected<targets.size()&&view.environment==UiEnvironment::EmuMmc){job.selectedTarget=targets[view.selected];view.backupVerified=false;resultReturn=Screen::Home;start(JobKind::PrepareImport,view,"Creando y verificando la copia previa del destino. Todavía no se reemplaza la partida...");}
                        break;
                    case Screen::ImportConfirm:
                        if(down&HidNpadButton_B){view.screen=Screen::Home;view.selected=0;}
                        else if((down&HidNpadButton_A)&&canImportUi(view.environment,view.backupVerified)){resultReturn=Screen::Home;start(JobKind::Import,view,"Importando la copia en la cuenta de destino confirmada...");}
                        break;
                    case Screen::Backups:
                        if(down&HidNpadButton_B){view.screen=Screen::Home;view.selected=0;}
                        else if((down&HidNpadButton_A)&&view.selected<recoveries.size()){job.selectedRecovery=recoveries[view.selected];view.sourceGame=game(job.selectedRecovery.destination);view.sourceTitleId=transferTitleId(job.selectedRecovery.destination.titleId);view.destinationProfile=profile(job.selectedRecovery.destination);view.destinationAccountId=accountIdentity(job.selectedRecovery.destination);view.backupPath=job.selectedRecovery.backupId;view.backupVerified=false;view.screen=Screen::RestoreConfirm;}
                        break;
                    case Screen::RestoreConfirm:
                        if(down&HidNpadButton_B)view.screen=Screen::Backups;
                        else if((down&HidNpadButton_A)&&view.environment==UiEnvironment::EmuMmc){resultReturn=Screen::Home;start(JobKind::Recover,view,"Verificando y restaurando la copia previa a la importación...");}
                        break;
                    case Screen::ConfigureReboot:case Screen::RebootConfirm:
                        if(down&HidNpadButton_B){view.screen=Screen::Home;view.selected=0;}
                        else if(down&HidNpadButton_A){job.networkNoticeAcknowledged=true;resultReturn=Screen::Home;if(view.screen==Screen::ConfigureReboot)start(JobKind::PrepareAccess,view,"Creando el acceso separado de NinMagic Saves en Hekate...");else if(view.automaticReboot)start(JobKind::Reboot,view,"Volviendo a comprobar la entrada confirmada y solicitando el reinicio...");}
                        break;
                    case Screen::Help:if(down&(HidNpadButton_B|HidNpadButton_Minus)){view.screen=Screen::Home;view.selected=0;}
                        break;
                    case Screen::Update:
                        if(down&HidNpadButton_B){view.screen=Screen::Home;view.selected=0;}
                        else if((down&HidNpadButton_X)&&canUpdateUi(view.environment)){resultReturn=Screen::Update;start(JobKind::CheckUpdate,view,"Consultando el canal HTTPS de NinMagic Saves...");}
                        else if((down&HidNpadButton_Y)&&canUpdateUi(view.environment)&&view.updateRecord.state==nm::CompletedUpdateState::CanArchive)view.screen=Screen::ConfirmArchive;
                        else if((down&HidNpadButton_A)&&canUpdateUi(view.environment)&&canDownloadAppUpdate(view)){resultReturn=Screen::Update;if(view.update.prepared)view.screen=Screen::ConfirmUpdate;else start(JobKind::PrepareUpdate,view,"Descargando y verificando la actualización...");}
                        break;
                    case Screen::ConfirmUpdate:
                        if(down&HidNpadButton_B)view.screen=Screen::Update;
                        else if((down&HidNpadButton_A)&&canUpdateUi(view.environment)){if(romMounted){romfsExit();romMounted=false;}resultReturn=Screen::Update;start(JobKind::InstallUpdate,view,"Guardando una copia e instalando el NRO verificado...");}
                        break;
                    case Screen::ConfirmArchive:
                        if(down&HidNpadButton_B)view.screen=Screen::Update;
                        else if((down&HidNpadButton_A)&&canUpdateUi(view.environment)){resultReturn=Screen::Update;start(JobKind::ArchiveRecord,view,"Archivando el registro confirmado...");}
                        break;
                    case Screen::Result:if(down&HidNpadButton_B){view.screen=resultReturn;view.selected=0;}
                        break;
                    case Screen::Busy:break;
                }
            }
        }
        u32 stride=0;auto*pixels=static_cast<uint32_t*>(framebufferBegin(&framebuffer,&stride));uiDraw(pixels,int(stride/sizeof(uint32_t)),view);framebufferEnd(&framebuffer);
    }
    if(job.active){threadWaitForExit(&job.thread);threadClose(&job.thread);appletSetAutoSleepDisabled(false);}nm::updateNetworkExit();framebufferClose(&framebuffer);uiExit();if(romMounted)romfsExit();return 0;
}
