#!/usr/bin/env python3
"""Offline figures from committed measurements only; never runs a server or test.
Requires an already available matplotlib. Does not install dependencies.
"""
import argparse
import json
import os
from pathlib import Path
import statistics as stats
import tempfile
import xml.etree.ElementTree as ET


def plot_formal(data, target, preview):
    """Render measured before/after data; retained groups keep their source identity."""
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    import numpy as np
    from statistics import median
    target.mkdir(parents=True, exist_ok=True)
    if preview:
        preview.mkdir(parents=True, exist_ok=True)
    plt.rcParams.update({'font.family':'DejaVu Sans','font.size':10,
                         'axes.spines.top':False,'axes.spines.right':False,'svg.fonttype':'none',
                         'svg.hashsalt':'91kv-20261119-retune'})
    old=data
    new=data['retune']
    colors=['#64748b','#2563eb','#e68619']
    def save(fig,name,title,note):
        fig.suptitle(title,fontsize=15,fontweight='bold',x=.07,ha='left')
        fig.text(.07,.025,note,fontsize=9,va='bottom')
        fig.tight_layout(rect=(.015,.12,.99,.92))
        fig.savefig(target/(name+'.svg'),metadata={'Date':None,'Title':title,'Description':note})
        dest = target/(name+'.svg')
        dest.write_text('\n'.join(line.rstrip() for line in dest.read_text().splitlines()) + '\n')
        ET.parse(target/(name+'.svg'))
        if preview: fig.savefig(preview/(name+'.png'),dpi=140)
        plt.close(fig)
    def bars(ax,groups,labels,legends,unit,log=False):
        x=np.arange(len(labels));width=.78/len(groups)
        for i,(group,label) in enumerate(zip(groups,legends)):
            pos=x+(i-(len(groups)-1)/2)*width
            med=[median(g) if g else float('nan') for g in group]
            ax.bar(pos,med,width*.9,label=label,color=colors[i])
            for p,vals,m in zip(pos,group,med):
                if not vals:
                    ax.text(p,.035,'FAIL',transform=ax.get_xaxis_transform(),ha='center',color='#b91c1c',fontsize=9)
                else:
                    ax.scatter([p]*len(vals),vals,s=13,c='#0f172a',zorder=3)
                    ax.annotate(f'{m:,.3f}' if 'Gbit' in unit else f'{m:,.1f}', (p,m), xytext=(0,4), textcoords='offset points', ha='center', fontsize=7.5, rotation=90 if log else 0)
        ax.set_xticks(x,labels);ax.set_ylabel(unit);ax.set_axisbelow(True);ax.grid(axis='y',alpha=.2)
        if log: ax.set_yscale('log');ax.set_ylim(100,10000000)
        else: ax.set_ylim(bottom=0);ax.margins(y=.18)
        ax.legend(frameon=False,fontsize=9,ncol=len(groups) if log else 1,loc='upper left' if log else 'best')
    # Allocator results were measured before this network fix and are intentionally reused.
    modes=['system','pool','jemalloc'];rows=old['memory']['results']
    fig,ax=plt.subplots(figsize=(11,5))
    bars(ax,[[[r['states'][s]['VmRSS_KiB']/1024 for r in rows if r['mode']==m] for m in modes]
             for s in ('start','peak','end')],modes,['Start','Full + verified','Deleted + verified'],'VmRSS (MiB)')
    save(fig,'20261119-allocator-rss','Million-key allocator RSS: three backends',
         'Retained 0e25dd7 measurements; no rerun. Bars: medians; dots: 3 rounds; five /proc samples per state.\n4 million verified operations per round. Full state RSS is not VmHWM; retained RSS alone does not prove a leak.')
    # Pipeline throughput includes both Redis references and all raw rounds.
    depths=[10,20,40,80,160];fig,axes=plt.subplots(1,2,figsize=(14,5.5))
    groups=[]
    for source,label in ((old,'kv-none'),(new,'kv-none'),(new,'redis-none')):
        groups.append([[r['qps'] for r in source['qps']['results'] if r['label']==label and r['pipeline']==p] for p in depths])
    bars(axes[0],groups,[str(p) for p in depths],['kv before','kv after','Redis after'],'Completed requests/s (log)',True)
    axes[0].set_xlabel('Pipeline depth P')
    axes[1].plot(depths,[median(g) for g in groups[1]],'o-',label='kv after',color=colors[1])
    axes[1].plot(depths,[median(g) for g in groups[2]],'o-',label='Redis after',color=colors[2])
    oldredis=[[r['qps'] for r in old['qps']['results'] if r['label']=='redis-none' and r['pipeline']==p] for p in depths]
    axes[1].plot(depths,[median(g) for g in oldredis],'x--',label='Redis before',color=colors[0])
    axes[1].set(xlabel='Pipeline depth P',ylabel='Completed requests/s (linear)');axes[1].grid(alpha=.2);axes[1].legend(frameon=False)
    save(fig,'20261119-pipeline','Pipeline: before / after and Redis reference',
         'c1; 100,000 measured requests/round, 3 rounds; 32 B hot key / 128 B value; pool; no persistence.\nkv HSET vs Redis SET. Very short fast samples limit precision; throughput is not per-command execution latency.')
    # Stack paired components from the same rounds, then take component medians.
    rows=old['save']['results'];intervals=[1000000,100000,10000,1000]
    rest=[];sav=[];total=[]
    for n in intervals:
        group=[r for r in rows if r['save_every']==n]
        rest.append(median([r['seconds']-r['save_seconds'] for r in group]))
        sav.append(median([r['save_seconds'] for r in group]))
        total.append(median([r['seconds'] for r in group]))
    fig,ax=plt.subplots(figsize=(11,5));x=np.arange(4)
    ax.bar(x,rest,label='Other client/write time',color=colors[0]);ax.bar(x,sav,bottom=rest,label='Synchronous SAVE time',color=colors[1])
    for i,(a,b,t) in enumerate(zip(rest,sav,total)):ax.text(i,a+b+20,f'{t:,.2f}s total median',ha='center',fontsize=9)
    ax.set_xticks(x,['1,000,000\n3 rounds','100,000\n3 rounds','10,000\n3 rounds','1,000\n1 round'])
    ax.set(xlabel='Writes between SAVE calls',ylabel='Elapsed time (s)');ax.legend(frameon=False);ax.margins(y=.18);ax.grid(axis='y',alpha=.2)
    save(fig,'20261119-save-frequency','SAVE frequency: elapsed time components',
         'Retained 0e25dd7 results; 1 million distinct Hash keys. Recovery verification excluded from timing.\nStacked values are medians of paired per-round components; their sum may differ from the total median shown.')
    # Treat failed old eBPF trials as failures, never as zero replicated throughput.
    modes=['none','socket','ebpf'];fig,axes=plt.subplots(1,2,figsize=(13,5.5))
    groups=[[[r['qps'] for r in src['replication']['results'] if r['mode']==m and r.get('success',True)] for m in modes] for src in (old,new)]
    bars(axes[0],groups,modes,['Before','After'],'Primary completed writes/s')
    vis=[[[r['visibility']['p99'] for r in src['replication']['results'] if r['mode']==m and r.get('success',True)] for m in ('socket','ebpf')] for src in (old,new)]
    bars(axes[1],vis,['socket','ebpf'],['Before','After'],'Replica observable P99 (ms)')
    save(fig,'20261119-replication','Two VMs: real-time replication before / after',
         '100k measured writes, c1/P1; three rounds/mode. Throughput accepted only with final marker + ONLINE checks.\nVisibility: separate 1,000 serial probes/round, 1ms polling. Old eBPF failed 3/3; no valid replicated throughput.')
    # Transfer remains the previously measured software SIW snapshot transfer.
    rows=old['transfer']['results'];directions=[135,136];fig,ax=plt.subplots(figsize=(11,5))
    bars(ax,[[[r['gbps'] for r in rows if r['mode']==m and r['sender']==d] for d in directions] for m in ('sendfile','rdma','iperf3')],
         ['135 -> 136','136 -> 135'],['sendfile','RDMA (SIW)','iperf3'],'Throughput (Gbit/s)')
    save(fig,'20261119-transfer','Two VMs: snapshot transfer and TCP reference',
         'Retained 0e25dd7 results; 3 rounds/direction. Native transfers: 168,000,052 B, CRC verified, RAM receive.\n1 MiB chunks, window 1; transfer-only timing excludes setup + CRC. iperf3: one flow, 30s. Software SIW, no hardware RDMA claim.')
    print('Generated five 2026-11-19 SVG figures from measured JSON only.')



