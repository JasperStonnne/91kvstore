import hashlib,json,os,pwd,signal,socket,struct,subprocess,time
from pathlib import Path

def send_packet(sock,data):
    raw=json.dumps(data,separators=(',',':')).encode();sock.sendall(struct.pack('!I',len(raw))+raw)
def recv_exact(sock,n):
    out=bytearray()
    while len(out)<n:
        chunk=sock.recv(n-len(out))
        if not chunk:raise EOFError('RPC EOF')
        out.extend(chunk)
    return bytes(out)
def recv_packet(sock):
    size=struct.unpack('!I',recv_exact(sock,4))[0]
    if size>4*1024*1024:raise ValueError('oversized RPC')
    return json.loads(recv_exact(sock,size))
def rpc(plan,action,**kwargs):
    with socket.create_connection((plan['replica_host'],plan['agent_port']),timeout=30) as s:
        s.settimeout(60);send_packet(s,dict(token=plan['token'],action=action,**kwargs));r=recv_packet(s)
    if not r.get('ok'):raise RuntimeError(r)
    return r['data']
def proc(pid):
    text=Path(f'/proc/{pid}/stat').read_text();f=text[text.rfind(')')+2:].split()
    return dict(pid=pid,state=f[0],start_tick=int(f[19]),cpu_seconds=(int(f[11])+int(f[12]))/os.sysconf('SC_CLK_TCK'),monotonic=time.monotonic())
def host_cpu():
    row=list(map(int,Path('/proc/stat').read_text().splitlines()[0].split()[1:]))
    return dict(total=sum(row[:8]),idle=row[3]+row[4],steal=row[7])
def cpu_delta(a,b):
    total=b['total']-a['total'];return {'busy_pct':100*(1-(b['idle']-a['idle'])/total),'steal_pct':100*(b['steal']-a['steal'])/total} if total else {}
def child_identity(uid,gid):
    return dict(user=uid,group=gid,extra_groups=os.getgrouplist(pwd.getpwuid(uid).pw_name,gid)) if os.geteuid()==0 else {}
def metadata(repo,binary):
    repo=Path(repo)
    def git(*args):return subprocess.check_output(['git','-C',str(repo),*args],text=True,**child_identity(repo.stat().st_uid,repo.stat().st_gid))
    digest=hashlib.sha256()
    for name in git('ls-files','src','include','Makefile').splitlines():
        p=repo/name
        if p.is_file():digest.update(name.encode()+b'\0'+p.read_bytes())
    return dict(head=git('rev-parse','HEAD').strip(),status=git('status','--short'),source_digest=digest.hexdigest(),submodule=git('submodule','status').strip(),binary_sha256=hashlib.sha256(Path(binary).read_bytes()).hexdigest(),uname=os.uname().release,arch=os.uname().machine,cpus=os.cpu_count(),loadavg=Path('/proc/loadavg').read_text().strip(),meminfo=Path('/proc/meminfo').read_text(),compiler=subprocess.check_output(['gcc','--version'],text=True).splitlines()[0])
def stop_owned(process,identity):
    if process.poll() is None:
        current=proc(process.pid)
        assert current['start_tick']==identity['start_tick']
        process.terminate()
        try:process.wait(timeout=8)
        except subprocess.TimeoutExpired:
            assert proc(process.pid)['start_tick']==identity['start_tick'];process.kill();process.wait(timeout=5)
