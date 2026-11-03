#include "ui.hpp"
#include <cstdio>
#include <string>
#include <vector>

// All entries below are offline design fixtures; they are never linked into the NRO.
int main(int argc,char**argv){
    using namespace nmsaves;
    if(argc!=3||!uiInit(argv[1]))return 1;
    View v;
    v.firmware="23.0.1 / EJEMPLO";v.atmosphere="Vista previa / datos ficticios";
    std::vector<uint32_t> pixels(1280*720);
    bool failed=false;
    const auto draw=[&](const std::string&name){
        uiDraw(pixels.data(),1280,v);const auto path=std::string(argv[2])+"/"+name+".ppm";
        FILE*f=std::fopen(path.c_str(),"wb");if(!f){failed=true;return;}
        std::fprintf(f,"P6\n1280 720\n255\n");
        for(const auto pixel:pixels){const unsigned char p[]={static_cast<unsigned char>(pixel),static_cast<unsigned char>(pixel>>8),static_cast<unsigned char>(pixel>>16)};if(std::fwrite(p,1,3,f)!=3)failed=true;}
        if(std::fclose(f)!=0)failed=true;
    };
    v.environmentDetail="No se pudo confirmar de forma coherente el entorno. No se abrirán partidas ni se iniciará la red.";
    draw("home-unknown");
    v.environment=UiEnvironment::EmuMmc;v.environmentDetail="Atmosphère confirma emuMMC. Elige un recorrido; no se importará ni reiniciará nada al abrir esta pantalla.";draw("home-emu");
    v.environment=UiEnvironment::SysMmc;v.offline=true;v.environmentDetail="Wi-Fi apagado y sin conexión. Las partidas originales solo se abren para exportar copias a la microSD.";draw("home-sys-offline");
    v.offline=false;v.environmentDetail="Desactiva Wi-Fi y activa modo avión. El acceso a partidas originales queda bloqueado hasta comprobar la desconexión.";draw("home-sys-connected");v.offline=true;
    v.rows={{"OCTOPATH TRAVELER II","Perfil original / ejemplo","Title ID: 0100000000000000. Perfil: original. Tipo: usuario.",true},{"Paw Patrol: Mighty Pups Save Adventure Bay!","Segundo perfil / ejemplo","Title ID: 0100000000000001. Perfil: segundo. Tipo: usuario.",true}};
    v.screen=Screen::Sources;draw("sources");v.screen=Screen::ExportConfirm;draw("export-confirm");
    v.screen=Screen::Busy;v.message="Copiando las partidas originales a la microSD...";v.progressKnown=true;v.progressTotal=2;v.progressCurrent=1;draw("export-progress");
    v.environment=UiEnvironment::EmuMmc;v.screen=Screen::Imports;draw("imports");v.transferDetail="2 copia(s) incompleta(s) o dañada(s) excluida(s).";draw("imports-warning");v.rows.clear();draw("imports-invalid-only");v.transferDetail="Todavía no hay copias exportadas. Usa Traer partidas originales y exporta desde sysMMC CFW.";draw("imports-empty");
    v.rows={{"OCTOPATH TRAVELER II","Cuenta emuMMC / ejemplo","Title ID: 0100000000000000. Perfil: cuenta emuMMC. Tipo: usuario.",false}};v.screen=Screen::Targets;draw("targets");
    v.sourceGame="OCTOPATH TRAVELER II";v.sourceTitleId="0100000000000000";v.sourceProfile="Perfil original";v.destinationProfile="Cuenta emuMMC diferente";v.sourceAccountId="UID: 0123456789ABCDEF0123456789ABCDEF";v.destinationAccountId="UID: FEDCBA9876543210FEDCBA9876543210";v.destinationTitleId=v.sourceTitleId;v.backupPath="backup-20261103-0100000000000000-1234567890";v.backupVerified=true;
    v.screen=Screen::ImportConfirm;draw("import-confirm");v.backupVerified=false;draw("import-blocked");
    v.screen=Screen::Backups;v.rows[0].subtitle="Cuenta emuMMC / completado";v.rows[0].detail="Copia previa a esta importación. Se perderán los cambios posteriores de la cuenta de destino.";draw("backups");v.screen=Screen::RestoreConfirm;draw("recovery-confirm");
    v.screen=Screen::ConfigureReboot;v.rebootToOriginal=true;v.originalAirplaneAcknowledged=false;
    v.rebootDetails={"Crea accesos separados para sysMMC CFW y emuMMC; no cambia el arranque automático.","Si aún no activaste modo avión en el original, cancela y hazlo primero desde el sistema original."};draw("reboot-configure");
    v.screen=Screen::RebootConfirm;v.automaticReboot=true;v.originalAirplaneAcknowledged=true;
    v.transferDetail="Destino: NinMagic Saves - Original CFW. Es CFW sobre la memoria original; el modo avión debe estar activado previamente.";
    v.rebootDetails={"Entrada: NinMagic Saves - Original CFW. bootloader/ini/ninmagic-saves.ini","A solicitará el reinicio hacia la entrada revisada.","Después del arranque, abre hbmenu y NinMagic Saves manualmente."};draw("reboot-confirm");
    v.automaticReboot=false;v.rebootDetails[1]="Reinicio automático no disponible: sal y entra manualmente en Hekate; elige la entrada indicada.";v.transferDetail="Destino: NinMagic Saves - Original CFW. Reinicia manualmente a Hekate > More configs y elige esa entrada.";draw("reboot-manual");
    v.screen=Screen::Help;draw("help");
    v.screen=Screen::Update;v.queryComplete=true;v.update.configured=true;v.update.runningVersion=AppVersion;v.update.version=AppVersion;v.update.sdVersion=AppVersion;v.update.sdVerified=true;draw("update-current");
    v.updateError="No se pudo consultar la actualización por HTTPS: Could not resolve host. Comprueba la conexión y vuelve a intentarlo.";draw("update-error");
    v.updateError.clear();v.update.version="0.3.0";v.update.available=true;v.update.sha256=std::string(64,'a');v.screen=Screen::ConfirmUpdate;draw("update-confirm");
    v.screen=Screen::Result;v.success=false;v.message="La copia previa del destino no superó la verificación. No se ha sobrescrito ninguna partida. Revisa la microSD y vuelve a intentarlo.";draw("result");
    uiExit();return failed?1:0;
}
