import http.server
import json
import datetime
import os
import threading
import sys
import time

sys.stdout.reconfigure(line_buffering=True)

# ================== 配置区 ==================
LOG_DIR = os.path.join(os.path.expanduser("~"), "Documents", "TouchLogs")
LOG_FILE = os.path.join(LOG_DIR, "touch_moments.jsonl")
PORT = 9333

ONSET = 0.005         
CONFIRM_N = 1         
INVERTED = False      
TAP_MS = 400          
SHORT_MS = 800        
LIGHT_MAX = 0.035     
SYNC_MS = 500         

CN = {"top": "头顶", "cheek": "脸颊", "chest": "前胸", "hand": "后背"}

latest_action = {"action": "none", "part": "", "time": "", "force": 0, "duration": 0}
# ============================================

def _new_state():
    return {
        'is_touching': False, 'start_time_ms': None, 'last_val': 0,
        'baseline': None, 'peak': 0.0, 'press_start_ms': None,
        'confirm': 0, 'pending_start_ms': None, 'last_end_ms': 0,
    }

sensor_states = {k: _new_state() for k in CN.keys()}
state_lock = threading.Lock()

DUAL = {
    frozenset(['top', 'cheek']):   "捏住了你的头",
    frozenset(['chest', 'hand']):  "抱住了你",
    frozenset(['top', 'chest']):   "把你的头按进怀里",
    frozenset(['top', 'hand']):    "从背后圈住你",
    frozenset(['cheek', 'chest']): "抱着你捧你脸",
    frozenset(['cheek', 'hand']):  "从背后捏住你的脸",
}

