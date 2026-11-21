const fs=require('fs'), path=require('path');
const directory=path.resolve(process.argv[2]||path.join(__dirname,'results/p4'));
const read=name=>JSON.parse(fs.readFileSync(path.join(directory,name),'utf8').replace(/^\uFEFF/,''));
const ranges=values=>({min:Math.min(...values),max:Math.max(...values)});
const resourceSummary=(rows,seconds)=>({
    peakThreads:Math.max(...rows.map(x=>x.threads||0)),
    peakWorkingSetBytes:Math.max(...rows.map(x=>x.working_set_bytes||0)),
    peakPrivateBytes:Math.max(...rows.map(x=>x.private_bytes||0)),
    cpuCorePercent:(rows.at(-1).cpu_seconds-rows[0].cpu_seconds)*100/seconds
});
const controlled=[];
for(const concurrency of [10,20,40]) {
    const runs=[1,2,3].map(run=>{
        const file=`controlled-${concurrency}-${run}.json`, x=read(file);
        if(x.inserted_and_notified!==300||x.database_rows!==300||!x.statistics_exact||x.timed_out) throw Error(`Invalid controlled run ${file}`);
        const g=x.metrics.gauges, s=x.metrics.samples, c=x.metrics.counters;
        if(g['crawler.db_pending'].current!==0||g['crawler.metadata_inflight'].current!==0
            ||g['crawler.db_pending_bytes'].peak>64*1024*1024) throw Error(`Capacity did not settle: ${file}`);
        return {file,elapsedSeconds:x.elapsed_seconds,insertedPerMinute:x.inserted_per_minute,
            actualMetadataPeak:g['crawler.metadata_inflight'].peak,
            metadataSuccessRate:c['crawler.metadata_succeeded']/c['crawler.metadata_attempts'],
            endToEndP95Ms:s['crawler.end_to_end_ms']?.p95??null,
            metadataQueueP95Ms:s['crawler.metadata_queue_wait_ms'].p95,
            metadataFetchP95Ms:s['crawler.metadata_fetch_ms'].p95,
            taskP95Ms:s['crawler.task_latency_ms'].p95,
            queueItemsPeak:g['crawler.metadata_queue_items'].peak,
            pendingItemsPeak:g['crawler.db_pending'].peak,pendingEstimatedBytesPeak:g['crawler.db_pending_bytes'].peak,
            notificationItemsPeak:g['crawler.notification_pending'].peak,
            ...resourceSummary(x.resources,x.elapsed_seconds),networkBandwidth:null};
    });
    if(runs.some(x=>x.endToEndP95Ms===null)) throw Error('Rebuild/repeat controlled runs with end-to-end timing');
    controlled.push({concurrency,runs,
        insertedPerMinute:ranges(runs.map(x=>x.insertedPerMinute)),
        endToEndP95Ms:ranges(runs.map(x=>x.endToEndP95Ms)),
        actualMetadataPeak:ranges(runs.map(x=>x.actualMetadataPeak)),
        peakThreads:ranges(runs.map(x=>x.peakThreads)),
        peakWorkingSetBytes:ranges(runs.map(x=>x.peakWorkingSetBytes)),
        cpuCorePercent:ranges(runs.map(x=>x.cpuCorePercent))});
}
const live=[];
for(const variant of ['p3-10','p4-10','p4-20','p4-40']) for(const run of [1,2,3]) {
    const prefix=`live-${variant}-${run}`, x=read(prefix+'.json');
    const start=x.start_snapshot.counters||{}, end=x.end_snapshot.counters||{};
    const delta=key=>(end[key]||0)-(start[key]||0);
    const rows=fs.readFileSync(path.join(directory,prefix+'.resources.jsonl'),'utf8').trim().split(/\r?\n/).filter(Boolean).map(JSON.parse);
    live.push({file:prefix+'.json',variant,seconds:x.measurement_seconds,dhtNodes:x.dht_nodes_at_end,
        announcements:delta('crawler.announcements'),attempts:delta('crawler.metadata_attempts'),successes:delta('crawler.metadata_succeeded'),
        inserted:delta('crawler.index_inserted'),networkSuccessRate:delta('crawler.metadata_attempts')?delta('crawler.metadata_succeeded')/delta('crawler.metadata_attempts'):null,
        ...resourceSummary(rows,(rows.at(-1).elapsed_ms-rows[0].elapsed_ms)/1000),networkBandwidth:null});
}
const waiters=[10,20,40].map(count=>{
    const x=read(`waiters-${count}.json`), r=x.resources;
    if(x.waiter_completions!==count) throw Error('Waiter callback accounting failed');
    return {file:`waiters-${count}.json`,count,callbacks:x.waiter_completions,baselineThreads:r[0].threads,
        finalThreads:r.at(-1).threads,...resourceSummary(r,(r.at(-1).elapsed_ms-r[0].elapsed_ms)/1000)};
});
const summary={phase:'P4',controlled,live,waiters,defaultConcurrency:10,tested64:false,
    networkBandwidthMeasured:false,networkConclusion:live.some(x=>x.attempts)?'inspect sample volume':'No metadata requests; live throughput/success/latency cannot be compared',
    resourcesScope:'Probe process only; no searchd, system memory, or wire bandwidth; CPU percent is percent of one logical core',
    caveats:['Synthetic successful metadata does not represent real network reliability or bandwidth.',
        'P3 baseline has external resource sampling; P4 live also has internal sampling. Do not attribute resource differences to concurrency alone.',
        '64 was not tested because 20/40 show no stable delivered-throughput benefit; fixed result reservations limit simultaneous unfinished requests.',
        'Watcher profiling invokes the library directly and does not pass through crawler admission. It measures thread shape, not production crawler capacity.']};
fs.writeFileSync(path.join(directory,'summary.json'),JSON.stringify(summary,null,2)+'\n');
console.log(JSON.stringify({controlled:controlled.map(({concurrency,insertedPerMinute,endToEndP95Ms,actualMetadataPeak})=>({concurrency,insertedPerMinute,endToEndP95Ms,actualMetadataPeak})),liveAttempts:live.reduce((n,x)=>n+x.attempts,0),waiters},null,2));