def plot_direct(data, target, preview):
    """Compare one direct eBPF run with retained historical medians; no benchmarking."""
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    assert data['measurement_passed'] and data['cleanup_verified']
    historical = data['historical']['comparisons']
    direct = data['direct']
    target.mkdir(parents=True, exist_ok=True)
    plt.rcParams.update({'font.family':'DejaVu Sans', 'font.size':10,
        'axes.spines.top':False, 'axes.spines.right':False, 'svg.fonttype':'none',
        'svg.hashsalt':'91kv-20261120-direct'})
    colors=['#64748b','#2563eb','#e68619']
    labels=['socket\n3 historical rounds','eBPF via .sock\n3 historical rounds','eBPF direct TCP\n1 new round']
    groups=[[v['qps'] for v in h['round_values']] for h in historical]+[[direct['qps']]]
    fig, axes=plt.subplots(1,2,figsize=(13.2,5.4))
    for i, values in enumerate(groups):
        middle=stats.median(values)
        axes[0].bar(i,middle,color=colors[i],width=.62)
        axes[0].scatter([i+(j-(len(values)-1)/2)*.07 for j in range(len(values))],values,
                        color='#111827',s=24,marker='D' if i==2 else 'o',zorder=3)
        axes[0].text(i,middle+800,f'{middle:,.2f}',ha='center',fontsize=10)
    axes[0].set(xticks=range(3),xticklabels=labels,ylabel='Primary successful replies/s',ylim=(0,35000))
    axes[0].set_title('Throughput: 100,000 measured HSET requests')
    for mode in range(3):
        values=[]
        for field in ('p50','p99'):
            values.append([v['visibility_'+field+'_ms'] for v in historical[mode]['round_values']]
                          if mode<2 else [direct['visibility'][field]])
        positions=[x+(mode-1)*.25 for x in range(2)]
        mids=[stats.median(v) for v in values]
        axes[1].bar(positions,mids,width=.22,color=colors[mode],label=labels[mode].replace('\n',' / '))
        for x, vals, middle in zip(positions,values,mids):
            axes[1].scatter([x+(j-(len(vals)-1)/2)*.035 for j in range(len(vals))],vals,
                            color='#111827',s=17,marker='D' if mode==2 else 'o',zorder=3)
            axes[1].text(x,middle+.12,f'{middle:.3f}',ha='center',fontsize=9)
    axes[1].set(xticks=range(2),xticklabels=['P50','P99'],ylabel='Submit-to-visible latency (ms)',ylim=(0,7.2))
    axes[1].set_title('Separate 1,000 serial visibility probes per round')
    axes[1].legend(frameon=False,fontsize=8,loc='upper left')
    for ax in axes:
        ax.set_axisbelow(True);ax.grid(axis='y',alpha=.2)
    title='Direct eBPF: one new round vs historical references'
    fig.suptitle(title,fontsize=16,fontweight='bold',x=.06,ha='left')
    note=('c1 / P1; 32 B hot key / 128 B value; pool; no persistence; -O2 -g. Bars: medians; dots: raw rounds.\n'
          '2026-11-19 references retained; direct run: 2026-11-20. No concurrent control rerun or causal claim.\n'
          'Visibility probes are outside throughput timing and include write/read RTT plus 1 ms polling; not one-way latency.')
    fig.text(.06,.025,note,fontsize=9,va='bottom')
    fig.tight_layout(rect=(.01,.18,.99,.91))
    dest=target/'20261120-direct-ebpf.svg'
    fig.savefig(dest,metadata={'Date':None,'Title':title,'Description':note})
    dest.write_text('\n'.join(line.rstrip() for line in dest.read_text().splitlines())+'\n')
    ET.parse(dest)
    if preview:
        preview.mkdir(parents=True,exist_ok=True)
        fig.savefig(preview/'20261120-direct-ebpf.png',dpi=150)
    plt.close(fig)
    print('Generated direct eBPF comparison from stored JSON only; no service or test executed.')