class TouchHandler(http.server.BaseHTTPRequestHandler):
    def do_POST(self):
        global latest_action
        if self.path != '/hello':
            self.send_response(404)
            self.end_headers()
            return

        try:
            content_length = int(self.headers['Content-Length'])
            post_data = self.rfile.read(content_length)
            data = json.loads(post_data.decode('utf-8'))
            now = datetime.datetime.now()
            now_ms = int(time.time() * 1000)

            logs_to_write = []
            with state_lock:
                released = []
                for sensor, value in data.items():
                    if sensor not in sensor_states:
                        continue
                    st = sensor_states[sensor]
                    
                    norm_val = value / 4095.0
                    
                    if st['baseline'] is None:
                        st['baseline'] = norm_val
                        print(f"📡 [{sensor}] 初始基线已校准为: {norm_val:.3f}", flush=True)
                        continue 

                    if INVERTED:
                        raw_net = st['baseline'] - norm_val
                    else:
                        raw_net = norm_val - st['baseline']
                    
                    net = raw_net if raw_net >= ONSET else 0.0

                    if net > 0:
                        st['confirm'] += 1
                        if st['confirm'] == 1:
                            st['pending_start_ms'] = now_ms
                        if st['confirm'] >= CONFIRM_N and not st['is_touching']:
                            st['is_touching'] = True
                            st['press_start_ms'] = st['pending_start_ms']
                            st['peak'] = net
                            print(f"    >>> {sensor} 判定为【按下】<<<", flush=True)
                        if st['is_touching'] and net > st['peak']:
                            st['peak'] = net
                        st['last_val'] = net
                    else:
                        st['confirm'] = 0
                        if st['is_touching']:
                            st['is_touching'] = False
                            st['last_end_ms'] = now_ms
                            released.append(sensor)
                            print(f"    <<< {sensor} 判定为【松手】>>>", flush=True)
                        st['last_val'] = 0

                processed = set()
                for s in released:
                    if s in processed:
                        continue
                    ps = sensor_states[s]['press_start_ms']
                    group = [s]
                    for s2 in released:
                        if s2 == s or s2 in processed or s2 in group:
                            continue
                        if abs(sensor_states[s2]['press_start_ms'] - ps) <= SYNC_MS:
                            group.append(s2)
                    for s2 in sensor_states:
                        if s2 == s or s2 in group:
                            continue
                        st2 = sensor_states[s2]
                        if (st2['is_touching'] and st2['press_start_ms'] is not None
                                and abs(st2['press_start_ms'] - ps) <= SYNC_MS):
                            group.append(s2)

                    for g in group:
                        processed.add(g)

                    if len(group) >= 3:
                        group = sorted(group, key=lambda x: sensor_states[x]['peak'], reverse=True)[:2]

                    action_text = None
                    if len(group) == 2:
                        action_text = DUAL.get(frozenset(group))

                    if action_text is None:
                        strongest = max(group, key=lambda x: sensor_states[x]['peak'])
                        sst = sensor_states[strongest]
                        dur_ms = now_ms - sst['press_start_ms']
                        action_text = self.classify_single(strongest, dur_ms, sst['peak'])
                        consume = [strongest]
                    else:
                        consume = list(group)

                    if len(consume) == 1:
                        force_val = sensor_states[consume[0]]['peak']
                        dur_val = now_ms - sensor_states[consume[0]]['press_start_ms']
                    else:
                        force_val = max(sensor_states[g]['peak'] for g in consume)
                        dur_val = max(now_ms - sensor_states[g]['press_start_ms'] for g in consume if sensor_states[g]['press_start_ms'] is not None)

                    if len(consume) > 1:
                        part_cn = "+".join(CN[g] for g in consume)
                    else:
                        part_cn = CN[consume[0]]

                    time_str = now.strftime("%H:%M:%S")
                    
                    latest_action = {
                        "action": action_text, "part": part_cn,
                        "time": time_str, "force": round(force_val, 3),
                        "duration": int(dur_val)
                    }

                    log_entry = {
                        "time": time_str, "part": part_cn,
                        "action": action_text, "force": round(force_val, 3),
                        "duration": int(dur_val),
                    }
                    os.makedirs(LOG_DIR, exist_ok=True)
                    with open(LOG_FILE, "a", encoding="utf-8") as f:
                        f.write(json.dumps(log_entry, ensure_ascii=False) + "\n")
                    
                    print(f"\n🎉 [触发动作] [{time_str}] {part_cn} - {action_text} (力度: {force_val:.3f}, 时长: {dur_val:.0f}ms)\n", flush=True)

                    for g in consume:
                        sensor_states[g]['is_touching'] = False
                        sensor_states[g]['last_end_ms'] = now_ms
                        sensor_states[g]['peak'] = 0.0

            self.send_response(200)
            self.end_headers()
        except Exception as e:
            print(f"\n❌ 处理请求时出错: {e}", flush=True)
            self.send_response(500)
            self.end_headers()

    def do_GET(self):
        global latest_action
        if self.path.startswith('/latest'):
            self.send_response(200)
            self.send_header('Content-Type', 'application/json; charset=utf-8')
            self.end_headers()
            if 'clear=1' in self.path:
                response_data = latest_action.copy()
                latest_action = {"action": "none", "part": "", "time": "", "force": 0, "duration": 0}
            else:
                response_data = latest_action
            self.wfile.write(json.dumps(response_data, ensure_ascii=False).encode('utf-8'))
        else:
            self.send_response(404)
            self.end_headers()

    def classify_single(self, sensor, dur_ms, peak):
        if dur_ms <= TAP_MS:
            return f"戳了戳你的{CN[sensor]}"
        elif dur_ms <= SHORT_MS:
            if peak <= LIGHT_MAX:
                return f"摸了摸你的{CN[sensor]}"
            else:
                return f"打了你一下的{CN[sensor]}"
        else:
            if peak <= LIGHT_MAX:
                return f"揉了揉你的{CN[sensor]}"
            else:
                return f"按住你的{CN[sensor]}没放"

    def log_message(self, format, *args):
        pass

if __name__ == '__main__':
    print(f"🚀 触觉翻译官（电脑端）已启动！端口: {PORT}", flush=True)
    print(f"📁 数据保存至: {LOG_FILE}", flush=True)
    print("="*50, flush=True)
    server = http.server.ThreadingHTTPServer(('0.0.0.0', PORT), TouchHandler)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\n程序已停止。", flush=True)