#include "ui.hpp"
#include "ui_text.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>

namespace nmsaves {
namespace {
constexpr int W=1280, H=720;
using Color=uint32_t;
constexpr Color rgb(unsigned r,unsigned g,unsigned b) { return r|(g<<8)|(b<<16)|0xff000000u; }
constexpr Color ink=rgb(227,239,254), muted=rgb(143,164,192), dim=rgb(85,113,146);
constexpr Color cyan=rgb(71,227,240), violet=rgb(176,131,255), pink=rgb(255,116,158);
constexpr Color green=rgb(177,255,87), orange=rgb(255,174,72), magenta=rgb(255,69,207);
constexpr Color panel=rgb(12,24,44), raised=rgb(18,36,61), border=rgb(32,57,82);
struct Glyph {int w=0,h=0,advance=0;std::vector<uint8_t> alpha;};
struct Font {std::array<Glyph,224> glyphs;};
std::map<int,Font> fonts;
std::vector<uint8_t> rawFont, logo;
std::array<uint8_t,224> advances{};
std::vector<Color> background;
Color *fb=nullptr;
int pitch=W;

void pixel(int x,int y,Color c) {if(x>=0&&x<W&&y>=0&&y<H) fb[y*pitch+x]=c;}
void rect(int x,int y,int w,int h,Color c) {
    int x0=std::max(0,x),x1=std::min(W,x+w),y0=std::max(0,y),y1=std::min(H,y+h);
    if(x1<=x0||y1<=y0) return;
    for(int yy=y0;yy<y1;++yy) std::fill(fb+yy*pitch+x0,fb+yy*pitch+x1,c);
}
Color blend(Color dst,Color src,unsigned a) {
    unsigned r=(((dst&255)*(255-a)+(src&255)*a)+127)/255;
    unsigned g=((((dst>>8)&255)*(255-a)+((src>>8)&255)*a)+127)/255;
    unsigned b=((((dst>>16)&255)*(255-a)+((src>>16)&255)*a)+127)/255;
    return rgb(r,g,b);
}
void alphaPixel(int x,int y,Color c,unsigned a) {
    if(x>=0&&x<W&&y>=0&&y<H&&a) {
        Color &p=fb[y*pitch+x];p=a==255?c:blend(p,c,a);
    }
}
void line(int x0,int y0,int x1,int y1,Color c) {
    int dx=std::abs(x1-x0),sx=x0<x1?1:-1,dy=-std::abs(y1-y0),sy=y0<y1?1:-1,err=dx+dy;
    for(;;){pixel(x0,y0,c);if(x0==x1&&y0==y1)break;int e2=2*err;if(e2>=dy){err+=dy;x0+=sx;}if(e2<=dx){err+=dx;y0+=sy;}}
}
void box(int x,int y,int w,int h,Color fill=panel,Color edge=border) {
    rect(x,y,w,h,fill);rect(x,y,w,1,edge);rect(x,y+h-1,w,1,edge);
    rect(x,y,1,h,edge);rect(x+w-1,y,1,h,edge);
    // Cut corners and short highlights give the panels their circuit-board shape.
    line(x,y+7,x+7,y,edge);line(x+w-8,y+h-1,x+w-1,y+h-8,edge);
    rect(x+1,y+1,6,2,fill);rect(x+1,y+3,4,2,fill);rect(x+1,y+5,2,2,fill);
}
std::vector<uint32_t> decode(const std::string&s) { return nm::ui_text::displayCodepoints(s); }
std::string encode(const std::vector<uint32_t>&v) {
    std::string s;for(auto cp:v) {if(cp<128)s+=static_cast<char>(cp);else{s+=static_cast<char>(0xc0|(cp>>6));s+=static_cast<char>(0x80|(cp&63));}}return s;
}
Font& font(int size) {
    auto it=fonts.find(size);if(it!=fonts.end())return it->second;
    Font scaled;
    for(unsigned i=0;i<224;++i) {
        auto &g=scaled.glyphs[i];g.w=size;g.h=(34*size+12)/24;g.advance=(advances[i]*size+12)/24;
        g.alpha.resize(g.w*g.h);
        for(int y=0;y<g.h;++y)for(int x=0;x<g.w;++x)
            g.alpha[y*g.w+x]=rawFont.empty()?0:rawFont[i*24*34+(y*34/g.h)*24+x*24/g.w];
    }
    return fonts.emplace(size,std::move(scaled)).first->second;
}
int width(const std::string&s,int size) {
    auto &f=font(size);int w=0;for(auto cp:decode(s))w+=f.glyphs[cp-32].advance;return w;
}
void text(const std::string&s,int x,int y,int size=18,Color c=ink) {
    auto &f=font(size);int offset=(5*size+12)/24;
    for(auto cp:decode(s)) {
        const auto &g=f.glyphs[cp-32];
        for(int yy=0;yy<g.h;++yy) {
            int py=y+yy-offset;if(py<0||py>=H)continue;
            for(int xx=0;xx<g.w;++xx)if(g.alpha[yy*g.w+xx])alphaPixel(x+xx,py,c,g.alpha[yy*g.w+xx]);
        }
        x+=g.advance;
    }
}
std::string fit(const std::string&s,int maxw,int size) {
    if(width(s,size)<=maxw)return s;
    auto chars=decode(s);while(!chars.empty()&&width(encode(chars)+"...",size)>maxw)chars.pop_back();
    return encode(chars)+"...";
}
void fitted(const std::string&s,int x,int y,int maxw,int size=18,Color c=ink) {text(fit(s,maxw,size),x,y,size,c);}
void wrapped(const std::string&s,int x,int y,int maxw,int size,int maxlines,Color c=ink) {
    std::istringstream words(s);std::string word,current;int n=0;
    while(words>>word) {
        std::string trial=current.empty()?word:current+" "+word;
        if(width(trial,size)>maxw&&!current.empty()) {
            fitted(current,x,y+n*(size+9),maxw,size,c);if(++n>=maxlines)return;current=word;
        } else current=trial;
        if(n==maxlines-1) {
            std::string rest;std::getline(words,rest);fitted(current+rest,x,y+n*(size+9),maxw,size,c);return;
        }
    }
    if(!current.empty())fitted(current,x,y+n*(size+9),maxw,size,c);
}
// File paths must remain exact: wrap at character boundaries, never abbreviate.
std::vector<std::string> pathLines(const std::string&s,int maxw,int size) {
    std::vector<std::string> lines;std::vector<uint32_t> current;
    for(auto cp:decode(s)) {
        auto trial=current;trial.push_back(cp);
        if(!current.empty()&&width(encode(trial),size)>maxw){lines.push_back(encode(current));current.clear();}
        current.push_back(cp);
    }
    if(!current.empty())lines.push_back(encode(current));
    return lines;
}
void exactPath(const std::string&s,int x,int y,int maxw,int size,int maxlines,Color c=muted) {
    const auto lines=pathLines(s,maxw,size);
    for(int i=0;i<std::min(maxlines,static_cast<int>(lines.size()));++i)text(lines[i],x,y+i*(size+7),size,c);
    if(static_cast<int>(lines.size())>maxlines)text("Continúa en el resumen de rutas.",x,y+maxlines*(size+7),12,dim);
}
void logoAt(int x,int y,int size) {
    if(logo.size()!=256*256*4){box(x,y,size,size,raised,magenta);text("NM",x+size/8,y+size/4,size/3,cyan);return;}
    for(int yy=0;yy<size;++yy)for(int xx=0;xx<size;++xx) {
        size_t p=((yy*256/size)*256+xx*256/size)*4;
        alphaPixel(x+xx,y+yy,rgb(logo[p],logo[p+1],logo[p+2]),logo[p+3]);
    }
}
void footer(const std::vector<std::pair<std::string,std::string>>&keys) {
    rect(35,659,1210,1,border);int x=39;
    for(const auto &k:keys){box(x,677,29,25,raised,border);text(k.first,x+7,682,13,ink);text(k.second,x+37,682,14,muted);x+=width(k.second,14)+60;}
    text("NINMAGIC / "+std::string(AppVersion),1080,684,12,magenta);
}
void accent(int x,int y,int w) {
    const Color colors[]={cyan,magenta,violet};for(int i=0;i<3;++i)rect(x+i*w/3,y,w/3+1,3,colors[i]);
}
void header(const View&v) {
    logoAt(36,22,61);text("NINMAGIC",108,27,27);text("SAVES / BETA",109,60,12,magenta);
    rect(343,35,1,35,border);text("PARTIDAS GUARDADAS",366,43,18,muted);
    box(1000,37,241,30,raised,magenta);text("BETA / "+std::string(AppVersion),1020,44,14,magenta);
    accent(36,99,1207);
    fitted("FW "+(v.firmware.empty()?std::string("sin comprobar"):v.firmware),38,116,245,13,muted);
    fitted(v.atmosphere.empty()?"Atmosphère: sin comprobar":v.atmosphere,293,116,420,13,cyan);
    text(v.appletMode?"MODO ÁLBUM":"MODO APLICACIÓN",1000,116,13,v.appletMode?orange:muted);
}
void heading(const std::string&a,const std::string&b) {text(a,37,157,29);text(b,38,200,17,muted);}
std::string modeLabel(UiEnvironment env){return env==UiEnvironment::EmuMmc?"emuMMC confirmada":env==UiEnvironment::SysMmc?"sysMMC CFW confirmada":"Entorno no confirmado";}
std::pair<std::string,std::string> actionLabel(HomeAction action){
    switch(action){
        case HomeAction::BringOriginals:return {"Traer partidas originales","Prepara el acceso a sysMMC CFW y revisa el reinicio."};
        case HomeAction::Import:return {"Importar copias desde la microSD","Elige el juego y la cuenta de destino en emuMMC."};
        case HomeAction::Backups:return {"Recuperar una copia previa","Restaura el estado anterior a una importación."};
        case HomeAction::Export:return {"Exportar partidas originales","Copia las seleccionadas a la microSD. El origen se conserva."};
        case HomeAction::ReturnEmuMmc:return {"Volver a emuMMC","Revisa el arranque y abre NinMagic Saves de nuevo."};
        case HomeAction::Refresh:return {"Comprobar el entorno","Las operaciones quedan bloqueadas hasta identificarlo."};
    }return {};
}
void home(const View&v){
    heading("Tus partidas, entre ambos entornos", "sysMMC original > copia en microSD > emuMMC de destino");
    box(37,244,393,380);accent(38,245,391);logoAt(143,269,177);
    text("ENTORNO ACTUAL",61,466,13,magenta);wrapped(modeLabel(v.environment),61,497,342,22,2);
    fitted(v.environment==UiEnvironment::SysMmc?(v.offline?"WI-FI APAGADO, SIN CONEXIÓN":"AVISO: ACTIVA MODO AVIÓN"):"ORIGEN Y DESTINO SEPARADOS",61,581,342,14,v.offline?green:orange);
    box(450,244,793,380);accent(451,245,791);const auto actions=homeActions(v.environment);
    for(size_t i=0;i<actions.size();++i){const int y=269+int(i)*87;const bool active=i==v.selected;auto labels=actionLabel(actions[i]);
        box(472,y,748,75,active?rgb(38,24,59):panel,active?magenta:border);text(std::to_string(i+1),488,y+17,22,active?cyan:muted);fitted(labels.first,523,y+12,675,22,active?ink:muted);fitted(labels.second,523,y+47,675,14,muted);
    }
    wrapped(v.environmentDetail,475,550,742,15,3,muted);
    std::vector<std::pair<std::string,std::string>> keys={{"A","Elegir"},{"X","Revisar entorno"}};
    if(canUpdateUi(v.environment))keys.push_back({"R","Actualizar"});
    keys.push_back({"-","Ayuda"});keys.push_back({"+","Salir"});footer(keys);
}
void list(const View&v){
    const bool sources=v.screen==Screen::Sources,imports=v.screen==Screen::Imports,targets=v.screen==Screen::Targets;
    heading(sources?"Partidas del sistema original":imports?"Copias guardadas en la microSD":targets?"Elige la cuenta de destino":"Copias previas a una importación",
        sources?"A marca varias partidas. Y revisa la copia a microSD.":targets?"Mismo juego; la cuenta y la versión pueden ser diferentes.":"Selecciona una copia para revisar el siguiente paso.");
    box(37,244,760,380);accent(38,245,758);box(816,244,427,380);accent(817,245,425);
    if(v.rows.empty())wrapped(v.transferDetail.empty()?"No hay resultados disponibles.":v.transferDetail,63,285,708,21,7,muted);
    const size_t start=(v.selected/6)*6;
    for(size_t i=start;i<v.rows.size()&&i<start+6;++i){const int y=263+int(i-start)*53;const auto&r=v.rows[i];bool active=i==v.selected;
        box(52,y,730,48,active?rgb(38,24,59):panel,active?magenta:border);
        if(sources){box(66,y+14,17,17,r.checked?cyan:panel,r.checked?cyan:muted);if(r.checked)text("+",69,y+13,13,panel);}
        fitted(r.title,sources?97:67,y+6,sources?669:699,18,active?ink:muted);fitted(r.subtitle,sources?97:67,y+29,sources?669:699,12,cyan);
    }
    fitted(std::to_string(v.rows.size())+(sources?" partidas / ":" copias / ")+std::to_string(v.rows.empty()?0:v.selected+1),62,598,704,13,muted);
    text(sources?"SOLO COPIA DEL ORIGEN":targets?"DESTINO EXPLÍCITO":imports?"COPIA VERIFICABLE":"RECUPERACIÓN",840,271,13,magenta);
    if(!v.rows.empty()&&v.selected<v.rows.size()){const auto&r=v.rows[v.selected];wrapped(r.title,840,306,379,22,3);wrapped(r.detail,840,415,379,16,6,muted);}
    else wrapped("No se ha seleccionado ninguna partida.",840,315,379,19,3,muted);
    if(imports&&!v.rows.empty()&&!v.transferDetail.empty())wrapped(v.transferDetail,840,565,379,14,3,orange);
    if(sources)wrapped("Las partidas originales no se sobrescriben. Cada usuario conserva una copia separada.",840,565,379,14,3,orange);
    if(targets)wrapped("A prepara y verifica un backup; después confirmarás la sustitución.",840,565,379,14,3,orange);
    std::vector<std::pair<std::string,std::string>> keys={{"A",sources?"Marcar":targets?"Preparar backup":"Elegir"}};
    if(sources)keys.push_back({"Y","Revisar exportación"});
    keys.push_back({"B","Volver"});keys.push_back({"+","Salir"});footer(keys);
}
void exportConfirm(const View&v){
    heading("Copiar las partidas seleccionadas", "El contenido original se abre solo para lectura");
    box(37,244,1206,380);accent(38,245,1204);unsigned count=0;for(const auto&r:v.rows)if(r.checked)++count;
    text(std::to_string(count)+" partidas seleccionadas",63,271,25,cyan);int y=320;unsigned shown=0;
    for(const auto&r:v.rows)if(r.checked&&shown++<6){fitted(r.title+" — "+r.subtitle,63,y,1158,17);y+=35;}
    if(count>6)text("Y "+std::to_string(count-6)+" más. B vuelve a la selección completa.",63,543,16,muted);
    wrapped("A crea copias separadas en la microSD. No borra ni modifica las partidas de sysMMC. Mantén el modo avión.",63,579,1156,16,2,orange);
    footer({{"A","Confirmar copia"},{"B","Volver a seleccionar"}});
}
void transferConfirm(const View&v,bool recovery){
    heading(recovery?"Recuperar la partida anterior":"Confirmar importación",recovery?"Se restaurará la copia previa a esta importación":"Mismo juego; la cuenta y la versión pueden ser diferentes.");
    box(37,244,1206,380);accent(38,245,1204);fitted(v.sourceGame,62,270,1155,26,ink);
    text("JUEGO / TITLE ID",63,317,13,magenta);text(v.sourceTitleId,63,345,20,cyan);
    text(recovery?"CUENTA DE DESTINO":"CUENTA DE ORIGEN",63,392,13,magenta);fitted(recovery?v.destinationProfile:v.sourceProfile,63,419,540,20);
    text(recovery?v.destinationAccountId:v.sourceAccountId,63,450,12,muted);
    if(!recovery){text("CUENTA DE DESTINO EN EMUMMC",642,392,13,magenta);fitted(v.destinationProfile,642,419,575,20);text(v.destinationAccountId,642,450,12,muted);}
    text(recovery?"COPIA REGISTRADA / SE VERIFICA ANTES DE ESCRIBIR":v.backupVerified?"BACKUP DEL DESTINO VERIFICADO":"BACKUP NO VERIFICADO",63,471,13,v.backupVerified?green:orange);
    exactPath(v.backupPath,63,498,1156,15,2,cyan);
    wrapped(recovery?"Se perderán los cambios posteriores a esa importación. La copia se conserva tras restaurarla.":"A reemplaza el destino sin unir progresos ni convertir formatos. Un juego antiguo puede no reconocer un guardado más nuevo.",63,572,1156,17,2,orange);
    std::vector<std::pair<std::string,std::string>> keys;
    if(v.environment==UiEnvironment::EmuMmc&&(recovery||v.backupVerified))keys.push_back({"A",recovery?"Confirmar recuperación":"Confirmar sustitución"});
    keys.push_back({"B","Cancelar"});footer(keys);
}
void rebootConfirm(const View&v,bool configure){
    heading(configure?"Preparar acceso separado en Hekate":v.rebootToOriginal?"Entrar al sistema original con CFW":"Volver al entorno emuMMC", "El reinicio cambia de entorno; NinMagic Saves se abre después manualmente");
    box(37,244,1206,380);accent(38,245,1204);
    wrapped(configure?"Se creará únicamente bootloader/ini/ninmagic-saves.ini. Las entradas actuales se conservan.":v.transferDetail,63,270,1156,21,3);
    int y=374;for(size_t i=0;i<v.rebootDetails.size()&&i<3;++i){wrapped(v.rebootDetails[i],63,y,1156,16,2,muted);y+=49;}
    if(v.rebootToOriginal)wrapped("Aviso: activa modo avión en el sistema original y desconecta Ethernet. Hacerlo después de arrancar no evita una conexión durante el inicio.",63,536,1156,17,2,orange);
    wrapped("No ofrece invisibilidad ni garantía frente a baneos. Tras arrancar, abre hbmenu y NinMagic Saves de nuevo.",63,585,1156,15,2,muted);
    std::vector<std::pair<std::string,std::string>> keys;
    if(configure||v.automaticReboot)keys.push_back({"A",configure?"Crear acceso":"Confirmar reinicio"});
    keys.push_back({"B","Cancelar"});keys.push_back({"+","Salir"});footer(keys);
}
void help(){
    heading("Un recorrido con copia y confirmación", "Versión beta: requiere probar el recorrido en tu consola");box(37,244,1206,380);accent(38,245,1204);
    const std::pair<const char*,const char*> rows[]={
        {"01  PREPARA EL SISTEMA ORIGINAL","Activa modo avión en el original antes de arrancar con CFW. La detección de red es informativa y no impide continuar."},
        {"02  EXPORTA EN SYSMMC CFW","Abre NinMagic Saves desde hbmenu. Marca las partidas y confirma su copia a microSD. Aquí solo se permite exportar y volver."},
        {"03  IMPORTA EN EMUMMC","Elige una copia y una partida de destino existente del mismo juego. La cuenta y la versión pueden ser distintas."},
        {"04  CONFIRMA DESPUÉS DEL BACKUP","La aplicación prepara y verifica una copia del destino antes de pedir confirmación. Importar reemplaza la partida; nunca fusiona contenidos."},
        {"05  RECUPERA SI LO NECESITAS","Recuperar restaura el estado previo a una importación. Se perderán los cambios posteriores de esa partida. Actualiza la aplicación solo desde emuMMC."}
    };
    for(int i=0;i<5;++i){int y=265+i*68;text(rows[i].first,62,y,13,magenta);wrapped(rows[i].second,62,y+24,1157,16,2,muted);}footer({{"B","Volver"},{"+","Salir"}});
}
void update(const View&v) {
    heading("Actualizar NinMagic Saves", "Canal independiente: ANE-JV / ninmagic-saves");
    box(37,244,341,380);accent(38,245,339);logoAt(117,274,180);
    text("VERSIÓN EN EJECUCIÓN",61,489,13,cyan);text(AppVersion,61,521,32);
    fitted(v.update.sdVerified?"microSD: "+v.update.sdVersion:"microSD: sin comprobar",61,583,292,16,muted);
    box(396,244,847,380);accent(397,245,845);
    std::string title="Consulta pendiente", body="Pulsa X para consultar la versión publicada. Una consulta fallida no significa que tengas la última versión.";Color color=muted;
    if(v.queryComplete&&v.updateError.empty()&&!v.update.version.empty()) {
        if(v.update.installed||(publishedAppOnSd(v.update)&&v.update.version!=AppVersion)){title="Cierra y vuelve a abrir NinMagic Saves";body="El NRO publicado ya está en la microSD. La aplicación en memoria seguirá mostrando su versión anterior hasta que la cierres.";color=green;}
        else if(v.update.available){title="Nueva versión: "+v.update.version;body=v.update.prepared?"La descarga ya está verificada. A vuelve a la revisión; todavía no se ha sustituido el NRO instalado.":"A descarga el NRO y comprueba su nombre, versión, tamaño y SHA-256. Después podrás confirmar la instalación.";color=cyan;}
        else {title=v.update.version==AppVersion?"Ya tienes la última versión publicada":"No hay una versión posterior publicada";body="Versión consultada: "+v.update.version+". Puedes volver a comprobar el canal con X.";color=green;}
    }
    if(!v.recoveryError.empty()&&v.updateRecord.state!=nm::CompletedUpdateState::CanArchive){title="La instalación necesita revisión";body=v.recoveryError;color=orange;}
    if(v.updateRecord.state==nm::CompletedUpdateState::Blocked){title="La instalación necesita revisión";body=v.updateRecord.detail;color=orange;}
    if(!v.updateError.empty()){title="No se pudo completar la consulta";body=v.updateError;color=orange;}
    text("CANAL HTTPS",422,270,13,magenta);wrapped(title,422,303,796,24,2,color);wrapped(body,422,385,796,18,5,muted);
    if(v.updateRecord.state==nm::CompletedUpdateState::CanArchive)fitted("Y: revisar un registro de actualización finalizado",422,555,793,16,cyan);
    else if(!v.recoveryError.empty())fitted("Registro: "+v.recoveryError,422,555,793,14,orange);
    std::vector<std::pair<std::string,std::string>> keys;
    if(canDownloadAppUpdate(v)){keys.push_back({"A",v.update.prepared?"Revisar descarga":"Descargar"});text("La instalación se confirma en una pantalla de revisión.",422,595,15,muted);}
    keys.push_back({"X","Comprobar"});
    if(v.updateRecord.state==nm::CompletedUpdateState::CanArchive)keys.push_back({"Y","Registro"});
    keys.push_back({"B","Volver"});keys.push_back({"+","Salir"});footer(keys);
}
void confirmation(const View&v,bool archive) {
    heading(archive?"Archivar registro finalizado":"Instalar la actualización", "Confirma solamente después de revisar estos datos");
    box(37,244,1206,380);accent(38,245,1204);
    text(archive?"REGISTRO COMPROBADO":"DESCARGA COMPROBADA",63,272,14,cyan);
    wrapped(archive?v.updateRecord.detail:"NinMagic Saves "+v.update.version+" reemplazará la copia instalada. Se conserva un NRO anterior para recuperación.",63,310,1156,23,3);
    text(archive?"Se conservan el NRO y sus copias de seguridad.":"Después de instalar, cierra la aplicación y vuelve a abrirla.",63,426,19,magenta);
    if(!archive){text("SHA-256",63,471,13,muted);fitted(v.update.sha256,63,498,1156,17,cyan);exactPath(AppPath,63,543,1156,17,2,muted);}
    else wrapped("Solo se archivarán los registros TXT revisados. No se modificarán partidas ni archivos del sistema.",63,480,1156,18,3,muted);
    footer({{"A",archive?"Confirmar archivo":"Confirmar instalación"},{"B","Cancelar"}});
}
void busy(const View&v) {
    heading("Operación en curso", "Espera a que termine para cerrar la aplicación");
    box(37,244,1206,380);accent(38,245,1204);logoAt(73,301,170);
    wrapped(v.message,282,304,914,26,3);wrapped("No extraigas la microSD ni apagues la consola durante la operación. El resultado indicará qué se pudo completar.",282,449,914,18,3,muted);
    rect(282,565,914,6,border);
    if(v.progressKnown&&v.progressTotal){text(std::to_string(v.progressCurrent)+" / "+std::to_string(v.progressTotal)+" partidas procesadas",282,526,17,cyan);rect(282,565,int(std::min(v.progressCurrent,v.progressTotal)*914/v.progressTotal),6,magenta);}
    else {int step=int(v.seconds*210)%760;rect(282+step,565,154,6,magenta);}
    footer({});
}
void result(const View&v) {
    heading(v.success?"Operación completada":"No se pudo completar", "Revisa el resultado antes de continuar");
    box(37,244,1206,380);accent(38,245,1204);logoAt(72,298,174);
    wrapped(v.message,283,288,914,22,9,v.success?ink:orange);
    footer({{"B","Volver"},{"+","Salir"}});
}
uint32_t le32(const uint8_t*p){return p[0]|uint32_t(p[1])<<8|uint32_t(p[2])<<16|uint32_t(p[3])<<24;}
std::vector<uint8_t> readFile(const std::string&path) {
    std::ifstream in(path,std::ios::binary);if(!in)return{};
    in.seekg(0,std::ios::end);auto n=in.tellg();if(n<0||n>4000000)return{};in.seekg(0);std::vector<uint8_t>data(static_cast<size_t>(n));in.read(reinterpret_cast<char*>(data.data()),n);return in?data:std::vector<uint8_t>{};
}
} // namespace
bool uiInit(const std::string&assetRoot) {
    uiExit();auto data=readFile(assetRoot+"/font.bin");logo=readFile(assetRoot+"/logo.rgba");
    if(data.size()!=32+224+224*24*34||le32(data.data())!=0x54464D4E||le32(data.data()+4)!=32||le32(data.data()+8)!=224||le32(data.data()+12)!=24||le32(data.data()+16)!=34)return false;
    std::copy(data.begin()+32,data.begin()+256,advances.begin());rawFont.assign(data.begin()+256,data.end());
    for(int size:{12,13,14,15,16,17,18,19,20,22,23,24,25,26,27,29,32})font(size);
    background.resize(W*H);Color *oldfb=fb;int oldpitch=pitch;fb=background.data();pitch=W;
    for(int y=0;y<H;++y)for(int x=0;x<W;++x){unsigned glow=static_cast<unsigned>(std::max(0,290-(std::abs(x-1100)+std::abs(y-120))/2));pixel(x,y,rgb(7+glow/11,8+glow/36,24+glow/14));}
    Color trace=rgb(44,27,60),node=rgb(92,39,88);
    for(int i=0;i<5;++i){int x=710+i*106;line(x,0,x,24+i*11,trace);line(x,24+i*11,x+22,46+i*11,trace);line(x+22,46+i*11,1279,46+i*11,trace);rect(x-2,22+i*11,5,5,node);}
    fb=oldfb;pitch=oldpitch;return true;
}
void uiDraw(uint32_t*pixels,int stride,const View&view) {
    if(!pixels||stride<W)return;
    fb=pixels;pitch=stride;
    if(background.size()==W*H)for(int y=0;y<H;++y)std::memcpy(fb+y*pitch,background.data()+y*W,W*sizeof(Color));else rect(0,0,W,H,rgb(7,8,24));
    header(view);
    switch(view.screen){
        case Screen::Home:home(view);break;case Screen::Help:help();break;
        case Screen::Sources:case Screen::Imports:case Screen::Targets:case Screen::Backups:list(view);break;
        case Screen::ExportConfirm:exportConfirm(view);break;
        case Screen::ImportConfirm:transferConfirm(view,false);break;case Screen::RestoreConfirm:transferConfirm(view,true);break;
        case Screen::ConfigureReboot:rebootConfirm(view,true);break;case Screen::RebootConfirm:rebootConfirm(view,false);break;
        case Screen::Update:update(view);break;case Screen::ConfirmUpdate:confirmation(view,false);break;
        case Screen::ConfirmArchive:confirmation(view,true);break;case Screen::Busy:busy(view);break;case Screen::Result:result(view);break;
    }
}
void uiExit(){fonts.clear();rawFont.clear();logo.clear();background.clear();fb=nullptr;pitch=W;}
} // namespace nmsaves
