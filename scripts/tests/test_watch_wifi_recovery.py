#!/usr/bin/env python3
"""Run actual post-join recovery callbacks with a queued application and fake radio."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
s = (root / "main/boards/common/wifi_board.cc").read_text()
def method(name, next_name):
    return s[s.index("void WifiBoard::" + name):s.index(next_name, s.index("void WifiBoard::" + name))]
code = r"""
#include <atomic>
#include <mutex>
#include <functional>
#include <vector>
#include <string>
#include <cstring>
#include <cassert>
#define ESP_LOGW(...)
#define ESP_LOGE(...)
#define ESP_LOGI(...)
using esp_event_base_t = int;
constexpr int WIFI_EVENT_STA_CONNECTED=1, WIFI_EVENT_STA_DISCONNECTED=2, IP_EVENT_STA_GOT_IP=3;
constexpr int ESP_OK=0, CONNECT_TIMEOUT_SEC=60;
struct wifi_event_sta_connected_t { unsigned char ssid[32]; };
struct wifi_event_sta_disconnected_t { int reason=1; };
int connects=0, connect_result=0, timer_starts=0;
bool timer_active=false;
int esp_wifi_connect(){++connects;return connect_result;}
void esp_timer_stop(int){timer_active=false;}
bool esp_timer_is_active(int){return timer_active;}
void esp_timer_start_once(int,unsigned long long){timer_active=true;++timer_starts;}
struct Application {
 std::vector<std::function<void()>> q;
 static Application& GetInstance(){static Application a;return a;}
 void Schedule(std::function<void()> f){q.push_back(f);}
 void Drain(){while(!q.empty()){auto batch=std::move(q);q.clear();for(auto& f:batch)f();}}
};
struct WifiManager {static WifiManager& GetInstance(){static WifiManager w;return w;}bool IsConfigMode(){return false;}};
enum class NetworkEvent {Connecting,Connected,Disconnected};
struct WifiBoard {
 struct Manual {bool active=false,connect_requested=false;std::string ssid,password;} manual_join_;
 struct Raw {std::string ssid,password;bool active=false,connected=false,reconnect_pending=false;} raw_connection_;
 std::mutex manual_join_mutex_,raw_connection_mutex_;
 std::atomic<unsigned> manual_join_generation_{0},raw_connection_generation_{0};
 int connect_timer_=1, fallback=0, teardown=0, unregistered=0,connected_events=0;
 void ClearRawConnection();
 void ScheduleRawReconnect(unsigned);
 void ScheduleRawConnected(unsigned);
 static void ManualJoinWifiHandler(void*,int,int,void*);
 static void ManualJoinIpHandler(void*,int,int,void*);
 void ScheduleOutcome(unsigned,bool,std::string,std::string){}
 void UnregisterManualJoinHandlers(){++unregistered;}
 void StopRawWifi(){++teardown;}
 void TryWifiConnect(){++fallback;}
 void SetWifiStatus(std::string){}
 void OnNetworkEvent(NetworkEvent e,std::string={}){if(e==NetworkEvent::Connected)++connected_events;}
};
METHODS
int main(){
 auto& app=Application::GetInstance(); WifiBoard b;
 b.raw_connection_={"Selected","password",true,true,false};
 wifi_event_sta_disconnected_t lost;
 WifiBoard::ManualJoinWifiHandler(&b,0,WIFI_EVENT_STA_DISCONNECTED,&lost);
 assert(connects==0);app.Drain();assert(connects==1 && timer_starts==1);
 WifiBoard::ManualJoinWifiHandler(&b,0,WIFI_EVENT_STA_DISCONNECTED,&lost);
 app.Drain();assert(connects==2 && timer_starts==1); // repeated failure doesn't extend deadline
 WifiBoard::ManualJoinIpHandler(&b,0,IP_EVENT_STA_GOT_IP,nullptr);
 app.Drain();assert(b.connected_events==1 && !timer_active && b.fallback==0);
 WifiBoard::ManualJoinWifiHandler(&b,0,WIFI_EVENT_STA_DISCONNECTED,&lost);
 b.ClearRawConnection();app.Drain();assert(connects==2); // old event can't revive canceled owner
 b.raw_connection_={"Selected","password",true,true,false};connect_result=1;
 WifiBoard::ManualJoinWifiHandler(&b,0,WIFI_EVENT_STA_DISCONNECTED,&lost);
 app.Drain();assert(b.fallback==1 && b.teardown==1 && b.unregistered==1);
}
"""
methods = method("ClearRawConnection()", "void WifiBoard::CancelWifiOperations()")
methods += method("ScheduleRawReconnect(", "bool WifiBoard::RegisterManualJoinHandlers()")
methods += method("ManualJoinWifiHandler(", "// Scan completion:")
with tempfile.TemporaryDirectory(prefix="wifi-recovery-") as d:
    p=Path(d);(p/"test.cc").write_text(code.replace("METHODS",methods))
    subprocess.run(["c++","-std=c++17",str(p/"test.cc"),"-o",str(p/"test")],check=True)
    subprocess.run([str(p/"test")],check=True)
print("PASS: actual Wi-Fi callbacks reconnect, retain deadline, reject stale recovery, and fall back on failure")
