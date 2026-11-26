"""Stage 4.4 fixed -c8 -P1 benchmark; temporary files only, no repository edits."""
import argparse,fcntl,json,os,re,shlex,shutil,statistics,subprocess,sys,time
from pathlib import Path
from common import *

def main():
    os.environ['LC_ALL']='C'
    parser=argparse.ArgumentParser()
    parser.add_argument('--plan',type=Path,required=True)
    parser.add_argument('--modes',nargs='+',choices=['none','ebpf-capture-only','socket','ebpf'],required=True)
    parser.add_argument('--rounds',type=int,default=3)
    parser.add_argument('--label',required=True)
    parser.add_argument('--finish-agent',action='store_true')
    args=parser.parse_args();plan=json.loads(args.plan.read_text());base=args.plan.parent
    assert args.rounds>=3 and plan['requests']==100000 and plan['connections']==8 and plan['pipeline']==1
    assert args.label.replace('-','').isalnum()
    privileged=any('ebpf' in mode for mode in args.modes)
    if privileged and os.geteuid()!=0:raise PermissionError('BPF modes require sudo')
    uid=plan['uid'];gid=plan['gid'];child=child_identity(uid,gid)
    lock=(base/'benchmark.lock').open('a+')
    print('WAIT benchmark lock',flush=True);fcntl.flock(lock,fcntl.LOCK_EX)
    output=base/(args.label+'-'+str(time.time_ns()));output.mkdir()
    results=[];status={'status':'running','modes':args.modes,'output':str(output)}
    def publish():
        (output/'results.json').write_text(json.dumps(results,indent=2))
        (base/(args.label+'-status.json')).write_text(json.dumps(status,indent=2))
    publish()
    for path,digest in plan['expected_files'].items():
        assert hashlib.sha256(Path(path).read_bytes()).hexdigest()==digest,path
    meta=metadata(plan['primary_repo'],plan['primary_binary']);remote=rpc(plan,'meta')
    assert meta['head']==remote['head']==plan['head'] and not meta['status'] and not remote['status']
    assert meta['source_digest']==remote['source_digest']
    assert meta['binary_sha256']==remote['binary_sha256']==plan['binary_sha256']
    (output/'environment.json').write_text(json.dumps({'primary':meta,'replica':remote,'plan':{k:v for k,v in plan.items() if k!='token'},'argv':sys.argv,'client_sha256':hashlib.sha256(Path(plan['client']).read_bytes()).hexdigest()},indent=2))
    def wait_for(predicate,description,timeout=30):
        end=time.monotonic()+timeout
        while time.monotonic()<end:
            if predicate():return
            time.sleep(.02)
        raise RuntimeError(description)
    try:
      for number in range(1,args.rounds+1):
        for mode in args.modes:
          name=f'{mode}-r{number}';root=output/name;root.mkdir()
          if os.geteuid()==0:os.chown(root,uid,gid)
          row={'mode':mode,'round':number,'name':name,'commands':[],'utc_start':time.time(),'success':False}
          primary=capture=log=None;primary_identity=capture_identity=None;rep_started=False
          print('RUN',name,flush=True)
          try:
            config=root/'server.conf'
            incremental='ebpf' if 'ebpf' in mode else 'socket'
            config.write_text(f"role=primary\nbind_ip={plan['primary_host']}\nservice_port={plan['primary_port']}\nrdma_port={plan['rdma_port']}\nreplication_incremental_mode={incremental}\npersistence_mode=none\nallocator=pool\nlog_level=info\n")
            if os.geteuid()==0:os.chown(config,uid,gid)
            log=(root/'primary.log').open('wb')
            row['server_commands']=[['stdbuf','-oL',plan['primary_binary'],str(config)]]
            primary=subprocess.Popen(row['server_commands'][0],cwd=root,stdout=log,stderr=log,stdin=subprocess.DEVNULL,**child)
            primary_identity=proc(primary.pid)
            def primary_ready():
                assert primary.poll() is None,(root/'primary.log').read_text()
                return f"listen port : {plan['primary_port']}" in (root/'primary.log').read_text()
            wait_for(primary_ready,'Primary did not listen')
            if 'ebpf' in mode:
                capture_log=(root/'capture.log').open('wb')
                row['server_commands'].append([plan['capture_binary'],str(primary.pid),plan['bpf_object']])
                capture=subprocess.Popen(row['server_commands'][-1],cwd=root,stdout=capture_log,stderr=capture_log,stdin=subprocess.DEVNULL)
                capture_identity=proc(capture.pid)
                def attached():
                    assert capture.poll() is None,(root/'capture.log').read_text()
                    return 'capture push client ready' in (root/'capture.log').read_text()
                wait_for(attached,'BPF attach did not finish',10)
                row['capture_identity']=capture_identity
            def client(phase,count,host=None,port=None,mutated=False,latencies=False):
                cmd=[plan['client'],'-h',host or plan['primary_host'],'-p',str(port or plan['primary_port']),'-c','8','-P','1','-n',str(count),'-k','32','-d','128','--phase',phase,'--primary-pid',str(primary.pid),'--capture-pid',str(capture.pid if capture else 0)]
                if mutated:cmd+=['--mutated','1','--probes','1000']
                if phase=='visibility':cmd+=['-R',plan['replica_host'],'-q',str(plan['replica_port'])]
                if latencies:cmd+=['-o',str(root/(phase+'-sorted-latency-ns.csv'))]
                gated=phase=='write' and count==100000 and latencies
                if gated:cmd+=['--gate']
                row['commands'].append(shlex.join(cmd))
                if gated:
                    run=subprocess.Popen(cmd,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,**child)
                    try:
                        assert run.stdout.readline().strip()=='READY', 'client failed before READY'
                        row['cpu_before']=rpc(plan,'metrics') if mode in ('socket','ebpf') else None
                        row['host_before']=host_cpu()
                        run.stdin.write('G');run.stdin.flush()
                        raw=run.stdout.readline()
                        row['host_after']=host_cpu()
                        row['cpu_after']=rpc(plan,'metrics') if mode in ('socket','ebpf') else None
                        tail,error=run.communicate(timeout=30)
                        assert run.returncode==0 and not tail, (run.returncode,error,tail)
                    except BaseException:
                        if run.poll() is None:run.kill();run.wait()
                        raise
                else:
                    run=subprocess.run(cmd,capture_output=True,text=True,timeout=300,**child)
                    if run.returncode:raise RuntimeError({'command':cmd,'exit':run.returncode,'stderr':run.stderr})
                    raw=run.stdout
                result=json.loads(raw);(root/(phase+'-'+str(len(row['commands']))+'.json')).write_text(raw)
                return result
            row['preload']=client('write',50000)
            replicated=mode in ('socket','ebpf')
            if replicated:
                row['replica_identity']=rpc(plan,'start',name=args.label+'-'+name);rep_started=True
                wait_for(lambda:'enter ONLINE' in rpc(plan,'log'),'Replica did not reach ONLINE',30)
                row['initial_online_log']=rpc(plan,'log')
                row['snapshot_verify']=client('verify',50000,plan['replica_host'],plan['replica_port'])
            row['warmup']=client('write',1000)
            time.sleep(.3)
            row['load']=client('write',100000,latencies=True)
            assert row['load']['requests']==100000 and row['load']['connections']==8 and row['load']['pipeline']==1
            assert row['load']['uid']==uid, 'Measured client must run as login user'
            before=row['cpu_before'];after=row['cpu_after']
            host_before=row['host_before'];host_after=row['host_after']
            row['primary_vm_cpu']=cpu_delta(host_before,host_after)
            if replicated:
                assert before['server'] and after['server']
                assert before['server']['start_tick']==after['server']['start_tick']
                row['replica_cpu_pct']=100*(after['server']['cpu_seconds']-before['server']['cpu_seconds'])/(after['server']['monotonic']-before['server']['monotonic'])
                row['replica_cpu_window_seconds']=after['server']['monotonic']-before['server']['monotonic']
                row['replica_vm_cpu']=cpu_delta(before['host_cpu'],after['host_cpu'])
                row['visibility']=client('visibility',1000,latencies=True)
            row['primary_verify']=client('verify',100000,mutated=replicated)
            if replicated:row['replica_verify']=client('verify',100000,plan['replica_host'],plan['replica_port'],True)
            def capture_stats():
                matches=[line for line in (root/'capture.log').read_text(errors='replace').splitlines() if line.startswith('capture TCP stats ')]
                return matches[-1] if matches else ''
            if capture:
                assert capture.poll() is None, 'Collector exited during test'
                expected=(151000+(1000 if replicated else 0))*189
                wait_for(lambda:f'cache_end={expected} ' in capture_stats(),'Collector did not consume every committed byte',8)
                row['capture_stats_after_formal']=capture_stats()
                assert ' valid=1 ' in row['capture_stats_after_formal'], 'Capture cache invalidated'
            if mode=='ebpf' and number==args.rounds:
                # Observations run after all timed rounds; excluded from table CPU/QPS.
                row['profiles']={}
                wait_for(lambda:all(f'{key}={expected} ' in capture_stats() for key in ['sent','applied']),'Replica did not drain before idle profile',8)
                row['idle_capture_before']=capture_stats()
                rpc(plan,'profile_start',label='idle')
                time.sleep(9)
                row['profiles']['idle']=rpc(plan,'profile_finish')
                row['idle_capture_after']=capture_stats()
                rpc(plan,'profile_start',label='load')
                row['profile_load']=client('pressure',100000)
                row['profiles']['load']=rpc(plan,'profile_finish')
                row['post_profile_primary_verify']=client('verify',100000)
                row['post_profile_replica_verify']=client('verify',100000,plan['replica_host'],plan['replica_port'])
                row['load_capture_after']=capture_stats()
            if replicated:
                final_log=rpc(plan,'log')
                row['full_sync_count']=final_log.count('enter FULL_SYNC')
                row['online_count']=final_log.count('enter ONLINE')
                assert row['full_sync_count']==row['online_count']==1, 'Unexpected resync or missing ONLINE'
            row['primary_identity']=primary_identity;row['success']=True
          except BaseException as e:
            row['error']=repr(e)
            raise
          finally:
            if rep_started:
                try:
                    stopped=rpc(plan,'stop');(root/'replica.log').write_text(stopped.get('log',''));row['replica_cleanup']=stopped
                except Exception as e:row['replica_cleanup_error']=repr(e)
            if capture:
                stop_owned(capture,capture_identity);capture_log.close()
            if primary:stop_owned(primary,primary_identity)
            if log:log.close()
            for temporary in list(root.glob('*.tmp*'))+list(root.glob('snapshot.bin')):
                if temporary.is_file():temporary.unlink()
            row['utc_end']=time.time();row['remaining_tmp']=[p.name for p in root.glob('*.tmp*')]
            if row.get('replica_cleanup'):
                row['replica_cleanup']['log']='see replica.log'
            results.append(row);publish()
          print('PASS',name,'qps',round(row['load']['qps'],2),flush=True)
      status['status']='passed'
    except BaseException as e:
      status['status']='failed';status['error']=repr(e);raise
    finally:
      publish()
      if args.finish_agent:
        try:rpc(plan,'finish')
        except Exception as e:status['agent_finish_error']=repr(e);publish()
      if os.geteuid()==0:
        for folder,dirs,files in os.walk(output):
            os.chown(folder,uid,gid)
            for name in files:os.chown(Path(folder)/name,uid,gid)
        os.chown(base/(args.label+'-status.json'),uid,gid)
      print('Evidence:',output,flush=True)
      if status['status']=='passed':
          from summarize import summarize
          summarize(output)
if __name__=='__main__':main()
