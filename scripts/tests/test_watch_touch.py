#!/usr/bin/env python3
"""Exercise the actual board-to-LVGL touch dispatch without hardware."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
board = (root / 'main/boards/waveshare/esp32-s3-touch-lcd-1.85c/esp32-s3-touch-lcd-1.85c.cc').read_text()
failure_start = board.index('if (esp_lcd_touch_read_data(touch_handle_) != ESP_OK)')
failure_end = board.index('vTaskDelete(nullptr)', failure_start)
assert 'app.OnWatchAction(WatchUi::Action::EndCall' in board[failure_start:failure_end]
start = board.index('            // Feed the toolkit once per sample;')
# End at the last statement of the per-sample touch dispatch (before the
# surrounding while-loop's closing brace). The old '#endif' terminator was
# removed with the CONFIG_VOICEMODE_CODEX_VOICE wrapper.
end = '            vTaskDelay(pdMS_TO_TICKS(kTouchPollMs));'
block = board[start:board.index(end, start) + len(end)]
program = r'''
#include <cassert>
#include <functional>
#include <vector>
struct Application {
    bool asleep=false, confirm=false;
    int wakes=0, answers=0, answer_x=0, answer_y=0;
    std::vector<std::function<void()>> queued;
    static Application& GetInstance(){static Application app; return app;}
    bool IsScreenAsleep(){return asleep;}
    bool IsConfirmActive(){return confirm;}
    void Schedule(std::function<void()> f){queued.push_back(f);}
    void NoteUserActivity(){asleep=false; ++wakes;}
    void OnConfirmTouchRelease(int x,int y){++answers;answer_x=x;answer_y=y;}
    void Drain(){for(auto& f:queued)f();queued.clear();}
};
struct Display {
    bool pressed=false;int x=0,y=0;
    void FeedTouch(bool down,int px,int py){pressed=down;x=px;y=py;}
};
void vTaskDelay(int){}
int pdMS_TO_TICKS(int x){return x;}
constexpr int kTouchPollMs=20;
struct Board {
    bool was_pressed=false,swallow_touch=false;
    int last_x=0,last_y=0;
    Display display;Display* display_=&display;
    void Sample(bool is_pressed,int x,int y){
        for(int once=0;once<1;++once){
BLOCK
        }
    }
};
int main(){
    Board b;auto& app=Application::GetInstance();
    b.Sample(true,100,120);assert(b.display.pressed);app.Drain();
    b.Sample(true,150,190);b.Sample(false,0,0);
    assert(!b.display.pressed && b.display.x==150 && b.display.y==190);
    app.asleep=true;b.Sample(true,180,180);assert(!b.display.pressed);
    app.Drain();b.Sample(true,190,190);assert(!b.display.pressed);
    b.Sample(false,0,0);b.Sample(true,200,200);assert(b.display.pressed);
    b.Sample(false,0,0);
    app.confirm=true;b.Sample(true,230,260);assert(!b.display.pressed);
    b.Sample(false,0,0);assert(app.answers==1&&app.answer_x==230&&app.answer_y==260);
}
'''.replace('BLOCK',block)
with tempfile.TemporaryDirectory(prefix='watch-touch-') as directory:
    path=Path(directory);(path/'test.cc').write_text(program)
    subprocess.run(['c++','-std=c++17',str(path/'test.cc'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
print('PASS: release keeps coordinates, wake consumes whole touch, confirmation owns input')
