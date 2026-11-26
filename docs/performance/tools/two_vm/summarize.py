"""Small pasteable output; full raw rows, commands and profiler logs remain on disk."""
import json
import statistics
import sys
import shlex
from pathlib import Path


def summarize(folder):
    folder=Path(folder)
    rows=json.loads((folder/'results.json').read_text())
    completed=[r for r in rows if r.get('success')]
    lines=['BEGIN_STAGE44B_SUMMARY',f'completed={len(completed)}/12 HEAD=49b8d3d -O2 c=8 P=1 n=100000 HSET key=32B value=128B',
           'ROUND mode round QPS P50ms P99ms CPU_P/C/R% visibility_P50/P99ms verify_P/R']
    def fmt(value):return '-' if value is None or value<0 else f'{value:.3f}'
    for row in rows:
        if not row.get('success'):
            lines.append('FAIL '+row['name']+' '+row.get('error','unknown'));continue
        load=row['load'];vis=row.get('visibility',{})
        verify_p=row['primary_verify']['matched'];verify_r=row.get('replica_verify',{}).get('matched','-')
        lines.append(f"ROUND {row['mode']} {row['round']} {load['qps']:.2f} {load['p50_ms']:.6f} {load['p99_ms']:.6f} "
                     +'/'.join(fmt(x) for x in [load['primary_cpu_pct'],load['capture_cpu_pct'],row.get('replica_cpu_pct')])+' '
                     +'/'.join(fmt(vis.get(x)) for x in ['ack_to_observed_p50_ms','ack_to_observed_p99_ms'])+f' {verify_p}/{verify_r}')
    lines.append('MEDIAN mode QPS P50ms P99ms QPSvariation% CPU_P/C/R% visibility_P50/P99ms')
    for mode in ['none','ebpf-capture-only','socket','ebpf']:
        group=[r for r in completed if r['mode']==mode]
        if len(group)!=3:continue
        median=lambda f:statistics.median(f(r) for r in group)
        q=median(lambda r:r['load']['qps']);qps=[r['load']['qps'] for r in group]
        cpus=[median(lambda r:r['load'][k]) for k in ['primary_cpu_pct','capture_cpu_pct']]
        cpus.append(median(lambda r:r['replica_cpu_pct']) if mode in ['socket','ebpf'] else None)
        vis=[median(lambda r:r['visibility'][k]) if mode in ['socket','ebpf'] else None for k in ['ack_to_observed_p50_ms','ack_to_observed_p99_ms']]
        lines.append(f"MEDIAN {mode} {q:.2f} {median(lambda r:r['load']['p50_ms']):.6f} {median(lambda r:r['load']['p99_ms']):.6f} "
                     +f'{100*(max(qps)-min(qps))/q:.2f} '+'/'.join(map(fmt,cpus))+' '+'/'.join(map(fmt,vis)))
    for row in completed:
        if row.get('mode')=='ebpf' and row.get('round')==3:
            for key in ['idle_capture_before','idle_capture_after','load_capture_after']:
                if key in row:lines.append('CAPTURE '+key+' '+row[key])
        for label,profile in row.get('profiles',{}).items():
            lines.append(f"PROFILE {label} CPU_R={profile['replica_cpu_pct']:.2f}% observation_seconds={profile['observation_seconds']:.2f}")
            for name,cmd in profile['commands'].items():lines.append(f"PROFILE_STATUS {label} {name} exit={cmd['exit']}")
            pidstat=profile['logs'].get('pidstat','')
            lines += [f'PIDSTAT {label} '+line.strip() for line in pidstat.splitlines() if 'Average:' in line]
            stat=profile['logs'].get('perf-stat','')
            lines += [f'PERF_STAT {label} '+line.strip() for line in stat.splitlines() if any(k in line for k in ['task-clock','context-switches','cpu-migrations','Error','Permission'])][:8]
            flat=profile['logs'].get('perf-flat','')
            entries=[line.strip() for line in flat.splitlines() if '%' in line and not line.lstrip().startswith('#')]
            lines += [f'PERF_HOT {label} '+line for line in entries[:12]]
    lines+=['END_STAGE44B_SUMMARY',f'Evidence: {folder}']
    text='\n'.join(lines)+'\n';(folder/'summary.txt').write_text(text);print(text,flush=True)
    commands=[]
    for row in rows:
        commands.append('\n'+row['name'])
        commands.extend(shlex.join(cmd) for cmd in row.get('server_commands',[]))
        if row.get('replica_identity'):commands.append(shlex.join(row['replica_identity']['command']))
        commands.extend(row.get('commands',[]))
    (folder/'commands.txt').write_text('\n'.join(commands)+'\n')


if __name__=='__main__':summarize(sys.argv[1])
