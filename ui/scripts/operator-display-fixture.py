#!/usr/bin/env python3
"""Serve ui/dist with deterministic G0759-shaped display messages, on localhost only.

Build the UI first, then run this script and open the printed URL. This has no
LinuxCNC connection. Power and homing commands change only the displayed fixture;
other commands are rejected with a long layout-test message. The touch URL flag
emulates touch pointer events; it does not test the native GNOME keyboard.
The underlying sample status is illustrative, not a physical machine setup.
"""
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlsplit, parse_qs
import copy, json
root = Path(__file__).resolve().parents[2]

class Handler(SimpleHTTPRequestHandler):

    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=str(root / 'ui/dist'), **kwargs)

    def do_GET(self):
        parsed = urlsplit(self.path)
        if parsed.path != '/fixture':
            return super().do_GET()
        fixtures = json.loads((root / 'protocol/fixtures/messages.json').read_text())
        hello = copy.deepcopy(next((x for x in fixtures if x['type'] == 'hello')))
        hello.update(machine_name='G0759 display fixture', simulated=True, jog_increments=[5, 1, 0.5, 0.1, 0.05, 0.01, 0.005], max_jog_velocity=1500, axis_max_jog_velocity={'x': 1500, 'y': 1500, 'z': 750}, homing_required=False)
        status = copy.deepcopy(next((x for x in fixtures if x['type'] == 'status')))
        status.update(machine='on', mode='manual', interp='idle', wait={'kind': 'none'})
        for axis in status['axes']:
            axis.update(homed=True, homing=False, work=-123.456 if axis['axis'] == 'x' else -9.876)
        status['motion'].update(velocity=0, in_position=True)
        status['spindle'].update(direction='stopped', commanded_rpm=0, actual_rpm=0, at_speed=False)
        status['program'].update(file=None, revision=0, current_line=0, motion_line=0)
        table = copy.deepcopy(next((x for x in fixtures if x['type'] == 'tool_table')))
        settings = copy.deepcopy(next((x for x in fixtures if x['type'] == 'tool_models' and x['availability'] == 'available')))
        script = "<script>\nconst fixtureMessages=PAYLOAD;let started=false; const emit=m=>window.coffeecncReceive(structuredClone(m));\n// UI-only emulation: real touch behavior and the GNOME OSK require native verification.\nif(new URLSearchParams(location.search).has('touch')) ['pointerdown','pointerup'].forEach(type=>document.addEventListener(type,e=>{Object.defineProperty(e,'pointerType',{value:'touch'});},true));\nwindow.webkit={messageHandlers:{coffeecnc:{postMessage(body){\n const r=JSON.parse(body),s=fixtureMessages[1],accept=()=>emit({type:'result',id:r.id,outcome:'accepted',reason:''});\n if(r.type==='ready'&&!started){started=true;fixtureMessages.forEach(emit);setInterval(()=>{s.seq++;emit(s);},50);accept();return;}\n if(r.type==='set_machine'){s.machine=r.state;accept();emit(s);return;}\n if(r.type==='home'){s.axes.forEach(a=>{if(a.axis===r.axis||r.axis==='all')a.homing=true;});accept();emit(s);setTimeout(()=>{s.axes.forEach(a=>a.homing=false);emit(s);},3000);return;}\n emit({type:'result',id:r.id,outcome:'rejected',reason:'Display-only fixture: no controller connected. This intentionally long message checks that operator errors stay visible in reserved space without moving the jog, MDI or execution controls.'});\n}}}};\n</script>".replace('PAYLOAD', json.dumps([hello, status, table, settings]))
        data = (root / 'ui/dist/index.html').read_text().replace('</head>', script + '</head>').encode()
        self.send_response(200)
        self.send_header('Content-Type', 'text/html; charset=utf-8')
        self.send_header('Content-Length', str(len(data)))
        self.send_header('Cache-Control', 'no-store')
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, *args):
        pass
if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser(description='CoffeeCNC browser-only operator display fixture')
    parser.add_argument('--port', type=int, default=5184)
    args = parser.parse_args()
    print(f'UI-only fixture http://127.0.0.1:{args.port}/fixture?tab=manual&touch=1&theme=light', flush=True)
    ThreadingHTTPServer(('127.0.0.1', args.port), Handler).serve_forever()