def plot_final(data, target, preview):
    """Stage 5: no benchmarks; keep each version, client and round group separate."""
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    import numpy as np
    from statistics import median
    target.mkdir(parents=True, exist_ok=True)
    if preview:preview.mkdir(parents=True,exist_ok=True)
    plt.rcParams.update({'font.family':'DejaVu Sans','font.size':10,
        'axes.spines.top':False,'axes.spines.right':False,'svg.fonttype':'none',
        'svg.hashsalt':'91kv-stage5-final','figure.facecolor':'white'})
    blue,orange,gray,green='#2563eb','#d97706','#64748b','#059669'
    def save(fig,name,title,note):
        fig.suptitle(title,x=.065,ha='left',fontweight='bold',fontsize=15)
        fig.text(.065,.025,note,va='bottom',fontsize=8.8,linespacing=1.4)
        fig.tight_layout(rect=(.015,.16,.985,.90))
        path=target/('20261126-'+name+'.svg')
        fig.savefig(path,metadata={'Date':None,'Title':title,'Description':note})
        path.write_text('\n'.join(line.rstrip() for line in path.read_text().splitlines())+'\n')
        ET.parse(path)
        if preview:fig.savefig(preview/('20261126-'+name+'.png'),dpi=125)
        plt.close(fig)
    def grid(ax,ylabel):
        ax.set_ylabel(ylabel);ax.set_axisbelow(True);ax.grid(axis='y',alpha=.18)
    def var(values):return 100*(max(values)-min(values))/median(values)
    def measured(ax,values,labels,colors=None,log=False):
        mids=[median(v) for v in values]
        ax.bar(range(len(values)),mids,color=colors or blue,width=.66)
        for i,(v,m) in enumerate(zip(values,mids)):
            ax.scatter([i+(k-(len(v)-1)/2)*.055 for k in range(len(v))],v,s=20,c='#111827',zorder=3)
            if len(v)>1:ax.errorbar(i,m,yerr=[[m-min(v)],[max(v)-m]],fmt='none',ecolor='#111827',capsize=4)
            ax.annotate(f'{m:,.0f}\n'+(f'v={var(v):.2f}%' if len(v)>1 else 'n=1; v=N/A'),(i,max(v)),xytext=(0,7),textcoords='offset points',ha='center',fontsize=8)
        ax.set_xticks(range(len(labels)),labels)
        if log:ax.set_yscale('log');ax.set_ylim(min(mids)*.65,max(mids)*2)
        else:ax.set_ylim(0,max(max(v) for v in values)*1.25)
    # Stage-1 retained AOF samples: do not relabel them as final-code reruns.
    labels=['ECHO','kv-none','kv-aof\nbefore','kv-aof\nlinked','Redis\nnone','Redis AOF\nalways']
    modes=['echo','kv-none','kv-aof-before','kv-aof','redis-none','redis-aof']
    values=[[r['qps'] for r in data['single']['rows'] if r['server']==m] for m in modes]
    fig,ax=plt.subplots(figsize=(11,5.8));measured(ax,values,labels,[gray,blue,gray,blue,orange,orange],True);grid(ax,'QPS (log scale)')
    save(fig,'single-aof','P1 network / persistence references: retained Stage 1',
         '3 rounds/case; c1/P1/n100000; hotkey + 128 ASCII x. Dots=rounds, whiskers=min/max; v=QPS range/median.\nLinked WRITE + DATASYNC waits for both CQEs; Redis appendfsync=always. These samples were NOT rerun at 49b8d3d.')
    depths=[10,20,40,80,160];rows=data['pipeline']['rows'];x=np.arange(5)
    fig,axes=plt.subplots(1,2,figsize=(13,5.8))
    for mode,color,label in [('kv-none',blue,'kv-none'),('redis-none',orange,'Redis 7.0.15')]:
        groups=[[r['qps'] for r in rows if r['program']==mode and r['pipeline']==p] for p in depths]
        mids=[median(v) for v in groups];offset=-.17 if mode=='kv-none' else .17
        pos=x+offset;axes[0].bar(pos,np.array(mids)/1e6,width=.3,color=color,label=label)
        for i,(p,v,m) in enumerate(zip(pos,groups,mids)):
            axes[0].scatter([p]*3,np.array(v)/1e6,color='#111827',s=14,zorder=3)
            axes[0].errorbar(p,m/1e6,yerr=[[(m-min(v))/1e6],[(max(v)-m)/1e6]],fmt='none',ecolor='#111827',capsize=3)
            axes[0].text(p,max(v)/1e6+.06,f'{m/1e6:.2f}\n{var(v):.2f}%',ha='center',fontsize=7.5)
        for metric,style in [('p50_ms','--'),('p99_ms','-')]:
            vals=[median([r[metric] for r in rows if r['program']==mode and r['pipeline']==p]) for p in depths]
            axes[1].plot(x,vals,'o'+style,color=color,label=label+' '+metric[:3].upper())
    for ax in axes:ax.set_xticks(x,[str(p) for p in depths]);ax.set_xlabel('Pipeline depth P')
    grid(axes[0],'Million requests/s');grid(axes[1],'redis-benchmark native batch latency (ms)')
    axes[0].set_ylim(0,3.6);axes[0].legend(frameon=False);axes[1].legend(frameon=False,fontsize=8)
    save(fig,'pipeline','49b8d3d: alternating Pipeline controls (30 runs)',
         'c1; 100000 requests/run; SET hotkey + 128 ASCII x; no persistence. Each server/P has 3 rounds.\nThroughput labels: median and QPS variation; dots=rounds, whiskers=min/max. P160: kv +11.76%; latency is not per-command CPU time.')
    fig,ax=plt.subplots(figsize=(11,5.8))
    for stage,color,label in [('O2',gray,'O2 baseline (1 run/P)'),('cursor64k',orange,'cursor + 64KiB (1 run/P)'),('tokens',green,'4KiB tokens stack (1 run/P)')]:
        vals=[next(r['qps'] for r in data['pipeline_stages']['rows'] if r['stage']==stage and r['pipeline']==p) for p in depths]
        ax.plot(x,np.array(vals)/1e6,'o--',color=color,label=label)
    vals=[median([r['qps'] for r in rows if r['program']=='kv-none' and r['pipeline']==p]) for p in depths]
    ax.plot(x,np.array(vals)/1e6,'o-',color=blue,label='49b8d3d controls (3 runs/P)')
    ax.set_xticks(x,[str(p) for p in depths]);ax.set_xlabel('Pipeline depth P');grid(ax,'Million requests/s');ax.legend(frameon=False,fontsize=9)
    save(fig,'pipeline-stages','Pipeline stages: measured observations, not isolated attribution',
         'Historical O2/cursor/stack: 1 run/P, variation=N/A. Final controls: 3 runs/P (ranges in Pipeline figure).\nNo paired O0 SET/hotkey sample exists. Stage changes, cross-window noise and version changes cannot be added as causal gains.')
    rows=data['save']['rows'];fig,axes=plt.subplots(1,2,figsize=(12.5,5.8))
    axes[0].bar(x[:4],[r['write_seconds'] for r in rows],color=blue,label='Write phase')
    axes[0].bar(x[:4],[r['drain_seconds'] for r in rows],bottom=[r['write_seconds'] for r in rows],color=orange,label='Final drain')
    for i,r in enumerate(rows):axes[0].text(i,r['total_seconds']+6,f"{r['total_seconds']:.3f}s\n{r['triggered']}/{r['completed']}",ha='center',fontsize=8)
    axes[0].set_xticks(range(4),['1M','100k','10k','1k']);axes[0].set_xlabel('Writes between SAVE; labels=triggered/completed');axes[0].set_ylim(0,330);grid(axes[0],'Write + final drain (s)');axes[0].legend(frameon=False,fontsize=8)
    axes[1].bar([0,1],[1713.18,rows[-1]['total_seconds']],color=[gray,blue]);axes[1].set_xticks([0,1],['Historical sync\n1000 snapshots','Async + coalescing\n230 snapshots'])
    for i,v in enumerate([1713.18,rows[-1]['total_seconds']]):axes[1].text(i,v+35,f'{v:.3f}s',ha='center')
    axes[1].set_ylim(0,2000);grid(axes[1],'1k SAVE interval total time (s)')
    save(fig,'save','Nonblocking SAVE: retained Stage 2 million-record measurements',
         '1 round/frequency; variation=N/A. Python serial HSET; 1M unique 32B keys + 128B values; all restarts verified.\nBackground save time overlaps writes; do not stack it. 1k total -83.80% includes fewer snapshots, not a 6.17x disk-I/O speedup.')
    modes=['none','ebpf-capture-only','socket','ebpf'];labels=['none','capture-only','socket','ebpf']
    fig,axes=plt.subplots(2,2,figsize=(13,9))
    for line,concurrency in enumerate([1,8]):
        rs=data['replication']['c'+str(concurrency)]
        values=[[r['load']['qps'] for r in rs if r['mode']==m] for m in modes]
        measured(axes[line,0],values,labels,[gray,orange,green,blue]);grid(axes[line,0],f'c{concurrency}/P1 Primary QPS')
        for metric,offset,color in [('ack_to_observed_p50_ms',-.17,green),('ack_to_observed_p99_ms',.17,blue)]:
            vals=[median([r['visibility'][metric] for r in rs if r['mode']==m]) for m in ['socket','ebpf']]
            axes[line,1].bar(np.arange(2)+offset,vals,.3,color=color,label=metric.split('_')[3].upper())
            for p,v in zip(np.arange(2)+offset,vals):axes[line,1].text(p,v+.06,f'{v:.3f}',ha='center',fontsize=9)
        axes[line,1].set_xticks([0,1],['socket','ebpf']);grid(axes[line,1],f'c{concurrency} separate visibility (ms)');axes[line,1].legend(['P50','P99'],frameon=False)
        axes[line,1].set_ylim(0,7 if concurrency==1 else 3)
    save(fig,'replication','49b8d3d: c1 and c8 replication controls (separate groups)',
         '3 rounds/mode/concurrency; 100000 HSET; key32B/value128B; preloaded 50k + 50k new; all records verified.\nQPS dots=rounds, whiskers=min/max; v=range/median. Visibility: separate 1000 probes, serial c1 vs c8 chains; not under sustained write load.')
    fig,axes=plt.subplots(1,2,figsize=(13,5.8))
    for line,c in enumerate([1,8]):
        rs=data['replication']['c'+str(c)]
        for key,label,offset,color in [('primary','Primary',-.23,blue),('capture','Capture',0,orange),('replica','Replica',.23,green)]:
            vals=[]
            for mode in modes:
                group=[r for r in rs if r['mode']==mode]
                values=[r.get('replica_cpu_pct') if key=='replica' else r['load'][key+'_cpu_pct'] for r in group]
                vals.append(median(values) if all(v is not None and v>=0 for v in values) else float('nan'))
            axes[line].bar(np.arange(4)+offset,vals,.21,label=label,color=color)
        axes[line].set_xticks(range(4),labels);axes[line].set_title(f'c{c}/P1: median of 3 timed rounds');axes[line].set_ylim(0,112);grid(axes[line],'Process CPU % (100% = one core)');axes[line].legend(frameon=False,fontsize=8)
    save(fig,'replication-cpu','Replication CPU distribution: utilization is part of the result',
         'Primary/Capture sampled in timed write window; Replica surrounding RPC window (c8 uses READY/GO gate).\nMissing processes omitted. QPS variations (none/capture/socket/ebpf): c1=5.15/20.22/19.45/4.88%; c8=1.68/1.20/0.69/0.34%.')
    fig,axes=plt.subplots(1,2,figsize=(12,5.8))
    profile_cpu=[data['replica_profile'][k]['replica_cpu_pct'] for k in ['idle','load']]
    axes[0].bar([0,1],profile_cpu,color=[gray,blue]);axes[0].set_xticks([0,1],['Idle online','Sustained c8 load']);axes[0].set_ylim(0,85);grid(axes[0],'Replica CPU % (one observation each)')
    for i,v in enumerate(profile_cpu):axes[0].text(i,v+2,f'{v:.2f}%',ha='center')
    names=['allocator','strcmp','epoll_pwait'];shares=data['replica_profile']['load']['self_samples_pct'];axes[1].bar(range(3),[shares[k] for k in ['kvs_allocator_alloc','strcmp','epoll_pwait']],color=[blue,orange,gray]);axes[1].set_xticks(range(3),names);grid(axes[1],'Load perf Self samples (%)');axes[1].set_ylim(0,35)
    save(fig,'replica-profile','Replica CPU: direct idle/load observations after timed rounds',
         'Idle: sent/applied fixed at 28728000, yet CPU=55.13%. Stack: nty_schedule_run -> epoll_pwait (81.82% children).\nCPU bars: proc ticks, idle 9.27s / load 10.02s; load pidstat 8s=69.16%. Epoll Self=26.59% idle; profiles n=1; variation=N/A.')
    native=data['transfer']['medians']['native'];fig,ax=plt.subplots(figsize=(11,5.8))
    for mode,offset,color in [('sendfile',-.18,blue),('rdma',.18,orange)]:
        rs=[r for r in native if r['mode']==mode];vals=[r['mib_per_second'] for r in rs]
        ax.bar(np.arange(2)+offset,vals,.32,label=mode+' (3 runs)',color=color)
        for i,r in enumerate(rs):ax.text(i+offset,r['mib_per_second']+3,f"{r['mib_per_second']:.2f}\nv={r['variation']:.2f}%",ha='center',fontsize=8)
    ax.set_xticks([0,1],['135 -> 136','136 -> 135']);grid(ax,'Transfer-only throughput (MiB/s)');ax.set_ylim(0,185);ax.legend(frameon=False)
    factor=data['units']['mibps_to_gbps'];secondary=ax.secondary_yaxis('right',functions=(lambda v:v*factor,lambda v:v/factor));secondary.set_ylabel('Gb/s (decimal bits/s)')
    save(fig,'transfer','Stage 3.2 retained transfer controls: software SIW, not hardware RNIC',
         "168000052 B / 160.22 MiB; 1MiB chunks, window=1. Clock: after 'R' ready exchange to final 'A' ACK.\n3 runs/direction/mode; v=QPS-style throughput range. Setup, checksum, file persistence and snapshot loading are excluded.")
    ip=data['transfer']['medians']['iperf'];fig,axes=plt.subplots(1,2,figsize=(12,5.8))
    for mode,offset,color,label in [('iperf3',-.18,gray,'Default (3 x 30s)'),('iperf3-W128K30',.18,blue,'-w 131072 (1 x 30s)')]:
        rs=[r for r in ip if r['mode']==mode]
        axes[0].bar(np.arange(2)+offset,[r['receiver_gbps'] for r in rs],.32,label=label,color=color)
        for pos,r in zip(np.arange(2)+offset,rs):
            samples=[v['receiver_gbps'] for v in data['transfer']['rounds'] if v['mode']==mode and v['sender']==r['sender'] and not v.get('warmup')]
            label_var=f'v={var(samples):.2f}%' if len(samples)>1 else 'v=N/A'
            axes[0].text(pos,r['receiver_gbps']+.025,f"{r['receiver_gbps']:.3f}\n{label_var}",ha='center',fontsize=8)
        axes[1].bar(np.arange(2)+offset,[r['mean_rtt_us']/1000 for r in rs],.32,color=color)
    for ax in axes:ax.set_xticks([0,1],['135 -> 136','136 -> 135'])
    grid(axes[0],'iperf3 receiver throughput (Gb/s)');grid(axes[1],'iperf3 sender TCP mean RTT (ms)');axes[0].set_ylim(0,1.55);axes[0].legend(frameon=False,fontsize=8,loc='upper left')
    save(fig,'bufferbloat','TCP buffer diagnostic: same NIC path, different buffering behavior',
         'Default 30s controls: n=3/direction; socket-window diagnostic: n=1/direction, variation=N/A.\n-w 131072: RTT ~61-63ms -> ~1.1ms, receiver ~0.44-0.46 -> ~1.14-1.15Gb/s. Supports queueing/bufferbloat, not a hardware RDMA claim.')
    mem=data['memory'];rows=mem['same_round_trim_pair'];fig,axes=plt.subplots(1,2,figsize=(12.5,6))
    for mode,offset,color in [('system-malloctrim',-.18,blue),('pool',.18,orange)]:
        vals=[next(r['VmRSS_KiB']/1024 for r in rows if r['mode']==mode and r['state']==phase) for phase in ['start','peak','end']]
        axes[0].bar(np.arange(3)+offset,vals,.32,color=color,label=mode)
        for pos,v in zip(np.arange(3)+offset,vals):axes[0].text(pos,v*1.1,f'{v:.3f}',ha='center',fontsize=8)
    axes[0].set_xticks(range(3),['Start','Full + verified','Deleted + trim']);axes[0].set_yscale('log');axes[0].set_ylim(1.5,700);grid(axes[0],'VmRSS (MiB, log scale)');axes[0].legend(frameon=False,fontsize=8)
    bottom=0
    for label,count,color in [('Requested objects',mem['requested_bytes'],blue),('Size-class rounding',mem['fragmentation_bytes'],orange),('Startup blocks + free + metadata',mem['start_occupied_block_bytes']+mem['slab_free_block_bytes']+mem['slab_metadata_and_alignment_bytes'],gray)]:
        v=count/1048576;axes[1].bar(0,v,bottom=bottom,color=color,label=f'{label}: {v:.2f} MiB');bottom+=v
    axes[1].set_xticks([0],['Pool small-slab mapping']);grid(axes[1],'Mapped MiB (not RSS)');axes[1].legend(frameon=False,fontsize=8,loc='upper left');axes[1].set_ylim(0,350)
    save(fig,'memory','Comparable reclaim: retained Stage 3.1 million-record pair',
         '1 round/mode; variation=N/A. Each RSS state=median of five /proc samples. system ALLOCTRIM after absent checks.\nPool immediately munmaps empty slabs; startup has five 64KiB slabs (0.3125MiB). Rounding waste=81.18MiB; no 160B class added.')
    print('Generated 10 Stage 5 SVG figures from archived data only.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--final-data',type=Path,help='Stage 5 final and retained-stage measurements JSON')
    parser.add_argument('--preview-dir', type=Path, help='optional local PNG QA copies')
    parser.add_argument('--direct-data', type=Path, help='single direct eBPF rerun JSON with historical references')
    parser.add_argument('--data', type=Path, help='formal matrix JSON containing retune results')
    parser.add_argument('--figures-dir', type=Path, help='optional SVG destination')
    args = parser.parse_args()
    if args.final_data:
        if args.data or args.direct_data:parser.error('--final-data cannot be combined with another data mode')
        with tempfile.TemporaryDirectory(prefix='91kv-stage5-plot-') as cache:
            os.environ['MPLCONFIGDIR']=cache
            os.environ['XDG_CACHE_HOME']=cache
            plot_final(json.loads(args.final_data.read_text()),args.figures_dir or Path(__file__).resolve().parent/'figures',args.preview_dir)
        return
    if args.direct_data:
        if args.data:
            parser.error('--data and --direct-data are mutually exclusive')
        with tempfile.TemporaryDirectory(prefix='91kv-direct-plot-') as cache:
            os.environ['MPLCONFIGDIR'] = cache
            os.environ['XDG_CACHE_HOME'] = cache
            plot_direct(json.loads(args.direct_data.read_text()), args.figures_dir or Path(__file__).resolve().parent/'figures', args.preview_dir)
        return
    if args.data:
        with tempfile.TemporaryDirectory(prefix='91kv-formal-plot-') as cache:
            os.environ['MPLCONFIGDIR'] = cache
            os.environ['XDG_CACHE_HOME'] = cache
            plot_formal(json.loads(args.data.read_text()), args.figures_dir or Path(__file__).resolve().parent/'figures', args.preview_dir)
        return
    here = Path(__file__).resolve().parent
    data = json.loads((here / 'data/measurements.json').read_text())
    target = here / 'figures'
    target.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='91kv-offline-plot-') as cache:
        os.environ['MPLCONFIGDIR'] = cache
        os.environ['XDG_CACHE_HOME'] = cache
        import matplotlib
        matplotlib.use('Agg')
        import matplotlib.pyplot as plt
        from matplotlib.lines import Line2D
        from matplotlib.ticker import FuncFormatter
        plt.rcParams.update({'font.family': 'DejaVu Sans', 'font.size': 10,
                             'axes.spines.top': False, 'axes.spines.right': False,
                             'axes.titleweight': 'bold', 'axes.labelcolor': '#334155',
                             'text.color': '#172b43', 'axes.edgecolor': '#94a3b8',
                             'svg.hashsalt': '91kvstore-phase13', 'svg.fonttype': 'path',
                             'figure.facecolor': 'white', 'savefig.facecolor': 'white'})
        colors = ['#2468a2', '#d17b20', '#378369']
        markers = ['o', 'D', '^']
        legend = [Line2D([], [], color=colors[i], marker=markers[i], linestyle='', label=f'Round {i+1}') for i in range(3)]
        legend.append(Line2D([], [], color='#172b43', lw=2.5, label='Median'))

        def style(ax):
            ax.set_axisbelow(True)
            ax.grid(axis='y', color='#e2e8f0', lw=.8)
            ax.tick_params(axis='both', length=0, pad=8)

        def points(ax, groups, labels, decimals=2, log=False):
            for i, values in enumerate(groups):
                for k, value in enumerate(values):
                    ax.scatter(i + (k-1)*.13, value, color=colors[k], marker=markers[k], s=43, zorder=4)
                med = stats.median(values)
                ax.hlines(med, i-.28, i+.28, colors='#172b43', lw=2.4, zorder=3)
                shift = 1.25*med if log else med + .055*ax.get_ylim()[1]
                ax.text(i, shift, f'{med:,.{decimals}f}', ha='center', va='bottom', fontsize=10)
            ax.set_xticks(range(len(labels)), labels)
            ax.set_xlim(-.55, len(labels)-.45)
            style(ax)

        def save(fig, name, title, notes):
            fig.suptitle(title, x=.035, y=.98, ha='left', fontsize=15, fontweight='bold')
            fig.legend(handles=legend, loc='upper left', bbox_to_anchor=(.025,.91), ncol=4, frameon=False, fontsize=9)
            fig.text(.035,.055, notes, ha='left', va='bottom', fontsize=9, linespacing=1.45)
            fig.subplots_adjust(left=.13 if name == 'single-hget' else .08,
                                right=.97, top=.76, bottom=.25, wspace=.37, hspace=.65)
            dest = target / (name+'.svg')
            fig.savefig(dest, format='svg', metadata={'Date': None, 'Title': title, 'Description': notes, 'Creator': '91kvstore offline plot_results.py'})
            # Accessibility text stays searchable even though fonts are SVG paths.
            text = dest.read_text()
            start = text.index('>', text.index('<svg')) + 1
            from html import escape
            text = text[:start] + '\n<title>' + escape(title) + '</title>\n<desc>' + escape(notes) + '</desc>' + text[start:]
            text = "\n".join(line.rstrip() for line in text.splitlines()) + "\n"
            dest.write_text(text)
            ET.parse(dest)
            if args.preview_dir:
                args.preview_dir.mkdir(parents=True, exist_ok=True)
                fig.savefig(args.preview_dir/(name+'.png'), dpi=140)
            plt.close(fig)

        initial = data['single_initial']
        fig, axes = plt.subplots(1,2,figsize=(12,5.3), gridspec_kw={'width_ratios':[3,1.3]})
        axes[0].set_yscale('log'); axes[0].set_ylim(1000,1000000)
        axes[0].set_yticks([1000,10000,100000,1000000], ['1,000','10,000','100,000','1,000,000'])
        points(axes[0], [[r['tool_completed_qps'] for r in initial if r['case']==c] for c in ['g1','g16','g64','g8']], ['c1 / P1','c1 / P16','c1 / P64','c8 / P1'], 0, True)
        axes[0].set_title('Initial matrix: 128,000 requests/round', fontsize=11)
        axes[0].set_ylabel('Tool-completed requests/s (log10 axis)')
        axes[1].set_ylim(0,570000)
        points(axes[1], [[r['tool_completed_qps'] for r in data['single_p16_followup']]], ['c1 / P16\n12,043,648 requests'], 2)
        axes[1].set_title('Separate ~25 s follow-up', fontsize=11)
        axes[1].yaxis.set_major_formatter(FuncFormatter(lambda v,p:f'{v/1000:.0f}k'))
        axes[1].set_ylabel('Tool-completed requests/s (linear)')
        save(fig,'single-hget','Single VM: HGET connection / pipeline matrix',
             'Loopback, one 128 B Hash hot key; redis-benchmark 7.0.15; default unoptimized userspace.\nDots = 3 raw rounds; line = median. Pre/post value checks, NOT per-response validation. P64 retained in full.')

        modes=['system','pool','jemalloc']; alloc=data['allocator_million']
        fig,ax=plt.subplots(figsize=(10,5.2)); ax.set_ylim(0,235)
        points(ax,[[r['four_phases_s'] for r in alloc if r['mode']==m] for m in modes],modes,3)
        ax.set_ylabel('Four-stage elapsed time (s)')
        save(fig,'allocator-time','Allocator: complete four-stage elapsed time',
             '1,000,000 distinct keys; key 32 B / value 128 B; mixed engines; serial TCP; 3 rounds/mode.\nInsert + verify + delete + verify-absent. Includes data generation, client, protocol and network; NOT allocator API time.')

        fig,ax=plt.subplots(figsize=(11,5.5)); ax.set_ylim(0,315)
        state_colors=['#a9c6df','#2468a2','#d17b20']; states=['start','full','deleted']
        for i,m in enumerate(modes):
            for k,state in enumerate(states):
                vals=[r[state+'_RSS_KiB']/1024 for r in alloc if r['mode']==m]; x=i+(k-1)*.25; med=stats.median(vals)
                ax.bar(x,med,.22,color=state_colors[k],zorder=2)
                for n,v in enumerate(vals):ax.scatter(x+(n-1)*.035,v,color='#172b43',s=10,zorder=3)
                ax.text(x,med+5,f'{med:.1f}',ha='center',va='bottom',fontsize=9)
        ax.set_xticks(range(3),modes);ax.set_ylabel('Resident memory: VmRSS (MiB)');style(ax)
        legends=[Line2D([],[],lw=8,color=state_colors[k],label=label) for k,label in enumerate(['Start','Full + verified','Deleted + verified'])]
        original=legend;legend=legends
        save(fig,'allocator-rss','Allocator: RSS at three measured states',
             'Same million-key workload. Bars = medians of 3 rounds; small dots = raw round states (5-read state medians).\nRSS snapshots are not VmHWM or allocated bytes. Retained RSS is not proof of a leak; VmSize is tabulated separately.')
        legend=original

        pers=data['persistence_selected'];fig,axes=plt.subplots(2,2,figsize=(12,8))
        panels=[('write_seconds','Write 100,000 records',24),('save_seconds','SAVE (Snapshot only)',.22),('restart_to_port_seconds','Restart until port connects',.52),('verification_seconds','Separate full recovery verification',4)]
        for ax,(field,title,upper) in zip(axes.flat,panels):
            ax.set_ylim(0,upper); ax.set_title(title,fontsize=11);ax.set_ylabel('Elapsed time (s)')
            groups=[[r[field] for r in pers if r['mode']==m and r[field] is not None] for m in ['snapshot','aof']]
            if field=='save_seconds':
                points(ax,[groups[0]],['Snapshot'],3);ax.set_xlim(-.55,1.5);ax.set_xticks([0,1],['Snapshot','AOF: not invoked']);ax.text(1,.035,'N/A',ha='center')
            else:points(ax,groups,['Snapshot','AOF'],3)
        save(fig,'persistence','Persistence: separate timings, different durability contracts',
             '100,000 mixed-engine records; variable values; 3 rounds/mode. Snapshot writes memory, then SAVE; AOF replies after append + sync.\nRestart stops at connectable port (~0.1 s polling); full verification is separate. SIGKILL recovery is NOT a power-loss test.')

        dual=[r for r in data['dual'] if r['included_in_formal_statistics']]
        fig,ax=plt.subplots(figsize=(10,5.5));ax.set_ylim(0,5600)
        points(ax,[[r['write-batch1_accepted_per_second'] for r in dual],[r['write-batch2_accepted_per_second'] for r in dual]],['First 50k\nReplica not started','Second 50k\nONLINE + first-batch verified'],2)
        ax.set_ylabel('Accepted writes / client-stage second')
        save(fig,'dual-write','Two VMs: whole client-stage write throughput',
             'Existing serial Python client on A; 4 engines; formal key 31 B / value 52-148 B; default unoptimized userspace.\nConditions differ; no AOF-only control, so no eBPF speedup claim. R3 cleanup orchestration was nonzero; data checks passed.')
        print('Generated 5 static SVGs; no test or service executed; matplotlib',matplotlib.__version__)


if __name__=='__main__':
    main()
