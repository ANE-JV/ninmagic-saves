#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace nm::ui_text {
// Invalid sequences consume only their lead byte, so a following valid ASCII
// character is never swallowed or manufactured from malformed continuation bits.
inline bool nextUtf8(const std::string&s,size_t&offset,uint32_t&cp) {
    if(offset>=s.size())return false;
    const size_t first=offset++;const auto lead=static_cast<unsigned char>(s[first]);
    if(lead<0x80){cp=lead;return true;}
    unsigned length=0;uint32_t minimum=0;
    if(lead>=0xC2&&lead<=0xDF){length=2;cp=lead&31;minimum=0x80;}
    else if(lead>=0xE0&&lead<=0xEF){length=3;cp=lead&15;minimum=0x800;}
    else if(lead>=0xF0&&lead<=0xF4){length=4;cp=lead&7;minimum=0x10000;}
    else {cp=0xFFFD;return false;}
    if(s.size()-first<length){cp=0xFFFD;return false;}
    for(unsigned n=1;n<length;++n){const auto c=static_cast<unsigned char>(s[first+n]);if((c&0xC0)!=0x80){cp=0xFFFD;return false;}cp=(cp<<6)|(c&63);}
    if(cp<minimum||cp>0x10FFFF||(cp>=0xD800&&cp<=0xDFFF)){cp=0xFFFD;return false;}
    offset=first+length;return true;
}
inline bool validUtf8(const std::string&s) {
    for(size_t at=0;at<s.size();){uint32_t cp=0;if(!nextUtf8(s,at,cp))return false;}return true;
}
inline void ascii(std::vector<uint32_t>&out,const char*s){while(*s)out.push_back(static_cast<unsigned char>(*s++));}
// The bundled atlas covers Latin-1. Normalize punctuation and common Latin
// extensions only; never invent a transliteration of CJK or other scripts.
inline bool appendDisplay(uint32_t cp,std::vector<uint32_t>&out) {
    if(cp==0xA0||cp==0x202F||cp==0x205F||cp==0x3000||(cp>=0x2000&&cp<=0x200A)){out.push_back(' ');return true;}
    if(cp==0xAD||cp==0xFEFF||cp==0x200B||cp==0x2060)return true;
    if((cp>=0x20&&cp<=0x7E)||(cp>=0xA1&&cp<=0xFF)){out.push_back(cp);return true;}
    switch(cp) {
    case 0x2018:case 0x2019:case 0x201A:case 0x201B:case 0x2032:out.push_back('\'');return true;
    case 0x201C:case 0x201D:case 0x201E:case 0x201F:case 0x2033:out.push_back('"');return true;
    case 0x2010:case 0x2011:case 0x2012:case 0x2013:case 0x2014:case 0x2015:case 0x2212:out.push_back('-');return true;
    case 0x2026:ascii(out,"...");return true;
    case 0x2022:out.push_back(0xB7);return true;
    case 0x2122:ascii(out,"(TM)");return true;
    case 0x0152:ascii(out,"OE");return true;case 0x0153:ascii(out,"oe");return true;
    case 0x0141:out.push_back('L');return true;case 0x0142:out.push_back('l');return true;
    case 0x0110:out.push_back('D');return true;case 0x0111:out.push_back('d');return true;
    case 0x011E:out.push_back('G');return true;case 0x011F:out.push_back('g');return true;
    case 0x0130:out.push_back('I');return true;case 0x0131:out.push_back('i');return true;
    case 0x0160:out.push_back('S');return true;case 0x0161:out.push_back('s');return true;
    case 0x0178:out.push_back('Y');return true;
    case 0x0179:case 0x017B:case 0x017D:out.push_back('Z');return true;
    case 0x017A:case 0x017C:case 0x017E:out.push_back('z');return true;
    case 0xFB00:ascii(out,"ff");return true;case 0xFB01:ascii(out,"fi");return true;case 0xFB02:ascii(out,"fl");return true;
    case 0xFB03:ascii(out,"ffi");return true;case 0xFB04:ascii(out,"ffl");return true;case 0xFB05:case 0xFB06:ascii(out,"st");return true;
    default:out.push_back('?');return false;
    }
}
inline std::vector<uint32_t> displayCodepoints(const std::string&s) {
    std::vector<uint32_t>out;
    for(size_t at=0;at<s.size();){uint32_t cp=0;if(!nextUtf8(s,at,cp))out.push_back('?');else appendDisplay(cp,out);}return out;
}
inline bool canDisplayUtf8(const std::string&s) {
    std::vector<uint32_t>out;
    for(size_t at=0;at<s.size();){uint32_t cp=0;if(!nextUtf8(s,at,cp)||!appendDisplay(cp,out))return false;}
    for(auto cp:out)if(cp>' ')return true;
    return false;
}
}
