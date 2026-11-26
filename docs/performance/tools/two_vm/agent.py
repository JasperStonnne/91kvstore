"""Ephemeral authenticated benchmark agent; fixed Replica actions, no shell/exec RPC."""
import json,os,secrets,shutil,socket,subprocess,sys,time
from pathlib import Path
from common import *
os.environ['LC_ALL']='C'
base=Path(__file__).resolve().parent
plan=json.loads((base/'agent-plan.json').read_text());repo=Path(plan['repo']);binary=repo/'bin/kvstore'
owner=repo.stat();child=child_identity(owner.st_uid,owner.st_gid)
profiles={};profile_identity=None;profile_before=None;profile_dir=None
assert metadata(repo,binary)['head']==plan['head']
assert hashlib.sha256(binary.read_bytes()).hexdigest()==plan['binary_sha256']
listener=socket.socket();listener.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1);listener.bind((plan['host'],plan['agent_port']));listener.listen(4);listener.settimeout(1)
(base/'ready.json').write_text(json.dumps({'port':listener.getsockname()[1],'pid':os.getpid(),'identity':proc(os.getpid())}))
server=None;identity=None;directory=None;logfile=None;last=time.monotonic();finished=False
print('READY 136 agent; waiting for 135 controller; Ctrl-C cleans owned processes',flush=True)

def profile_start(label):
    global profile_identity,profile_before,profile_dir
    assert label in ('idle','load') and not profiles and server and server.poll() is None
    profile_identity=proc(server.pid);profile_before=proc(server.pid)
    profile_dir=base/'profiles'/label;profile_dir.mkdir(parents=True,exist_ok=False)
    os.chown(profile_dir.parent,owner.st_uid,owner.st_gid)
    commands={
        'pidstat':['pidstat','-u','-w','-p',str(server.pid),'1','8'],
        'perf-record':['perf','record','-e','cpu-clock','-F','199','-g','--call-graph','dwarf,4096','-p',str(server.pid),'-o',str(profile_dir/'perf.data'),'--','sleep','8'],
        'perf-stat':['perf','stat','-x,','-e','task-clock,context-switches,cpu-migrations','-p',str(server.pid),'--','sleep','8'],
    }
    for name,cmd in commands.items():
        log=(profile_dir/(name+'.txt')).open('wb')
        try:
            process=subprocess.Popen(cmd,stdout=log,stderr=log,stdin=subprocess.DEVNULL,start_new_session=True)
            profiles[name]=(process,proc(process.pid),log,cmd)
        except FileNotFoundError as error:
            log.write(str(error).encode());log.close()
    (profile_dir/'commands.json').write_text(json.dumps(commands,indent=2))
    return {'pid':server.pid,'label':label}

def profile_finish():
    assert profile_dir and profile_identity
    result={'label':profile_dir.name,'commands':{},'logs':{}}
    for name,(process,ident,log,cmd) in list(profiles.items()):
        try:process.wait(timeout=12)
        except subprocess.TimeoutExpired:
            if process.poll() is None and proc(process.pid)['start_tick']==ident['start_tick']:
                os.killpg(process.pid,signal.SIGTERM)
                try:process.wait(timeout=3)
                except subprocess.TimeoutExpired:os.killpg(process.pid,signal.SIGKILL);process.wait()
        log.close();result['commands'][name]={'argv':cmd,'exit':process.returncode}
        result['logs'][name]=(profile_dir/(name+'.txt')).read_text(errors='replace')
        del profiles[name]
    after=proc(server.pid);assert after['start_tick']==profile_identity['start_tick']
    duration=after['monotonic']-profile_before['monotonic']
    if (profile_dir/'perf.data').exists():
        for name,extra in [('perf-flat',['--no-children','--percent-limit','0.5','--sort','comm,dso,symbol']),('perf-callgraph',['--children','--percent-limit','1'])]:
            cmd=['perf','report','--stdio','-i',str(profile_dir/'perf.data'),*extra]
            try:
                run=subprocess.run(cmd,capture_output=True,text=True,timeout=25)
                text=run.stdout+run.stderr
                (profile_dir/(name+'.txt')).write_text(text)
                result['commands'][name]={'argv':cmd,'exit':run.returncode};result['logs'][name]=text[:500000]
            except Exception as error:result['logs'][name]=repr(error)
    result['observation_seconds']=duration
    result['replica_cpu_pct']=100*(after['cpu_seconds']-profile_before['cpu_seconds'])/duration
    (profile_dir/'summary.json').write_text(json.dumps(result,indent=2))
    for path in profile_dir.iterdir():os.chown(path,owner.st_uid,owner.st_gid)
    os.chown(profile_dir,owner.st_uid,owner.st_gid)
    return result

