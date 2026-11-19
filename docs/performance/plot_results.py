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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--preview-dir', type=Path, help='optional local PNG QA copies')
    parser.add_argument('--data', type=Path, help='formal matrix JSON containing retune results')
    parser.add_argument('--figures-dir', type=Path, help='optional SVG destination')
    args = parser.parse_args()
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
