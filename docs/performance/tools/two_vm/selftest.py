"""Loopback mock protocol test of the temporary driver, not of kvstore/replication."""
import json
import socketserver
import subprocess
import sys
import threading


def check(binary):
    values={};lock=threading.Lock()
    class Server(socketserver.ThreadingTCPServer):
        allow_reuse_address=True
        daemon_threads=True
    class Handler(socketserver.StreamRequestHandler):
        def handle(self):
            while True:
                line=self.rfile.readline()
                if not line:return
                assert line[:1]==b'*',line
                args=[]
                for _ in range(int(line[1:])):
                    size=int(self.rfile.readline()[1:]);args.append(self.rfile.read(size))
                    assert self.rfile.read(2)==b'\r\n'
                assert len(args[1])==32
                with lock:
                    if args[0] in (b'HSET',b'HMOD'):
                        assert len(args)==3 and len(args[2])==128
                        values[args[1]]=args[2];reply=b'+OK\r\n'
                    else:
                        assert args[0]==b'HGET'
                        value=values.get(args[1])
                        reply=b'$-1\r\n' if value is None else b'$128\r\n'+value+b'\r\n'
                self.wfile.write(reply);self.wfile.flush()
    server=Server(('127.0.0.1',0),Handler)
    thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
    port=server.server_address[1]
    base=[str(binary),'-h','127.0.0.1','-p',str(port),'-c','8','-P','1','-n','1000','-k','32','-d','128']
    try:
        cases=[('write',[]),('verify',[]),('visibility',['-R','127.0.0.1','-q',str(port)]),
               ('verify',['--mutated','1']),('pressure',['--seconds','1']),('verify',[])]
        for phase,extra in cases:
            run=subprocess.run(base+['--phase',phase]+extra,capture_output=True,text=True,timeout=20)
            assert run.returncode==0,(phase,run.stderr)
            result=json.loads(run.stdout)
            if phase=='write':assert result['connections']==8 and result['requests']==1000
            if phase=='visibility':assert result['probe_connections']==8 and result['samples']==1000
            if phase=='verify':assert result['matched']==1000
        run=subprocess.run(base+['--phase','write','--gate'],input='G',capture_output=True,text=True,timeout=20)
        assert run.returncode==0 and run.stdout.splitlines()[0]=='READY',(run.stdout,run.stderr)
        assert json.loads(run.stdout.splitlines()[1])['requests']==1000
        print('PASS temporary driver: c8/P1 write, gate, visibility, pressure and full verification')
    finally:server.shutdown();server.server_close()


if __name__=='__main__':check(sys.argv[1])
