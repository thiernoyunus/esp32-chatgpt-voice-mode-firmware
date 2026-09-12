#include "watch_ui.h"
#include "watch_icons.h"
#include "watch_dotmatrix.h"   /* the app-pixels look; the ChatGPT pages are moving to it */
#include "bloub/bloub_shapes.h"
#include "voice_character.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <utility>

namespace {
constexpr uint32_t kRaised = 0x181F2C, kAccent = 0x10A37F;
// The round screen's geometry, shared with the design mockups: centre, and the
// radius everything drawn has to stay inside of.
constexpr int kCenter = 180, kSafeR = 176;
constexpr int kDotNav = 44;    // smallest comfortable touch target here
constexpr int kDotRowH = 46;   // app-pixels list row
constexpr const char* kShapeNames[]={"Circle","Pebble","Squircle","Capsule","Triangle","Hexagon","Cloud","Droplet"};
static_assert(sizeof(kShapeNames)/sizeof(kShapeNames[0])==voice_character::kShapeCount,
              "a silhouette has no name, or a name has no silhouette");
constexpr const char* kColourNames[]={"Cream","Grey","Brown","Red","Orange","Amber","Green","Teal","Blue","Violet","Pink"};
static_assert(sizeof(kColourNames)/sizeof(kColourNames[0])==voice_character::kColorCount,
              "a colour has no name, or a name has no colour");
/* Straight ahead and expressionless. bloub's NEUTRAL expression is the rest
 * gaze measured off the reference video - a three-quarter view - which at
 * preview size reads as looking off to one side and leaves only one eye
 * visible, so it does not read as neutral at all. Neutral here means the
 * plain front-facing face. */
static const bloub_gaze_t kNeutralPreview={0.0f,0.0f,0.0f};
lv_obj_t* ShapePreview(lv_obj_t* parent, int shape, uint32_t color, int size=44, int scale=18) {
    auto* buf=static_cast<lv_color16_t*>(dm_alloc(size*size*sizeof(lv_color16_t)));
    if(!buf) return nullptr;
    memset(buf,0,size*size*sizeof(lv_color16_t));
    auto canvas=lv_canvas_create(parent);
    if(!canvas){dm_release(buf);return nullptr;}
    lv_canvas_set_buffer(canvas,buf,size,size,LV_COLOR_FORMAT_RGB565);
    lv_obj_set_size(canvas,size,size);
    lv_obj_add_event_cb(canvas,dm_free_buffer,LV_EVENT_DELETE,buf);
    bloub_face_cfg_t face{};face.radii=SHAPE_PROFILES[shape];face.gaze=&kNeutralPreview;
    face.split=BLOUB_EYE_SPLIT;face.scale=scale;face.cx=face.cy=size/2.0f;face.sx=face.sy=1;face.eye_alpha=1;
    for(int e=0;e<2;++e){face.eyes[e].w=.236f;face.eyes[e].h=.447f;face.eyes[e].open=1;}
    bloub_draw_face(reinterpret_cast<uint16_t*>(buf),size,size,&face,lv_color_to_u16(lv_color_hex(color)),0);
    return canvas;
}
int ChordHalf(int y) {
    const int dy = y - kCenter;
    const int inside = kSafeR * kSafeR - dy * dy;
    return inside > 0 ? static_cast<int>(std::sqrt(static_cast<float>(inside))) : 0;
}
void Background(lv_obj_t* obj) {
    lv_obj_set_style_bg_color(obj, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
}
// Render an icon at its source size, except 24px controls placed inside a 96px
// home tile which are upscaled to 48px so they read at the same visual weight as
// the 52px ChatGPT icon. The layout size matches the rendered size so centering
// is exact. Row/control parents (32px chip, 62px row) stay at native 24px.
void Icon(lv_obj_t* parent, const lv_image_dsc_t* source) {
    auto image = lv_image_create(parent);
    lv_image_set_src(image, source);
    if (source->header.w == 24) {
        const int pw = lv_obj_get_style_width(parent, LV_PART_MAIN);
        const int ph = lv_obj_get_style_height(parent, LV_PART_MAIN);
        if (pw >= 96 && ph >= 96) {
            lv_image_set_scale(image, 512);            // 24 -> 48
            lv_obj_set_size(image, 48, 48);            // match rendered size
        }
    }
    lv_obj_set_style_image_recolor(image, lv_color_white(), 0);
    lv_obj_set_style_image_recolor_opa(image, LV_OPA_COVER, 0);
    lv_obj_remove_flag(image, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(image);
}
// Click handler. The lambda is heap-allocated and owned by the button; we free
// it on LV_EVENT_DELETE so a lambda that captures `this` cannot outlive its
// button. We copy the function before invoking so a click handler that deletes
// its own button mid-navigation cannot leave us holding a freed function.
void Click(lv_obj_t* obj, std::function<void()> fn) {
    if (!fn) return;
    auto owned = new std::function<void()>(std::move(fn));
    lv_obj_add_event_cb(obj, [](lv_event_t* e) {
        auto fn = static_cast<std::function<void()>*>(lv_event_get_user_data(e));
        if (lv_event_get_code(e) == LV_EVENT_DELETE) { delete fn; return; }
        if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
            auto invoke = *fn;
            invoke();
        }
    }, LV_EVENT_ALL, owned);
}
bool IsPrintableAscii(char c) { return c >= 0x20 && c <= 0x7E; }
bool IsHex(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
}

WatchUi::WatchUi(lv_obj_t* voice, std::shared_ptr<LvglFont> font, Callback callback)
    : voice_(voice), font_owner_(std::move(font)),
      font_(font_owner_ == nullptr ? nullptr : font_owner_->font()), callback_(std::move(callback)) {
    shell_ = Box(lv_obj_get_parent(voice), 0, 0, 360, 360, 0x000000, 0);
    lv_obj_set_style_text_font(shell_, font_, 0);
    lv_obj_set_style_text_color(shell_, lv_color_white(), 0);
    Background(shell_);
    Show(Page::Home);
}
WatchUi::~WatchUi() { lv_obj_delete(shell_); }
void WatchUi::Emit(Action a, int n, const std::string& text, const std::string& secret) {
    if (callback_) callback_(a, n, text, secret);
}
void WatchUi::SetFont(std::shared_ptr<LvglFont> font) {
    if (font == nullptr || font->font() == nullptr) return;
    font_owner_ = std::move(font);
    font_ = font_owner_->font();
    if (shell_ != nullptr) lv_obj_set_style_text_font(shell_, font_, 0);
}
lv_obj_t* WatchUi::Box(lv_obj_t* p, int x, int y, int w, int h, uint32_t c, int r) {
    auto o = lv_obj_create(p);
    lv_obj_set_pos(o, x, y); lv_obj_set_size(o, w, h);
    lv_obj_set_style_radius(o, r, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(c), 0);
    lv_obj_set_style_border_width(o, 0, 0); lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}
lv_obj_t* WatchUi::Label(lv_obj_t* p, const char* text, int width) {
    auto l = lv_label_create(p); lv_label_set_text(l, text);
    lv_obj_set_style_text_color(l, lv_color_white(), 0);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    if (width) { lv_obj_set_width(l, width); lv_label_set_long_mode(l, LV_LABEL_LONG_DOT); }
    return l;
}
lv_obj_t* WatchUi::Button(lv_obj_t* p, int x, int y, int w, int h, const char* text,
                         const lv_image_dsc_t* icon, std::function<void()> fn, uint32_t color) {
    auto b = Box(p,x,y,w,h,color, std::min(26,h/2));
    lv_obj_remove_flag(b, LV_OBJ_FLAG_CLICKABLE);
    if (fn) {
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(b, lv_color_hex(0x344052), LV_STATE_PRESSED);
    }
    if (icon) Icon(b,icon);
    else { auto l=Label(b,text,w-8); lv_obj_set_style_text_align(l,LV_TEXT_ALIGN_CENTER,0); lv_obj_center(l); }
    Click(b,std::move(fn)); return b;
}
void WatchUi::Header(const char* title, Page back) {
    if(dot_style_){
        // Back arrow and title on ONE line, centred as a pair - the way the
        // device's own ChatGPT sheet already lays it out, and why the arrow
        // left the corner. The title steps down one dot size when the pair
        // cannot clear the circle: the button is a fixed 44px touch target and
        // must not shrink.
        dm_style_t st={4,3,1,kAccent,0x101010};
        const int cy=70;
        const int half=ChordHalf(cy-kDotNav/2)-10;
        int w=dm_width(title,&st);
        // Step the title down a dot size until the pair clears the circle. The
        // button is a fixed 44px touch target and must not shrink, so a title
        // too long for one line pays for it in size instead.
        while(st.pitch>2&&kDotNav+12+w>2*half){st.pitch--;st.dot--;w=dm_width(title,&st);}
        const int x0=kCenter-(kDotNav+12+w)/2;
        Button(shell_,x0,cy-kDotNav/2,kDotNav,kDotNav,"",&watch_icons::back,[this,back]{Show(back);},0x1A1A1A);
        dm_text(shell_,x0+kDotNav+12,cy-DM_H*st.pitch/2,title,&st);
        Box(shell_,84,cy+26,192,1,0x2A2A33,0);   // hairline, not a filled bar
        return;
    }
   Button(shell_,66,38,44,44,"",&watch_icons::back,[this,back]{Show(back);});
    auto l=Label(shell_,title,154);lv_obj_set_pos(l,116,49);
}
lv_obj_t* WatchUi::Column() {
    auto c=Box(shell_,62,dot_style_?100:90,236,dot_style_?212:220,0,0);
    lv_obj_set_style_bg_opa(c,LV_OPA_TRANSP,0);
    lv_obj_add_flag(c,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(c,LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c,dot_style_?0:10,0);
    lv_obj_set_scroll_dir(c,LV_DIR_VER);
    lv_obj_set_scrollbar_mode(c,LV_SCROLLBAR_MODE_OFF);
    return column_=c;
}
// Row is read-only when `fn` is empty: no chevron, no navigation behavior.
// The bool is captured before std::move so the chevron stays consistent.
void WatchUi::Row(const char* title,const char* value,const lv_image_dsc_t* icon,std::function<void()> fn) {
    if(dot_style_){
        // Plain row: name left, value right on the SAME line, no icon chip and
        // no chevron. Only the current row is filled, and it carries a 3px
        // accent rule on the left - which is what makes the list scannable with
        // no other chrome at all. A page marks its current row by passing "On"
        // as that row's value.
        const bool current=value!=nullptr&&strcmp(value,"On")==0;
        auto r=Box(column_,0,0,236,kDotRowH,current?0x141414:0x000000,2);
        lv_obj_set_style_bg_opa(r,current?LV_OPA_COVER:LV_OPA_TRANSP,0);
        if(current){auto s=Box(r,0,0,3,kDotRowH,kAccent,0);lv_obj_remove_flag(s,LV_OBJ_FLAG_CLICKABLE);}
        if(fn) Click(r,std::move(fn));
        dm_style_t n={3,2,1,current?0xFFFFFFu:0x8E8E93u,0x101010u};
        dm_style_t v={3,2,1,current?kAccent:0x5A5A5Fu,0x101010u};
        const bool has_value=value!=nullptr&&value[0]!=0;
        // The row has to hold a name and a value on ONE line, and nine
        // characters of name at pitch 3 leaves no room for one. Both step down
        // to pitch 2 together - the same trade the header makes with its title.
        if(has_value&&14+dm_width(title,&n)+16+dm_width(value,&v)>236-12){
            n.pitch=2;n.dot=1;v.pitch=2;v.dot=1;
        }
        dm_text(r,14,(kDotRowH-DM_H*n.pitch)/2,title,&n);
        if(has_value){
            const int vw=dm_width(value,&v);
            // Anything that still cannot fit beside its name is dropped rather
            // than written over it - a long chat title, usually.
            if(14+dm_width(title,&n)+16+vw<=236-12)
                dm_text(r,236-12-vw,(kDotRowH-DM_H*v.pitch)/2,value,&v);
        }
       return;
    }
   const bool navigable = static_cast<bool>(fn);
    auto r=Button(column_,0,0,236,62,"",nullptr,std::move(fn));
    lv_obj_clean(r);
    if(icon){auto chip=Box(r,10,15,32,32,0x232C3A,16);Icon(chip,icon);lv_obj_remove_flag(chip,LV_OBJ_FLAG_CLICKABLE);}
    const int left=icon?52:14;
    auto l=Label(r,title,210-left);lv_obj_set_pos(l,left,value?8:21);
    if(value){auto v=Label(r,value,210-left);lv_obj_set_style_text_color(v,lv_color_hex(0xAEB6C4),0);lv_obj_set_pos(v,left,34);}
    if(navigable){
        auto chevron=Label(r,">",12);
        lv_obj_set_pos(chevron,216,22);
        lv_obj_remove_flag(chevron,LV_OBJ_FLAG_CLICKABLE);
    }
}
void WatchUi::Show(Page page) {
    page_=page;
    // Every page wears the app-pixels language except the three that are not
    // lists: Home keeps its icon tiles by request, the call screen is the
    // character, and the keyboard and Wi-Fi setup bring their own layout.
    switch(page){
    case Page::Home: case Page::Voice: case Page::Keyboard: case Page::WifiSetup:
    case Page::Sleep:
        dot_style_=false;break;
    default: dot_style_=true;break;
    }
    if(page==Page::Voice){
        lv_obj_add_flag(shell_,LV_OBJ_FLAG_HIDDEN);lv_obj_remove_flag(voice_,LV_OBJ_FLAG_HIDDEN);return;
    }
    lv_obj_add_flag(voice_,LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(shell_,LV_OBJ_FLAG_HIDDEN);lv_obj_move_foreground(shell_);
    lv_obj_clean(shell_);column_=clock_=date_=value_=field_=keys_=wifi_status_=notice_=error_=nullptr;
    keyboard_done_={};
    // App-pixels pages are pure black with no gradient; the rest keep the navy
    // watch background they have always had.
    if(dot_style_){lv_obj_set_style_bg_color(shell_,lv_color_hex(0x000000),0);lv_obj_set_style_bg_grad_dir(shell_,LV_GRAD_DIR_NONE,0);}
    else Background(shell_);
    switch(page){
    case Page::Home: {
        clock_=Label(shell_,time_.c_str());lv_obj_align(clock_,LV_ALIGN_TOP_MID,0,30);
        // ChatGPT: show voice page but don't auto-connect; user taps orb to connect
        Button(shell_,74,82,96,96,"",&watch_icons::chatgpt,[this]{Show(Page::Voice);},kAccent);
        Button(shell_,190,82,96,96,"",&watch_icons::settings,[this]{Show(Page::Settings);Emit(Action::Refresh);},0x424D61);
        Button(shell_,74,212,96,96,"",&watch_icons::clock,[this]{Show(Page::Clock);},0x3354A4);
        // Home tile labels — 3 tiles centred in the chord-safe area
        const char* names[]={"ChatGPT","Settings","Clock"};
        for(int i=0;i<3;++i){
            auto l=Label(shell_,names[i],80);
            lv_obj_set_style_text_align(l,LV_TEXT_ALIGN_CENTER,0);
            lv_obj_set_pos(l,82+(i%2)*116,180+(i/2)*130);
        }
        break;
    }
    case Page::Settings:
        Header("Settings",Page::Home);Column();
        Row("Wi-Fi",info_.network.empty()?"Not connected":info_.network.c_str(),&watch_icons::wifi,[this]{Show(Page::Wifi);Emit(Action::ScanWifi);});
        Row("Brightness",nullptr,&watch_icons::sun,[this]{Show(Page::Brightness);});
        Row("Volume",nullptr,&watch_icons::volume,[this]{Show(Page::Volume);});
        Row("About",info_.version.c_str(),&watch_icons::info,[this]{Show(Page::About);});break;
    case Page::Brightness: {
        Header("Brightness",Page::Settings);
        const int initial=info_.brightness;
        value_=Label(shell_,(std::to_string(initial)+"%").c_str());
        lv_obj_align(value_,LV_ALIGN_TOP_MID,0,100);
        auto slider=lv_slider_create(shell_);
        lv_obj_set_size(slider,216,18);
        lv_obj_align(slider,LV_ALIGN_TOP_MID,0,140);
        lv_obj_set_ext_click_area(slider,18);
        lv_slider_set_range(slider,5,100);
        lv_slider_set_value(slider,initial,LV_ANIM_OFF);
        lv_obj_set_style_bg_color(slider,lv_color_hex(kAccent),LV_PART_INDICATOR);
        lv_obj_add_event_cb(slider,[](lv_event_t* e){
            auto self=static_cast<WatchUi*>(lv_event_get_user_data(e));
            int value=lv_slider_get_value(static_cast<lv_obj_t*>(lv_event_get_target(e)));
            lv_label_set_text(self->value_,(std::to_string(value)+"%").c_str());
            if(lv_event_get_code(e)==LV_EVENT_RELEASED){
                self->info_.brightness=value;
                self->Emit(Action::Brightness,value);
            }
        },LV_EVENT_VALUE_CHANGED,this);
        lv_obj_add_event_cb(slider,[](lv_event_t* e){
            auto self=static_cast<WatchUi*>(lv_event_get_user_data(e));
            int v=lv_slider_get_value(static_cast<lv_obj_t*>(lv_event_get_target(e)));
            self->info_.brightness=v;
            self->Emit(Action::Brightness,v);
        },LV_EVENT_RELEASED,this);
        // Sleep timeout row sits inside the chord-safe band below the slider
        // (y=200..262) so it doesn't overlap Cancel-style chrome.
        // sleep_value is a std::string local; its .c_str() outlives the call.
        const std::string sleep_value = info_.sleep_seconds == 0
            ? std::string("Always on")
            : std::to_string(info_.sleep_seconds) + " s";
        column_ = Box(shell_, 62, 200, 236, 62, 0, 0);
        lv_obj_set_style_bg_opa(column_, LV_OPA_TRANSP, 0);
        Row("Sleep timeout", sleep_value.c_str(), &watch_icons::clock,
            [this]{ Show(Page::Sleep); });
        break;
    }
    case Page::Volume: Header("Volume",Page::Settings);Slider(false);break;
    case Page::Wifi:
        Header("Wi-Fi",Page::Settings);Column();
        wifi_status_=Label(column_,info_.wifi_status.empty()?(info_.network.empty()?"Not connected":info_.network.c_str()):info_.wifi_status.c_str(),236);
        for(const auto& ssid:info_.networks){
            const bool saved=std::find(info_.saved_networks.begin(),info_.saved_networks.end(),ssid)!=info_.saved_networks.end();
            Row(ssid.c_str(),ssid==info_.network?"Connected":saved?"Saved":"Join network",&watch_icons::wifi,[this,ssid]{ChooseNetwork(ssid);});
        }
        Row("Other network", "Enter Wi-Fi name",&watch_icons::wifi,[this]{
            OpenKeyboard(FieldKind::Ssid,"Wi-Fi name","",[this](const std::string& name){ ChooseNetwork(name); });
        });
        Row("Scan again",nullptr,&watch_icons::wifi,[this]{Emit(Action::ScanWifi);});
        Row("Phone setup",nullptr,&watch_icons::more,[this]{Show(Page::WifiSetup);});break;
    case Page::WifiSetup: {
        Header("Phone setup",Page::Wifi);
        auto l=Label(shell_,"This ends the voice call.\nJoin the Apollo hotspot\non your phone, then open\n192.168.4.1",230);
        lv_label_set_long_mode(l,LV_LABEL_LONG_WRAP);lv_obj_set_style_text_align(l,LV_TEXT_ALIGN_CENTER,0);lv_obj_set_pos(l,65,112);
        Button(shell_,90,244,180,52,"Start setup",nullptr,[this]{Emit(Action::SetupWifi);},kAccent);break;
    }
    case Page::Sleep: {
        Header("Sleep timeout", Page::Brightness); Column();
        struct Opt { int seconds; const char* label; };
        const Opt opts[] = {
            {0,   "Always on"},
            {30,  "30 seconds"},
            {60,  "1 minute"},
            {120, "2 minutes"},
            {300, "5 minutes"},
        };
        for (const auto& opt : opts) {
            const bool current = info_.sleep_seconds == opt.seconds;
            // No chevron: tapping the row is the affordance; a > would imply
            // a sub-page that does not exist.
            const char* value_text = current ? "On" : nullptr;
            Row(opt.label, value_text, nullptr, [this, opt] {
                info_.sleep_seconds = opt.seconds;
                Emit(Action::Sleep, opt.seconds);
                Show(Page::Brightness);
            });
            if (current) {
                auto top = lv_obj_get_child(column_, lv_obj_get_child_cnt(column_) - 1);
                lv_obj_set_style_bg_color(top, lv_color_hex(0x232C3A), 0);
            }
        }
        break;
    }
    case Page::Clock: {
        Header("Clock",Page::Home);clock_=Label(shell_,time_.c_str());lv_obj_set_style_transform_scale(clock_,384,0);lv_obj_align(clock_,LV_ALIGN_CENTER,0,-20);
        date_=Label(shell_,date_text_.c_str(),220);lv_obj_set_style_text_align(date_,LV_TEXT_ALIGN_CENTER,0);lv_obj_align(date_,LV_ALIGN_CENTER,0,36);
        auto l=Label(shell_,info_.connected?"Wi-Fi connected":"Waiting for Wi-Fi",220);lv_obj_set_style_text_align(l,LV_TEXT_ALIGN_CENTER,0);lv_obj_align(l,LV_ALIGN_CENTER,0,80);break;
    }
    case Page::About:{
        Header("About Apollo",Page::Settings);Column();
        Row("Firmware",info_.version.c_str(),&watch_icons::info,{});
        Row("Voice","Codex Voice / WebRTC",&watch_icons::mic,{});
        std::string battery=info_.battery<0?"Not available":std::to_string(info_.battery)+"%"+(info_.charging?" - charging":"");
        Row("Battery",battery.c_str(),&watch_icons::info,{});
        Row("Display","360 x 360",&watch_icons::sun,{});break;
    }
    case Page::CodexSettings:
        Header("ChatGPT",Page::Voice);Column();
        Row("Shape",kShapeNames[std::clamp(info_.shape,0,voice_character::kShapeCount-1)],nullptr,[this]{Show(Page::Shapes);});
        Row("Colour",kColourNames[std::clamp(info_.colour,0,voice_character::kColorCount-1)],nullptr,[this]{Show(Page::Colours);});
        Row("Voice",info_.voice.empty()?"Default":info_.voice.c_str(),&watch_icons::mic,[this]{Show(Page::Voices);});
        Row("Model",info_.model.c_str(),&watch_icons::more,[this]{model_return_=Page::CodexSettings;Show(Page::Models);Emit(Action::Models);});
        Row("Chat",info_.temporary_chat?"Temporary":info_.chat.c_str(),&watch_icons::more,[this]{Show(Page::Chats);Emit(Action::Models);});
        Row("Reasoning",info_.reasoning.c_str(),&watch_icons::more,[this]{Show(Page::Reasoning);});
        Row("Captions",info_.captions?"On":"Off",nullptr,[this]{
            info_.captions=!info_.captions;
            Emit(Action::Captions,info_.captions?1:0);
            Show(Page::CodexSettings);
        });break;
    case Page::Shapes: {
        Header("Shape",Page::CodexSettings);Column();
        // Four rows fill the space between the header rule and the bottom of
        // the circle; the rest stay reachable by swiping. No counter: the list
        // is short enough to see, and the number was just sitting in space.
        lv_obj_set_height(column_,208);
        for(int i=0;i<voice_character::kShapeCount;++i){
            Row(kShapeNames[i],info_.shape==i?"On":nullptr,nullptr,[this,i]{
                info_.shape=i; Emit(Action::SelectShape,i); Show(Page::Shapes);
            });
            auto row=lv_obj_get_child(column_,lv_obj_get_child_cnt(column_)-1);
            lv_obj_set_height(row,52);   /* fill the page rather than 46 of it */
            // Row() creates the title canvas immediately after the optional
            // selection stripe. Give the larger face a dedicated left lane so
            // it never covers the first letters of the name.
            const uint32_t title_index=info_.shape==i?1:0;
            if(lv_obj_get_child_cnt(row)>title_index){
                lv_obj_set_x(lv_obj_get_child(row,title_index),50);
                lv_obj_set_y(lv_obj_get_child(row,title_index),15);
            }
            if(auto preview=ShapePreview(row,i,info_.shape==i?0xF1EFE9:0x8E8E93))
                lv_obj_set_pos(preview,2,4);
        }
        break;
    }
    case Page::Colours: {
        Header("Colour",Page::CodexSettings);
        const int row_n[]={3,4,4}, row_y[]={140,202,264}; int at=0;
        // The rows have to account for every colour: one short and the last is
        // unreachable, one over and this walks off the end of the palette.
        static_assert(3+4+4==voice_character::kColorCount,
                      "the colour grid's rows no longer add up to the palette");
        for(int row=0;row<3;++row) for(int col=0;col<row_n[row];++col,++at){
            const int cx=180+(col*54-(row_n[row]-1)*27);
            auto swatch=Box(shell_,cx-23,row_y[row]-23,46,46,voice_character::kColors[at],12);
            Click(swatch,[this,at]{info_.colour=at;Emit(Action::SelectColour,at);Show(Page::Colours);});
            if(info_.colour==at){
                lv_obj_set_style_border_width(swatch,2,0);
                lv_obj_set_style_border_color(swatch,lv_color_white(),0);
                lv_obj_set_style_outline_width(swatch,3,0);
                lv_obj_set_style_outline_color(swatch,lv_color_black(),0);
            }
        }
        break;
    }
    case Page::Voices: {
        Header("Voice",Page::CodexSettings);Column();
        // Names come from the app-server's v1 realtime voice set, which is what
        // a v3 ChatGPT Voice call accepts. "Default" clears the saved choice.
        const char* voices[]={"Default","Cove","Juniper","Maple","Spruce","Ember",
                              "Vale","Breeze","Arbor","Sol"};
        for(const auto& voice:voices){
            const bool current=info_.voice.empty()?strcmp(voice,"Default")==0:info_.voice==voice;
            Row(voice,current?"On":nullptr,nullptr,[this,voice]{
                info_.voice=strcmp(voice,"Default")==0?std::string():voice;
                Emit(Action::SelectVoice,0,info_.voice);
                Show(Page::CodexSettings);
            });
            if(current){
                auto top=lv_obj_get_child(column_,lv_obj_get_child_cnt(column_)-1);
                lv_obj_set_style_bg_color(top,lv_color_hex(0x232C3A),0);
            }
        }
        break;
    }
    case Page::Chats: {
        Header("Chat",Page::CodexSettings);Column();
        // Temporary chats stay out of Codex, so the picker is pointless then.
        Row("Temporary chat",info_.temporary_chat?"On":"Off",nullptr,[this]{
            info_.temporary_chat=!info_.temporary_chat;
            Emit(Action::TemporaryChat,info_.temporary_chat?1:0);
            Show(Page::Chats);
        });
        if(info_.temporary_chat){
            auto l=Label(column_,"Calls stay out of Codex\nuntil you turn this off.",236);
            lv_label_set_long_mode(l,LV_LABEL_LONG_WRAP);
            break;
        }
        Row("New chat",info_.chat=="New chat"?"On":nullptr,nullptr,[this]{
            Emit(Action::SelectChat,0);Show(Page::CodexSettings);
        });
        if(info_.chats.empty()){
            auto l=Label(column_,"Open a voice call to load\nyour recent chats.",236);
            lv_label_set_long_mode(l,LV_LABEL_LONG_WRAP);
        }
        for(size_t i=0;i<info_.chats.size();++i){
            const bool current=info_.chat==info_.chats[i];
            Row(info_.chats[i].c_str(),current?"On":nullptr,nullptr,[this,i]{
                Emit(Action::SelectChat,static_cast<int>(i+1));Show(Page::CodexSettings);
            });
            if(current){
                auto top=lv_obj_get_child(column_,lv_obj_get_child_cnt(column_)-1);
                lv_obj_set_style_bg_color(top,lv_color_hex(0x232C3A),0);
            }
        }
        break;
    }
    case Page::Models:
        Header("Model",model_return_);Column();
        if(info_.models.size()<=1) {auto l=Label(column_,"Open a voice call to load\nyour available models.",236);lv_label_set_long_mode(l,LV_LABEL_LONG_WRAP);}
        for(size_t i=0;i<info_.models.size();++i){Row(info_.models[i].c_str(),nullptr,nullptr,[this,i]{Emit(Action::SelectModel,static_cast<int>(i));Show(model_return_);});}break;
    case Page::Approvals: {
        Header("Approvals",Page::CodexSettings);
        auto l=Label(shell_,"Approval requests appear\non this screen.\n\nYour approval policy is\nmanaged in Codex.",230);
        lv_label_set_long_mode(l,LV_LABEL_LONG_WRAP);lv_obj_set_style_text_align(l,LV_TEXT_ALIGN_CENTER,0);lv_obj_set_pos(l,65,110);break;
    }
    case Page::Reasoning: {
        Header("Reasoning",Page::CodexSettings);Column();
        const char* levels[]={"Low","Medium","High","XHigh","Max","Ultra"};
        for(const auto& level:levels){
            const bool current=info_.reasoning==level;
            Row(level,current?"On":nullptr,nullptr,[this,level]{
                info_.reasoning=level;
                Emit(Action::SelectReasoning,0,level);
                Show(Page::CodexSettings);
            });
            if(current){
                auto top=lv_obj_get_child(column_,lv_obj_get_child_cnt(column_)-1);
                lv_obj_set_style_bg_color(top,lv_color_hex(0x232C3A),0);
            }
        }
        break;
    }
    default:break;
    }
    UpdateNotice();
}
void WatchUi::UpdateNotice() {
    if (notice_) { lv_obj_delete(notice_); notice_ = nullptr; }
    if (!info_.notice.empty() && page_ != Page::Voice) {
        notice_ = Box(shell_, 48, 148, 264, 64, 0x344052, 16);
        auto notice_label = Label(notice_, info_.notice.c_str(), 248);
        lv_label_set_long_mode(notice_label, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(notice_label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(notice_label);
    }
}
void WatchUi::Slider(bool brightness) {
    const int initial=brightness?info_.brightness:info_.volume;
    value_=Label(shell_,(std::to_string(initial)+"%").c_str());lv_obj_align(value_,LV_ALIGN_CENTER,0,-45);
    auto slider=lv_slider_create(shell_);lv_obj_set_size(slider,216,18);lv_obj_set_pos(slider,72,180);
    lv_obj_set_ext_click_area(slider,18);lv_slider_set_range(slider,brightness?5:0,100);lv_slider_set_value(slider,initial,LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider,lv_color_hex(kAccent),LV_PART_INDICATOR);
    lv_obj_add_event_cb(slider,[](lv_event_t* e){
        auto self=static_cast<WatchUi*>(lv_event_get_user_data(e));
        int value=lv_slider_get_value(static_cast<lv_obj_t*>(lv_event_get_target(e)));
        lv_label_set_text(self->value_,(std::to_string(value)+"%").c_str());
        if(lv_event_get_code(e)==LV_EVENT_RELEASED){
            bool brightness=self->page_==Page::Brightness;
            (brightness?self->info_.brightness:self->info_.volume)=value;
            self->Emit(brightness?Action::Brightness:Action::Volume,value);
        }
    },LV_EVENT_VALUE_CHANGED,this);
    lv_obj_add_event_cb(slider,[](lv_event_t* e){
        auto self=static_cast<WatchUi*>(lv_event_get_user_data(e));
        int v=lv_slider_get_value(static_cast<lv_obj_t*>(lv_event_get_target(e)));
        bool b=self->page_==Page::Brightness;(b?self->info_.brightness:self->info_.volume)=v;
        self->Emit(b?Action::Brightness:Action::Volume,v);
    },LV_EVENT_RELEASED,this);
    auto hint=Label(shell_,"Saved when you lift your finger",230);lv_obj_set_style_text_align(hint,LV_TEXT_ALIGN_CENTER,0);lv_obj_set_pos(hint,65,232);
}
void WatchUi::SetInfo(const Info& info) {
    bool wifi_changed=info_.networks!=info.networks||info_.network!=info.network||info_.wifi_status!=info.wifi_status;
    bool models_changed=info_.models!=info.models;
    bool chats_changed=info_.chats!=info.chats||info_.chat!=info.chat||info_.temporary_chat!=info.temporary_chat;
    // Captions live on the ChatGPT page, not the Chats page, so they need
    // their own trigger - folded in with the chats it would only ever have
    // redrawn a page the row is not on.
    const bool captions_changed=info_.captions!=info.captions;
    bool sleep_changed=info_.sleep_seconds!=info.sleep_seconds;
    bool notice_changed=info_.notice!=info.notice;
    info_=info;
    if((page_==Page::Wifi&&wifi_changed)
       ||(page_==Page::Models&&models_changed)
       ||(page_==Page::Chats&&chats_changed)
       ||(page_==Page::Sleep&&sleep_changed)
       ||(page_==Page::Brightness&&sleep_changed)
       ||(page_==Page::CodexSettings&&captions_changed)) Show(page_);
    else if (notice_changed) UpdateNotice();
}
void WatchUi::Tick(const char* clock,const char* date){
    time_=clock;date_text_=date;
    if(clock_)lv_label_set_text(clock_,clock);
    if(date_)lv_label_set_text(date_,date);
}
void WatchUi::SetCallActive(bool active){
    if(active&&!call_active_&&page_==Page::Home)Show(Page::Voice);
    call_active_=active;
}
void WatchUi::ChooseNetwork(const std::string& ssid){
    join_ssid_=ssid;
    if(std::find(info_.saved_networks.begin(),info_.saved_networks.end(),ssid)!=info_.saved_networks.end()){
        Emit(Action::JoinWifi,1,ssid);Show(Page::Wifi);return;
    }
    OpenKeyboard(FieldKind::Password,"Wi-Fi password","",[this,ssid](std::string password){
        Emit(Action::JoinWifi,0,ssid,password);
        std::fill(password.begin(), password.end(), '\0');
        Show(Page::Wifi);
    });
}
void WatchUi::ShowKeyboardError(const char* text){
    if(!error_){
        // Error sits between the key area and the action buttons; it never
        // appears over a key, so an invalid input never obscures what the
        // user already typed.
        error_=Label(shell_,"",220);
        lv_obj_set_style_text_align(error_,LV_TEXT_ALIGN_CENTER,0);
        lv_label_set_long_mode(error_,LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_color(error_,lv_color_hex(0xF25C54),0);
        lv_obj_set_pos(error_,70,250);
    }
    lv_label_set_text(error_,text);
}
void WatchUi::ClearKeyboardError(){
    if(error_){lv_label_set_text(error_,"");}
}
void WatchUi::OpenKeyboard(FieldKind kind,const std::string& title,const std::string& initial,
                           std::function<void(const std::string&)> done){
    keyboard_kind_=kind;
    if (page_ != Page::Keyboard) keyboard_return_=page_;
    Show(Page::Keyboard);
    keyboard_done_=std::move(done);
    key_page_=0;uppercase_=false;
    auto l=Label(shell_,title.c_str(),220);
    lv_obj_set_style_text_color(l,lv_color_white(),0);
    lv_obj_set_style_text_align(l,LV_TEXT_ALIGN_CENTER,0);
    lv_obj_set_pos(l,70,34);
    field_=lv_textarea_create(shell_);
    lv_obj_set_pos(field_,70,62);lv_obj_set_size(field_,220,34);
    lv_obj_set_style_pad_all(field_,5,0);
    lv_obj_set_style_bg_color(field_,lv_color_hex(0x232C3A),0);
    lv_obj_set_style_text_color(field_,lv_color_white(),0);
    lv_obj_set_style_border_color(field_,lv_color_hex(0x58667A),0);
    lv_obj_set_style_border_width(field_,1,0);
    lv_textarea_set_one_line(field_,true);
    const bool secret = kind == FieldKind::Password;
    const int max_len = kind == FieldKind::Password ? 64 : 32;
    lv_textarea_set_max_length(field_,max_len);
    lv_textarea_set_password_mode(field_,secret);
    if(secret)lv_textarea_set_password_show_time(field_,0);
    lv_textarea_set_text(field_,initial.c_str());
    // Keys container spans the chord-safe key area only. The function row sits
    // at the bottom of the container, and Cancel/Submit live on shell_ below.
    keys_=Box(shell_,0,100,360,156,0,0);
    lv_obj_set_style_bg_opa(keys_,LV_OPA_TRANSP,0);
    KeyboardKeys();
    const char* submit_label = kind == FieldKind::Ssid ? "Next"
                            : kind == FieldKind::Password ? "Join"
                            : "Submit";
    Button(shell_,90,278,84,44,"Cancel",nullptr,[this]{
        if(field_){ lv_textarea_set_text(field_,""); }
        ClearKeyboardError();
        Show(keyboard_return_);
    });
    Button(shell_,186,278,84,44,submit_label,nullptr,[this]{
        if(!field_) return;
        std::string value=lv_textarea_get_text(field_);
        // Validate per FieldKind. On invalid: show inline error and stay on
        // the keyboard page. On valid: hand off to the caller; the caller
        // owns navigation (ChooseNetwork, JoinWifi, etc.).
        if(keyboard_kind_==FieldKind::Ssid){
            if(value.empty()||value.size()>32||value.find('\0')!=std::string::npos){
                ShowKeyboardError("Enter 1-32 characters");
                return;
            }
        } else if(keyboard_kind_==FieldKind::Password){
            const bool empty_open=value.empty();
            const bool psk63=value.size()>=8&&value.size()<=63
                &&std::all_of(value.begin(),value.end(),IsPrintableAscii);
            const bool hex64=value.size()==64
                &&std::all_of(value.begin(),value.end(),IsHex);
            if(!(empty_open||psk63||hex64)){
                ShowKeyboardError("Password must be 8-63 chars or 64 hex");
                return;
            }
        }
        ClearKeyboardError();
        auto done=keyboard_done_;
        lv_textarea_set_text(field_,"");
        if(done) done(value);
        std::fill(value.begin(), value.end(), '\0');
    },kAccent);
}
void WatchUi::KeyboardKeys(){
    lv_obj_clean(keys_);
    const char* letters[]={"qwertyuiop","asdfghjkl","zxcvbnm"};
    const char* symbols[]={"1234567890","@#$%&-+()","!\"':;?/\\"};
    const char* extra[]={"[]{}<>_=~","`^*.,|", "0123456789"};
    for(int row=0;row<3;++row){
        std::string chars=key_page_==0?letters[row]:key_page_==1?symbols[row]:extra[row];
        if(uppercase_&&key_page_==0)for(char& c:chars)c=static_cast<char>(c-'a'+'A');
        const int y_abs=104+40*row,h=32;
        int dy=std::max(std::abs(y_abs-180),std::abs(y_abs+h-180));
        int avail=static_cast<int>(2*std::sqrt(176*176-dy*dy))-12;
        int w=(avail-(static_cast<int>(chars.size())-1)*4)/static_cast<int>(chars.size());
        int x=180-(w*static_cast<int>(chars.size())+4*(static_cast<int>(chars.size())-1))/2;
        for(char c:chars){
            std::string key(1,c);
            Button(keys_,x,y_abs-92,w,h,key.c_str(),nullptr,
                   [this,key]{ if(field_) lv_textarea_add_text(field_,key.c_str()); },
                   0x232C3A);
            x+=w+4;
        }
    }
    // Function row at relative y=124 (absolute y=224), h=28 — sits between
    // the last key row (y_abs=176..208) and Cancel/Submit (y_abs=278).
    Button(keys_,26,124,66,28,key_page_==0?"123":key_page_==1?"#+=":"abc",nullptr,
           [this]{ key_page_=(key_page_+1)%3; KeyboardKeys(); });
    Button(keys_,98,124,48,28,"Aa",nullptr,
           [this]{ uppercase_=!uppercase_; key_page_=0; KeyboardKeys(); });
    Button(keys_,152,124,98,28,"space",nullptr,
           [this]{ if(field_) lv_textarea_add_text(field_," "); });
    Button(keys_,256,124,76,28,"Del",nullptr,
           [this]{ if(field_) lv_textarea_delete_char(field_); });
}