def stop():
    global server,identity,directory,logfile
    if not server:return {}
    if profiles:
        try:profile_finish()
        except Exception as error:print('Profiler cleanup:',repr(error),flush=True)
    stop_owned(server,identity)
    logfile.close()
    result={'log':(directory/'server.log').read_text(errors='replace'),'exit':server.returncode,'remaining_tmp':[p.name for p in directory.glob('*.tmp*')]}
    shutil.rmtree(directory);server=identity=directory=logfile=None
    return result
try:
 while not finished:
    now=time.monotonic()
    if server and now-last>300:stop()
    if now-last>3600:break
    try:connection,peer=listener.accept()
    except socket.timeout:continue
    if peer[0] not in (plan['host'],plan['primary_host']):
        connection.close();continue
    with connection:
      connection.settimeout(3)
      try:
        request=recv_packet(connection)
        if not secrets.compare_digest(request.get('token',''),plan['token']):raise PermissionError('invalid token')
        last=time.monotonic();action=request['action']
        if action=='meta':result=metadata(repo,binary)
        elif action=='start':
            assert server is None
            name=request['name'];assert name.replace('-','').replace('_','').isalnum()
            directory=base/('runtime-'+name);directory.mkdir()
            cfg=directory/'server.conf'
            cfg.write_text(f"role=replica\nbind_ip={plan['host']}\nservice_port={plan['service_port']}\nprimary_host={plan['primary_host']}\nprimary_port={plan['primary_port']}\npersistence_mode=none\nallocator=pool\nlog_level=info\n")
            os.chown(cfg,owner.st_uid,owner.st_gid)
            logfile=(directory/'server.log').open('wb')
            os.chown(directory,owner.st_uid,owner.st_gid)
            server=subprocess.Popen(['stdbuf','-oL',str(binary),str(cfg)],cwd=directory,stdout=logfile,stderr=logfile,stdin=subprocess.DEVNULL,**child)
            identity=proc(server.pid);result=dict(identity,command=['stdbuf','-oL',str(binary),str(cfg)])
        elif action=='metrics':
            result={'server':proc(server.pid) if server and server.poll() is None else None,'host_cpu':host_cpu()}
        elif action=='log':result=(directory/'server.log').read_text(errors='replace') if directory else ''
        elif action=='profile_start':result=profile_start(request['label'])
        elif action=='profile_finish':result=profile_finish()
        elif action=='stop':result=stop()
        elif action=='finish':result=stop();finished=True
        else:raise ValueError('unsupported action')
        send_packet(connection,{'ok':True,'data':result})
      except Exception as e:
        try:send_packet(connection,{'ok':False,'error':repr(e)})
        except OSError:pass
finally:
 stop();listener.close()
 (base/'agent-finished.json').write_text(json.dumps({'pid':os.getpid(),'finished':True}))
 if os.geteuid()==0:
    for path in [base/'ready.json',base/'agent-finished.json']:os.chown(path,owner.st_uid,owner.st_gid)
 print('DONE 136: owned Replica/profilers stopped; evidence kept under',base,flush=True)
